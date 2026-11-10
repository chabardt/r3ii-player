/* r3srv — music server for the R3's library over Wi-Fi (read-only, LAN).
 *
 *   UPnP/DLNA MediaServer: shows up by itself in VLC (Local Network > Universal Plug'n'Play)
 *     SSDP on 239.255.255.250:1900, /desc.xml, ContentDirectory Browse -> DIDL-Lite.
 *   HTTP: /            browsable page            /m3u/<folder>  playlist of the folder (recursive)
 *         /f/<file>    the file, with Range (seeking)
 * One poll() loop, no threads. Folders are read when browsed (no scan, no database).
 *
 *   Radio: /radio (page), /radio.m3u?genre=Metal[&sub=Power metal] — an endless Explorer-style mix
 *         (an album, then a similar artist's album...), and "R3 Radio" folders over UPnP.
 *         Genres come from r3player's library index (/usr/data/r3_mix.idx) and <ROOT>/Genres.txt.
 *
 *   r3srv [-r ROOT] [-p PORT] [-n NAME] [-m MAXSTREAMS] [-u UUIDFILE] [-s STATUSFILE]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "explore.h"
#include "smartmix.h"

static const char *ROOT = "/usr/data/mnt/sd_0", *NAME = "HiBy R3", *UUIDF = "/usr/data/r3srv_uuid", *STATF = NULL;
static int PORT = 8200, MAXSTREAMS = 8;
static char UUID[48];
static const char *TOPS[] = { "Music", "Others", NULL };   /* what we publish under ROOT */

/* ---------------------------------------------------------------- utils */
static void die(const char *m) { perror(m); exit(1); }
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

typedef struct { char *p; size_t n, cap; } buf_t;
static void bput(buf_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 4096; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void bprintf(buf_t *b, const char *fmt, ...) {
    char tmp[2048]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { bput(b, tmp, n); return; }
    char *big = malloc(n + 1); va_start(ap, fmt); vsnprintf(big, n + 1, fmt, ap); va_end(ap); bput(b, big, n); free(big);
}
static void bxml(buf_t *b, const char *s) {           /* XML-escape */
    for (; *s; s++) switch (*s) {
        case '&': bput(b, "&amp;", 5); break; case '<': bput(b, "&lt;", 4); break;
        case '>': bput(b, "&gt;", 4); break; case '"': bput(b, "&quot;", 6); break;
        case '\'': bput(b, "&apos;", 6); break; default: bput(b, s, 1); }
}
static void burl(buf_t *b, const char *s) {           /* percent-encode a path (keep '/') */
    for (; *s; s++) { unsigned char c = *s;
        if (isalnum(c) || strchr("/-_.~", c)) bput(b, (char *)&c, 1);
        else { char e[4]; snprintf(e, sizeof e, "%%%02X", c); bput(b, e, 3); } }
}
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else if (*s == '+') *o++ = ' ';
        else *o++ = *s; }
    *o = 0;
}
/* a relative library path is safe if it has no "..", no leading '/', and starts with a published top dir */
static int safe_rel(const char *rel) {
    if (!rel || rel[0] == '/' ) return 0;
    for (const char *p = rel; *p; ) {
        const char *s = strchr(p, '/'); size_t n = s ? (size_t)(s - p) : strlen(p);
        if ((n == 2 && !strncmp(p, "..", 2)) || (n == 1 && p[0] == '.')) return 0;
        p += n; if (*p == '/') p++; }
    if (!rel[0]) return 1;
    for (int i = 0; TOPS[i]; i++) { size_t n = strlen(TOPS[i]);
        if (!strncmp(rel, TOPS[i], n) && (rel[n] == 0 || rel[n] == '/')) return 1; }
    return 0;
}
static const char *ext_of(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }
static const char *audio_mime(const char *n) {
    const char *e = ext_of(n);
    if (!strcasecmp(e, "mp3") || !strcasecmp(e, "mpga")) return "audio/mpeg";
    if (!strcasecmp(e, "flac")) return "audio/flac";
    if (!strcasecmp(e, "wav")) return "audio/wav";
    if (!strcasecmp(e, "m4a") || !strcasecmp(e, "aac") || !strcasecmp(e, "mp4")) return "audio/mp4";
    if (!strcasecmp(e, "ogg") || !strcasecmp(e, "opus")) return "audio/ogg";
    return NULL;
}
static const char *any_mime(const char *n) {
    const char *m = audio_mime(n); if (m) return m;
    const char *e = ext_of(n);
    if (!strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg")) return "image/jpeg";
    if (!strcasecmp(e, "png")) return "image/png";
    return NULL;
}

/* ---------------------------------------------------------------- directory listing */
typedef struct { char name[256]; int dir; } ent_t;
static int ent_cmp(const void *a, const void *b) {
    const ent_t *x = a, *y = b; if (x->dir != y->dir) return y->dir - x->dir; return strcasecmp(x->name, y->name); }
/* entries of ROOT/rel (folders + audio files), sorted; *n = count. "" = the published tops */
static ent_t *list_dir(const char *rel, int *n) {
    *n = 0; ent_t *v = NULL; int cap = 0;
    if (!rel[0]) {
        for (int i = 0; TOPS[i]; i++) { char p[1024]; struct stat st; snprintf(p, sizeof p, "%s/%s", ROOT, TOPS[i]);
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { v = realloc(v, sizeof(ent_t) * (*n + 1));
                snprintf(v[*n].name, sizeof v[0].name, "%s", TOPS[i]); v[*n].dir = 1; (*n)++; } }
        return v; }
    char p[1024]; snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    DIR *d = opendir(p); if (!d) return NULL; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) { char f[1600]; struct stat st; snprintf(f, sizeof f, "%s/%s", p, e->d_name);
            dir = stat(f, &st) == 0 && S_ISDIR(st.st_mode); }
        if (!dir && !audio_mime(e->d_name)) continue;
        if (*n == cap) { cap = cap ? cap * 2 : 64; v = realloc(v, sizeof(ent_t) * cap); }
        snprintf(v[*n].name, sizeof v[0].name, "%s", e->d_name); v[*n].dir = dir; (*n)++; }
    closedir(d);
    qsort(v, *n, sizeof(ent_t), ent_cmp);
    return v;
}
static int find_cover(const char *rel, char *out, size_t cap) {
    static const char *names[] = { "cover.jpg", "Cover.jpg", "folder.jpg", "Folder.jpg", "front.jpg", "cover.png", "folder.png", NULL };
    for (int i = 0; names[i]; i++) { char p[1600]; struct stat st;
        snprintf(p, sizeof p, "%s/%s/%s", ROOT, rel, names[i]);
        if (stat(p, &st) == 0) { snprintf(out, cap, "%s/%s", rel, names[i]); return 1; } }
    return 0;
}
/* ---------------------------------------------------------------- embedded album art
 * Find the picture inside a track's tags and return where it sits in the file, so it is
 * served straight from the card (sendfile), never decoded or copied.
 * FLAC: PICTURE metadata block. MP3: ID3v2.2/2.3/2.4 PIC/APIC frame. M4A: moov/udta/meta/ilst/covr/data.
 * Front cover (picture type 3) wins over other pictures. */
static uint32_t rd32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t syncsafe(const unsigned char *p) { return ((uint32_t)(p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) | ((p[2] & 0x7f) << 7) | (p[3] & 0x7f); }
static const char *img_mime(int fd, off_t off) {
    unsigned char m[4]; if (pread(fd, m, 4, off) != 4) return NULL;
    if (m[0] == 0xFF && m[1] == 0xD8) return "image/jpeg";
    if (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') return "image/png";
    return NULL;
}
static int art_flac(int fd, off_t fsz, off_t *off, off_t *len) {
    off_t p = 4; int found = 0; unsigned char h[4];
    for (int blocks = 0; blocks < 256 && p + 4 <= fsz && pread(fd, h, 4, p) == 4; blocks++) {
        int last = h[0] & 0x80, type = h[0] & 0x7f; off_t bl = ((off_t)h[1] << 16) | (h[2] << 8) | h[3];
        if (type == 6 && bl >= 32) {
            unsigned char b[8]; off_t q = p + 4;
            if (pread(fd, b, 8, q) != 8) break; uint32_t ptype = rd32(b), ml = rd32(b + 4); q += 8 + ml;
            if (pread(fd, b, 4, q) != 4) break; q += 4 + rd32(b) + 16;          /* description, w/h/depth/colours */
            if (pread(fd, b, 4, q) != 4) break; off_t dl = rd32(b); q += 4;
            if (dl > 0 && q + dl <= p + 4 + bl && (!found || ptype == 3)) { *off = q; *len = dl; found = 1 + (ptype == 3); }
            if (found == 2) return 1; }
        p += 4 + bl; if (last) break; }
    return found > 0;
}
static int art_id3(int fd, off_t fsz, off_t *off, off_t *len) {
    unsigned char h[10]; if (pread(fd, h, 10, 0) != 10) return 0;
    int ver = h[3]; if (ver < 2 || ver > 4 || (h[5] & 0x80)) return 0;          /* unsynchronised tags: skip */
    off_t end = 10 + (off_t)syncsafe(h + 6), p = 10; if (end > fsz) end = fsz; int found = 0;
    if (ver >= 3 && (h[5] & 0x40)) { unsigned char e[4]; if (pread(fd, e, 4, p) != 4) return 0;
        p += ver == 4 ? syncsafe(e) : rd32(e) + 4; }
    while (p + 10 <= end) {
        unsigned char f[10]; int hl = ver == 2 ? 6 : 10;
        if (pread(fd, f, hl, p) != hl || !f[0]) break;                          /* padding */
        off_t fl = ver == 2 ? (((off_t)f[3] << 16) | (f[4] << 8) | f[5]) : ver == 4 ? syncsafe(f + 4) : rd32(f + 4);
        off_t body = p + hl; if (fl <= 0 || body + fl > end) break;
        int pic = ver == 2 ? !memcmp(f, "PIC", 3) : !memcmp(f, "APIC", 4);
        int plain = ver == 2 || !(f[9] & 0x0f);                                  /* no compression / per-frame unsync */
        if (pic && plain) {
            unsigned char b[512]; int n = (int)pread(fd, b, fl < (off_t)sizeof b ? (size_t)fl : sizeof b, body), i;
            if (n > 4) {
                int enc = b[0]; i = 1;
                if (ver == 2) i += 3; else { while (i < n && b[i]) i++; i++; }       /* image format / MIME */
                int ptype = i < n ? b[i] : 0; i++;
                if (enc == 1 || enc == 2) { while (i + 1 < n && (b[i] || b[i + 1])) i += 2; i += 2; }   /* UTF-16 description */
                else { while (i < n && b[i]) i++; i++; }
                if (i < n && (!found || ptype == 3)) { *off = body + i; *len = fl - i; found = 1 + (ptype == 3); }
                if (found == 2) return 1; } }
        p = body + fl; }
    return found > 0;
}
/* MP4 atoms: find child `name` inside [s,e) */
static int mp4_child(int fd, off_t s, off_t e, const char *name, off_t *cs, off_t *ce) {
    unsigned char h[16];
    for (int n = 0; n < 4096 && s + 8 <= e && pread(fd, h, 16, s) >= 8; n++) {
        off_t sz = rd32(h), hl = 8;
        if (sz == 1) { sz = ((off_t)rd32(h + 8) << 32) | rd32(h + 12); hl = 16; } else if (sz == 0) sz = e - s;
        if (sz < hl || s + sz > e) return 0;
        if (!memcmp(h + 4, name, 4)) { *cs = s + hl; *ce = s + sz; return 1; }
        s += sz; }
    return 0;
}
static int art_mp4(int fd, off_t fsz, off_t *off, off_t *len) {
    off_t a, b, c, d;
    if (!mp4_child(fd, 0, fsz, "moov", &a, &b) || !mp4_child(fd, a, b, "udta", &a, &b) ||
        !mp4_child(fd, a, b, "meta", &a, &b)) return 0;
    a += 4;                                                                     /* meta is a full box */
    if (!mp4_child(fd, a, b, "ilst", &a, &b) || !mp4_child(fd, a, b, "covr", &a, &b) ||
        !mp4_child(fd, a, b, "data", &c, &d) || d - c <= 8) return 0;
    *off = c + 8; *len = d - c - 8; return 1;                                   /* type + locale */
}
/* embedded picture of a track: 1 + region and MIME */
static int art_embedded(const char *rel, off_t *off, off_t *len, const char **mime) {
    char p[1600]; snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    struct stat st; unsigned char h[12]; int ok = 0;
    if (fstat(fd, &st) == 0 && pread(fd, h, 12, 0) == 12) {
        if (!memcmp(h, "fLaC", 4)) ok = art_flac(fd, st.st_size, off, len);
        else if (!memcmp(h, "ID3", 3)) ok = art_id3(fd, st.st_size, off, len);
        else if (!memcmp(h + 4, "ftyp", 4)) ok = art_mp4(fd, st.st_size, off, len);
        if (ok && *len > 16 * 1024 * 1024) ok = 0;
        if (ok) { *mime = img_mime(fd, *off); ok = *mime != NULL; } }
    close(fd); return ok;
}
/* album art for a folder or a track, as a server path ("f/<cover file>" or "art/<track>"); 0 = none */
static int art_for(const char *rel, int is_dir, char *out, size_t cap) {
    char dir[1024], cov[1200]; off_t o, l; const char *m;
    if (!is_dir && art_embedded(rel, &o, &l, &m)) { snprintf(out, cap, "art/%s", rel); return 1; }
    snprintf(dir, sizeof dir, "%s", rel);
    if (!is_dir) { char *s = strrchr(dir, '/'); if (s) *s = 0; else return 0; }
    if (!is_dir) { if (find_cover(dir, cov, sizeof cov)) { snprintf(out, cap, "f/%s", cov); return 1; } return 0; }
    /* folder: one pass over it (SD card lookups are slow): a cover file, else the first of
     * up to 3 tracks that carries a picture */
    static const char *names[] = { "cover.jpg", "folder.jpg", "front.jpg", "cover.png", "folder.png", NULL };
    char p[1600], tracks[3][256]; int nt = 0; snprintf(p, sizeof p, "%s/%s", ROOT, dir);
    DIR *d = opendir(p); if (!d) return 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        for (int i = 0; names[i]; i++) if (!strcasecmp(e->d_name, names[i])) {
            snprintf(out, cap, "f/%s/%s", dir, e->d_name); closedir(d); return 1; }
        if (nt < 3 && e->d_type != DT_DIR && audio_mime(e->d_name)) snprintf(tracks[nt++], sizeof tracks[0], "%s", e->d_name); }
    closedir(d);
    for (int i = 0; i < nt; i++) { char t[1600]; snprintf(t, sizeof t, "%s/%s", dir, tracks[i]);
        if (art_embedded(t, &o, &l, &m)) { snprintf(out, cap, "art/%s", t); return 1; } }
    return 0;
}
#define ART_RICH 100      /* listings up to this size get per-entry art; bigger ones stay fast */
static void display_name(const char *fname, int dir, char *out, size_t cap) {
    snprintf(out, cap, "%s", fname);
    if (!dir) { char *d = strrchr(out, '.'); if (d) *d = 0; }
}

/* ---------------------------------------------------------------- connections */
enum { C_READ, C_WRITE, C_FILE };
typedef struct {
    int fd, state, is_stream;
    char req[16384]; size_t rlen;
    buf_t out; size_t off;           /* response head/body being written */
    int ffd; off_t fpos, fend;       /* file streaming */
    char host[128];                  /* our address as the client reached us: "ip:port" */
    long long last;
} conn_t;
#define MAXC 32
static conn_t C[MAXC];
static int nstreams(void) { int n = 0; for (int i = 0; i < MAXC; i++) if (C[i].fd > 0 && C[i].is_stream) n++; return n; }
static void status_write(void) {
    if (!STATF) return; FILE *f = fopen(STATF, "w"); if (!f) return;
    fprintf(f, "streams=%d\nmax=%d\nport=%d\n", nstreams(), MAXSTREAMS, PORT); fclose(f);
}
static void conn_close(conn_t *c) {
    int was = c->is_stream;
    if (c->fd > 0) close(c->fd);
    if (c->ffd > 0) close(c->ffd);
    free(c->out.p); memset(c, 0, sizeof *c);
    if (was) status_write();
}
static void respond(conn_t *c, int code, const char *ctype, const char *body, size_t blen, const char *extra) {
    const char *msg = code == 200 ? "OK" : code == 206 ? "Partial Content" : code == 404 ? "Not Found" :
                      code == 400 ? "Bad Request" : code == 503 ? "Service Unavailable" : code == 416 ? "Range Not Satisfiable" : "Error";
    bprintf(&c->out, "HTTP/1.1 %d %s\r\nServer: Linux/3.x UPnP/1.0 r3srv/1.0\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n%s\r\n", code, msg, ctype, blen, extra ? extra : "");
    if (body) bput(&c->out, body, blen);
    c->state = C_WRITE;
}
static void respond_buf(conn_t *c, int code, const char *ctype, buf_t *b) {
    respond(c, code, ctype, b->p ? b->p : "", b->n, NULL); free(b->p); }

/* ---------------------------------------------------------------- HTTP pages */
static void page_browse(conn_t *c, const char *rel) {
    int n; ent_t *v = list_dir(rel, &n);
    if (rel[0] && !v) { respond(c, 404, "text/plain", "not found\n", 10, NULL); return; }
    buf_t b = { 0 };
    bprintf(&b, "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                "<title>"); bxml(&b, NAME); bprintf(&b, "</title><style>body{font:16px sans-serif;background:#0a0a0a;color:#f4e4d0;margin:16px}"
                "a{color:#e8731c;text-decoration:none}li{margin:6px 0}.m{color:#8a6a4a;font-size:13px}</style>"
                "<h2>"); bxml(&b, NAME); bprintf(&b, "</h2><p>/"); bxml(&b, rel);
    if (rel[0]) { bprintf(&b, " &nbsp; <a href='/m3u/"); burl(&b, rel); bprintf(&b, "'>&#9654; play all (m3u for VLC)</a>"); }
    if (!rel[0]) bprintf(&b, " &nbsp; <a href='/radio'>&#128251; radio</a>");
    bprintf(&b, "</p><ul>");
    if (rel[0]) { char up[1024]; snprintf(up, sizeof up, "%s", rel); char *s = strrchr(up, '/'); if (s) *s = 0; else up[0] = 0;
        bprintf(&b, "<li><a href='/browse/"); burl(&b, up); bprintf(&b, "'>..</a>"); }
    for (int i = 0; i < n; i++) {
        char sub[1600], dn[300]; snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", v[i].name);
        display_name(v[i].name, v[i].dir, dn, sizeof dn);
        bprintf(&b, "<li><a href='/%s/", v[i].dir ? "browse" : "f"); burl(&b, sub); bprintf(&b, "'>%s ", v[i].dir ? "&#128193;" : "&#9835;");
        bxml(&b, dn); bprintf(&b, "</a>");
        if (v[i].dir) { bprintf(&b, " <a class=m href='/m3u/"); burl(&b, sub); bprintf(&b, "'>m3u</a>"); } }
    bprintf(&b, "</ul><p class=m>In VLC: Media &gt; Open Network Stream, paste an m3u link; or Local Network &gt; Universal Plug'n'Play.</p>");
    free(v); respond_buf(c, 200, "text/html; charset=utf-8", &b);
}
static void m3u_walk(buf_t *b, const char *host, const char *rel, int depth, int *count) {
    if (depth > 6 || *count > 20000) return;
    int n; ent_t *v = list_dir(rel, &n); if (!v) return;
    for (int i = 0; i < n; i++) { char sub[1600]; snprintf(sub, sizeof sub, "%s/%s", rel, v[i].name);
        if (v[i].dir) m3u_walk(b, host, sub, depth + 1, count);
        else { char dn[300]; display_name(v[i].name, 0, dn, sizeof dn);
            bprintf(b, "#EXTINF:-1,%s\nhttp://%s/f/", dn, host); burl(b, sub); bput(b, "\n", 1); (*count)++; } }
    free(v);
}
static void page_m3u(conn_t *c, const char *rel) {
    buf_t b = { 0 }; int count = 0; bprintf(&b, "#EXTM3U\n");
    m3u_walk(&b, c->host, rel, 0, &count);
    respond_buf(c, 200, "audio/x-mpegurl; charset=utf-8", &b);
}
static void serve_file(conn_t *c, const char *rel, const char *range, int head) {
    char p[1600]; snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    const char *mime = any_mime(p); struct stat st;
    if (!mime || stat(p, &st) != 0 || !S_ISREG(st.st_mode)) { respond(c, 404, "text/plain", "not found\n", 10, NULL); return; }
    int is_audio = audio_mime(p) != NULL;
    if (is_audio && !head && nstreams() >= MAXSTREAMS) {
        respond(c, 503, "text/plain", "R3: too many listeners\n", 23, "Retry-After: 30\r\n"); return; }
    off_t start = 0, end = st.st_size - 1; int partial = 0;
    if (range && !strncasecmp(range, "bytes=", 6)) {
        long long a = -1, z = -1; const char *r = range + 6;
        if (*r == '-') { z = atoll(r + 1); a = st.st_size - z; z = st.st_size - 1; }
        else { a = atoll(r); const char *dash = strchr(r, '-'); z = (dash && dash[1] && isdigit((unsigned char)dash[1])) ? atoll(dash + 1) : st.st_size - 1; }
        if (a < 0) a = 0; if (z >= st.st_size) z = st.st_size - 1;
        if (a > z) { char ex[64]; snprintf(ex, sizeof ex, "Content-Range: bytes */%lld\r\n", (long long)st.st_size);
            respond(c, 416, "text/plain", "", 0, ex); return; }
        start = a; end = z; partial = 1; }
    char crange[96] = "", extra[512];
    if (partial) snprintf(crange, sizeof crange, "Content-Range: bytes %lld-%lld/%lld\r\n",
                          (long long)start, (long long)end, (long long)st.st_size);
    snprintf(extra, sizeof extra, "Accept-Ranges: bytes\r\n%s"
             "transferMode.dlna.org: Streaming\r\ncontentFeatures.dlna.org: DLNA.ORG_OP=01;DLNA.ORG_CI=0\r\n", crange);
    respond(c, partial ? 206 : 200, mime, NULL, (size_t)(end - start + 1), extra);
    if (head) return;
    c->ffd = open(p, O_RDONLY); if (c->ffd < 0) { conn_close(c); return; }
    c->fpos = start; c->fend = end + 1; c->is_stream = is_audio;
    if (is_audio) status_write();
}

/* /art/<track>: the picture embedded in the track, straight from the file */
static void serve_art(conn_t *c, const char *rel, int head) {
    off_t off, len; const char *mime;
    if (!audio_mime(rel) || !art_embedded(rel, &off, &len, &mime)) { respond(c, 404, "text/plain", "no picture\n", 11, NULL); return; }
    respond(c, 200, mime, NULL, (size_t)len, "Cache-Control: max-age=86400\r\n");
    if (head) return;
    char p[1600]; snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    c->ffd = open(p, O_RDONLY); if (c->ffd < 0) { conn_close(c); return; }
    c->fpos = off; c->fend = off + len; c->is_stream = 0;                       /* pictures don't count as listeners */
}

/* ---------------------------------------------------------------- UPnP */
static void upnp_desc(conn_t *c) {
    buf_t b = { 0 };
    bprintf(&b, "<?xml version=\"1.0\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType><friendlyName>"); bxml(&b, NAME);
    bprintf(&b, "</friendlyName><manufacturer>r3ii-player</manufacturer><modelName>HiBy R3 Pro II</modelName>"
        "<modelDescription>r3ii music server</modelDescription><UDN>uuid:%s</UDN>"
        "<serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId><SCPDURL>/cds.xml</SCPDURL>"
        "<controlURL>/ctl/cds</controlURL><eventSubURL>/evt/cds</eventSubURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId><SCPDURL>/cms.xml</SCPDURL>"
        "<controlURL>/ctl/cms</controlURL><eventSubURL>/evt/cms</eventSubURL></service>"
        "</serviceList><presentationURL>http://%s/</presentationURL></device></root>\n", UUID, c->host);
    respond_buf(c, 200, "text/xml; charset=\"utf-8\"", &b);
}
static const char CDS_SCPD[] =
    "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
    "<actionList><action><name>Browse</name><argumentList>"
    "<argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>"
    "<argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>"
    "<argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>"
    "<argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>"
    "<argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
    "<argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>"
    "<argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>"
    "<argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
    "<argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
    "<argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetSystemUpdateID</name><argumentList><argument><name>Id</name><direction>out</direction>"
    "<relatedStateVariable>SystemUpdateID</relatedStateVariable></argument></argumentList></action>"
    "<action><name>GetSortCapabilities</name><argumentList><argument><name>SortCaps</name><direction>out</direction>"
    "<relatedStateVariable>SortCapabilities</relatedStateVariable></argument></argumentList></action>"
    "<action><name>GetSearchCapabilities</name><argumentList><argument><name>SearchCaps</name><direction>out</direction>"
    "<relatedStateVariable>SearchCapabilities</relatedStateVariable></argument></argumentList></action>"
    "</actionList><serviceStateTable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType>"
    "<allowedValueList><allowedValue>BrowseMetadata</allowedValue><allowedValue>BrowseDirectChildren</allowedValue></allowedValueList></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>"
    "<stateVariable sendEvents=\"yes\"><name>SystemUpdateID</name><dataType>ui4</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>SortCapabilities</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>SearchCapabilities</name><dataType>string</dataType></stateVariable>"
    "</serviceStateTable></scpd>";
static const char CMS_SCPD[] =
    "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
    "<actionList><action><name>GetProtocolInfo</name><argumentList>"
    "<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
    "<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
    "</argumentList></action></actionList><serviceStateTable>"
    "<stateVariable sendEvents=\"yes\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"yes\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>"
    "</serviceStateTable></scpd>";

/* value of <tag>...</tag> in a SOAP body (no namespaces on argument names) */
static int soap_arg(const char *body, const char *tag, char *out, size_t cap) {
    char open[96]; snprintf(open, sizeof open, "<%s>", tag);
    const char *s = strstr(body, open); if (!s) { snprintf(open, sizeof open, "<%s ", tag); s = strstr(body, open);
        if (s) s = strchr(s, '>'); else return 0; if (!s) return 0; s++; } else s += strlen(open);
    const char *e = strstr(s, "</"); if (!e) return 0;
    size_t n = (size_t)(e - s); if (n >= cap) n = cap - 1; memcpy(out, s, n); out[n] = 0;
    /* unescape the few entities that appear in IDs */
    char *r = out, *w = out;
    while (*r) { if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; } else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
        else if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; } else if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; }
        else if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; } else *w++ = *r++; }
    *w = 0; return 1;
}
/* object IDs: "0" = root, else the relative library path */
static const char *rel_of_id(const char *id) { return strcmp(id, "0") ? id : ""; }
/* <upnp:albumArtURI> for a server path ("f/..." or "art/...") */
static void didl_art(buf_t *d, const char *host, const char *art) {
    if (!art) return; buf_t u = { 0 }; bprintf(&u, "http://%s/", host); burl(&u, art);
    bprintf(d, "<upnp:albumArtURI>");
    bxml(d, u.p); bprintf(d, "</upnp:albumArtURI>"); free(u.p);
}
static void didl_container(buf_t *d, const char *host, const char *id, const char *parent, const char *title, int childs, const char *art) {
    bprintf(d, "<container id=\""); bxml(d, id); bprintf(d, "\" parentID=\""); bxml(d, parent);
    bprintf(d, "\" restricted=\"1\" childCount=\"%d\"><dc:title>", childs); bxml(d, title);
    bprintf(d, "</dc:title><upnp:class>object.container.storageFolder</upnp:class>");
    didl_art(d, host, art); bprintf(d, "</container>");
}
static void didl_item(buf_t *d, const char *host, const char *rel, const char *parent, const char *title, const char *cover) {
    char p[1600]; struct stat st; snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    long long sz = stat(p, &st) == 0 ? (long long)st.st_size : 0;
    bprintf(d, "<item id=\""); bxml(d, rel); bprintf(d, "\" parentID=\""); bxml(d, parent);
    bprintf(d, "\" restricted=\"1\"><dc:title>"); bxml(d, title);
    bprintf(d, "</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>");
    didl_art(d, host, cover);
    buf_t u = { 0 }; bprintf(&u, "http://%s/f/", host); burl(&u, rel);
    bprintf(d, "<res protocolInfo=\"http-get:*:%s:*\" size=\"%lld\">", audio_mime(rel), sz); bxml(d, u.p);
    bprintf(d, "</res></item>"); free(u.p);
}
static void soap_reply(conn_t *c, const char *action, const char *svc, buf_t *inner) {
    buf_t b = { 0 };
    bprintf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%sResponse xmlns:u=\"%s\">", action, svc);
    if (inner && inner->p) bput(&b, inner->p, inner->n);
    bprintf(&b, "</u:%sResponse></s:Body></s:Envelope>", action);
    if (inner) free(inner->p);
    respond_buf(c, 200, "text/xml; charset=\"utf-8\"", &b);
}
static void soap_fault(conn_t *c, int code, const char *desc) {
    char body[1024]; int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault><faultcode>s:Client</faultcode>"
        "<faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
        "<errorCode>%d</errorCode><errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>", code, desc);
    respond(c, 500, "text/xml; charset=\"utf-8\"", body, n, NULL);
}
static int radio_browse(buf_t *d, const char *host, const char *id, int meta, int start, int count, int *total);
static void cds_control(conn_t *c, const char *soapaction, const char *body) {
    const char *svc = "urn:schemas-upnp-org:service:ContentDirectory:1";
    if (strstr(soapaction, "GetSystemUpdateID")) { buf_t i = { 0 }; bprintf(&i, "<Id>1</Id>"); soap_reply(c, "GetSystemUpdateID", svc, &i); return; }
    if (strstr(soapaction, "GetSortCapabilities")) { buf_t i = { 0 }; bprintf(&i, "<SortCaps></SortCaps>"); soap_reply(c, "GetSortCapabilities", svc, &i); return; }
    if (strstr(soapaction, "GetSearchCapabilities")) { buf_t i = { 0 }; bprintf(&i, "<SearchCaps></SearchCaps>"); soap_reply(c, "GetSearchCapabilities", svc, &i); return; }
    if (!strstr(soapaction, "Browse")) { soap_fault(c, 401, "Invalid Action"); return; }
    char id[1024] = "0", flag[64] = "BrowseDirectChildren", si[16] = "0", rc[16] = "0";
    soap_arg(body, "ObjectID", id, sizeof id); soap_arg(body, "BrowseFlag", flag, sizeof flag);
    soap_arg(body, "StartingIndex", si, sizeof si); soap_arg(body, "RequestedCount", rc, sizeof rc);
    const char *rel = rel_of_id(id);
    int start = atoi(si), count = atoi(rc); if (count <= 0) count = 1 << 30;
    buf_t d = { 0 }; int returned = 0, total = 0;
    bprintf(&d, "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">");
    if (!strncmp(id, "radio:", 6)) { returned = radio_browse(&d, c->host, id, !strcmp(flag, "BrowseMetadata"), start, count, &total); goto done; }
    if (!safe_rel(rel)) { free(d.p); soap_fault(c, 701, "No such object"); return; }
    char parent[1024]; snprintf(parent, sizeof parent, "%s", rel); { char *s = strrchr(parent, '/'); if (s) *s = 0; else snprintf(parent, sizeof parent, "%s", rel[0] ? "0" : "-1"); }
    if (!strcmp(flag, "BrowseMetadata")) {
        char p[1600]; struct stat st; snprintf(p, sizeof p, "%s/%s", ROOT, rel);
        char art[1700];
        if (!rel[0]) didl_container(&d, c->host, "0", "-1", NAME, 3, NULL);
        else if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { const char *t = strrchr(rel, '/');
            didl_container(&d, c->host, rel, parent, t ? t + 1 : rel, 0, art_for(rel, 1, art, sizeof art) ? art : NULL); }
        else if (stat(p, &st) == 0 && audio_mime(rel)) { const char *t = strrchr(rel, '/'); char dn[300]; display_name(t ? t + 1 : rel, 0, dn, sizeof dn);
            didl_item(&d, c->host, rel, parent, dn, art_for(rel, 0, art, sizeof art) ? art : NULL); }
        else { free(d.p); soap_fault(c, 701, "No such object"); return; }
        returned = total = 1;
    } else {
        int n; ent_t *v = list_dir(rel, &n);
        if (rel[0] && !v) { free(d.p); soap_fault(c, 701, "No such object"); return; }
        /* folder art for its tracks: a cover file, or (big folders) the first embedded picture */
        char fcov[1700]; int rich = n <= ART_RICH, has_cover = 0;
        if (rel[0]) { char cover[1200];
            if (find_cover(rel, cover, sizeof cover)) { snprintf(fcov, sizeof fcov, "f/%s", cover); has_cover = 1; }
            else if (!rich) has_cover = art_for(rel, 1, fcov, sizeof fcov); }
        total = n;
        int radio = !rel[0] && explore_refresh();                     /* the root also holds "R3 Radio", last */
        if (radio) { total = n + 1; if (start <= n && returned < count && start + count > n) { didl_container(&d, c->host, "radio:", "0", "R3 Radio", 0, NULL); } }
        for (int i = start; i < n && returned < count; i++, returned++) {
            char sub[1600], dn[300], art[1700]; snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", v[i].name);
            display_name(v[i].name, v[i].dir, dn, sizeof dn);
            if (v[i].dir) didl_container(&d, c->host, sub, rel[0] ? rel : "0", dn, 0,
                                         rel[0] && rich && art_for(sub, 1, art, sizeof art) ? art : NULL);
            else didl_item(&d, c->host, sub, rel[0] ? rel : "0", dn,
                           has_cover ? fcov : rich && art_for(sub, 0, art, sizeof art) ? art : NULL); }
        if (radio && start <= n && start + count > n) returned++;
        free(v);
    }
done:
    bprintf(&d, "</DIDL-Lite>");
    buf_t i = { 0 }; bprintf(&i, "<Result>"); bxml(&i, d.p); free(d.p);
    bprintf(&i, "</Result><NumberReturned>%d</NumberReturned><TotalMatches>%d</TotalMatches><UpdateID>1</UpdateID>", returned, total);
    soap_reply(c, "Browse", svc, &i);
}
static void cms_control(conn_t *c, const char *soapaction) {
    const char *svc = "urn:schemas-upnp-org:service:ConnectionManager:1";
    if (strstr(soapaction, "GetProtocolInfo")) { buf_t i = { 0 };
        bprintf(&i, "<Source>http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,http-get:*:audio/mp4:*,http-get:*:audio/wav:*</Source><Sink></Sink>");
        soap_reply(c, "GetProtocolInfo", svc, &i); return; }
    soap_fault(c, 401, "Invalid Action");
}

/* ---------------------------------------------------------------- radio
 * explore.c (shared with r3player) reads the library through smartmix_each(): here that is
 * r3player's index file, read again whenever it (or Genres.txt) changes. */
#define IDX_FILE "/usr/data/r3_mix.idx"
#define STATS_FILE "/usr/data/r3_stats"
int smartmix_version(void) {
    struct stat a, g; char gp[1200]; snprintf(gp, sizeof gp, "%s/Genres.txt", ROOT);
    if (stat(IDX_FILE, &a) != 0) return 0;
    int v = (int)(a.st_mtime ^ a.st_size); if (stat(gp, &g) == 0) v ^= (int)(g.st_mtime * 31 + g.st_size);
    return v ? v : 1; }
int smartmix_tags_done(void) { return 1; }
static unsigned hs(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
int smartmix_each(smartmix_track_fn fn, void *u) {
    FILE *f = fopen(IDX_FILE, "r"); if (!f) return -1;
    enum { HP = 8192 }; char **pk = calloc(HP, sizeof *pk); int *pv = calloc(HP, sizeof *pv);   /* plays per track */
    FILE *sf = fopen(STATS_FILE, "r"); char l[1600];
    if (sf && pk && pv) { while (fgets(l, sizeof l, sf)) { l[strcspn(l, "\n")] = 0; char *t = strchr(l, '\t'); if (!t) continue; *t = 0;
        unsigned k = hs(l) & (HP - 1); for (int n = 0; n < HP && pk[k]; n++) k = (k + 1) & (HP - 1);
        if (!pk[k]) { pk[k] = strdup(l); pv[k] = atoi(t + 1); } } }
    if (sf) fclose(sf);
    int n = 0;
    while (fgets(l, sizeof l, f)) { l[strcspn(l, "\n")] = 0;
        char *a = strchr(l, '\t'); if (!a) continue; *a++ = 0; char *g = strchr(a, '\t'); if (!g) continue; *g++ = 0;
        char *y = strchr(g, '\t'); if (!y) continue; *y++ = 0;
        int plays = 0; if (pk) { unsigned k = hs(l) & (HP - 1);
            for (int c = 0; c < HP && pk[k]; c++, k = (k + 1) & (HP - 1)) if (!strcmp(pk[k], l)) { plays = pv[k]; break; } }
        fn(u, l, a, g, atoi(y), plays, 0, 0); n++; }
    fclose(f);
    if (pk) for (int i = 0; i < HP; i++) free(pk[i]);
    free(pk); free(pv); return n; }

#define RMAX 160
typedef struct { char rel[RMAX][1024]; char title[RMAX][300]; int n; char last[128]; } mix_t;
/* the next artist: similar to a, but inside the chosen genre / subgenre (a radio asked for
 * Power metal stays in Power metal; "similar" alone only keeps to the main genre) */
static int radio_next(int a, int fam, int sub) {
    int b = a >= 0 ? explore_similar(a) : -1;
    if (b < 0) return explore_pick(fam, sub);
    if (sub >= 0 && strcmp(explore_artist_sub(b), explore_sub_name(sub))) return explore_pick(fam, sub);
    if (fam >= 0 && explore_artist_fam(b) != fam) return explore_pick(fam, -1);
    return b; }
/* about `albums` albums: a pick in the genre (or after `from`: the next one after it), then the next ones */
static int radio_mix(mix_t *m, int fam, int sub, const char *from, int albums) {
    m->n = 0; m->last[0] = 0;
    if (!explore_refresh()) return 0;
    int a = from && from[0] && explore_find(from) >= 0 ? radio_next(explore_find(from), fam, sub) : explore_pick(fam, sub);
    size_t rl = strlen(ROOT);
    for (int k = 0; k < albums && a >= 0 && m->n < RMAX; k++) {
        char dir[1024], t[256]; if (!explore_album(a, NULL, dir, sizeof dir, t, sizeof t)) break;
        explore_seen(a);
        const char *rel = !strncmp(dir, ROOT, rl) && dir[rl] == '/' ? dir + rl + 1 : dir;
        int nv; ent_t *v = list_dir(rel, &nv);
        for (int i = 0; v && i < nv && m->n < RMAX; i++) { if (v[i].dir) continue;
            char dn[300]; display_name(v[i].name, 0, dn, sizeof dn);
            snprintf(m->rel[m->n], 1024, "%s/%s", rel, v[i].name);
            snprintf(m->title[m->n], 300, "%s - %s", explore_artist(a), dn); m->n++; }
        free(v);
        snprintf(m->last, sizeof m->last, "%s", explore_artist(a));
        a = radio_next(a, fam, sub); }
    return m->n; }
static void q_param(const char *q, const char *key, char *out, size_t cap) {   /* ?a=b&c=d, decoded ('+' = space) */
    out[0] = 0; if (!q) return; size_t kl = strlen(key);
    for (const char *p = q; p && *p; ) { if (!strncmp(p, key, kl) && p[kl] == '=') { p += kl + 1; size_t n = strcspn(p, "&");
            if (n >= cap) n = cap - 1; memcpy(out, p, n); out[n] = 0;
            for (char *c = out; *c; c++) if (*c == '+') *c = ' ';
            url_decode(out); return; }
        p = strchr(p, '&'); if (p) p++; } }
static int fam_of(const char *g) {                     /* "Metal", "metal" or "0"; -1 = any */
    if (!g[0]) return -1; if (isdigit((unsigned char)g[0])) { int f = atoi(g); return f >= 0 && f < EX_NFAM ? f : -1; }
    for (int f = 0; f < EX_NFAM; f++) if (!strcasecmp(g, explore_fam_name(f))) return f;
    return -1; }
static int sub_of(int fam, const char *s) {
    if (!s[0]) return -1;
    for (int f = fam < 0 ? 0 : fam; f < (fam < 0 ? EX_NFAM : fam + 1); f++) { int ids[64], n = explore_subs(f, ids, 64);
        for (int i = 0; i < n; i++) if (!strcasecmp(s, explore_sub_name(ids[i]))) return ids[i]; }
    return -1; }
static void burlq(buf_t *b, const char *s) {            /* percent-encode a query value */
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) bput(b, (const char *)p, 1); else bprintf(b, "%%%02X", *p); } }
static void radio_link(buf_t *b, const char *host, int fam, int sub, const char *from) {
    bprintf(b, "http://%s/radio.m3u?", host);
    if (fam >= 0) { bprintf(b, "genre="); burlq(b, explore_fam_name(fam)); }
    if (sub >= 0) { bprintf(b, "&sub="); burlq(b, explore_sub_name(sub)); }
    if (from && from[0]) { bprintf(b, "&from="); burlq(b, from); } }
static void radio_m3u(conn_t *c, const char *query) {
    char g[96], sb[96], from[160]; q_param(query, "genre", g, sizeof g); q_param(query, "sub", sb, sizeof sb); q_param(query, "from", from, sizeof from);
    static mix_t m;
    if (!explore_refresh()) { respond(c, 503, "text/plain", "library index not ready (start r3player once)\n", 46, NULL); return; }
    int fam = fam_of(g), sub = sub_of(fam, sb); if (sub >= 0 && fam < 0) fam = explore_sub_fam(sub);
    radio_mix(&m, fam, sub, from, 3);
    buf_t b = { 0 }; bprintf(&b, "#EXTM3U\n");
    for (int i = 0; i < m.n; i++) { bprintf(&b, "#EXTINF:-1,%s\nhttp://%s/f/", m.title[i], c->host); burl(&b, m.rel[i]); bput(&b, "\n", 1); }
    /* the last entry is the next batch: VLC opens it when it gets there, so the radio never ends */
    bprintf(&b, "#EXTINF:-1,R3 radio (more)\n"); radio_link(&b, c->host, fam, sub, m.last); bput(&b, "\n", 1);
    respond_buf(c, 200, "audio/x-mpegurl; charset=utf-8", &b); }
static void radio_page(conn_t *c) {
    buf_t b = { 0 };
    bprintf(&b, "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>R3 radio</title>"
                "<style>body{font:16px sans-serif;background:#0a0a0a;color:#f4e4d0;margin:16px}a{color:#e8731c;text-decoration:none}"
                "li{margin:6px 0}.m{color:#8a6a4a;font-size:13px}ul ul{margin:4px 0 10px}</style><h2>R3 radio</h2>");
    if (!explore_refresh()) { bprintf(&b, "<p>The library index isn't ready yet (r3player builds it).</p>"); respond_buf(c, 200, "text/html; charset=utf-8", &b); return; }
    bprintf(&b, "<p class=m>An endless mix from the library: an album, then an album by a similar artist, and so on. "
                "In VLC: Media &gt; Open Network Stream and paste a link, or Local Network &gt; Universal Plug'n'Play &gt; R3 Radio.</p><ul>");
    buf_t u = { 0 }; radio_link(&u, c->host, -1, -1, NULL);
    bprintf(&b, "<li><a href='"); bxml(&b, u.p); bprintf(&b, "'>&#9654; Surprise me</a> <span class=m>(%d artists)</span>", explore_artists()); free(u.p);
    for (int f = 0; f < EX_NFAM; f++) { int na = explore_fam_artists(f); if (!na) continue;
        buf_t l = { 0 }; radio_link(&l, c->host, f, -1, NULL);
        bprintf(&b, "<li><a href='"); bxml(&b, l.p); bprintf(&b, "'>&#9654; "); bxml(&b, explore_fam_name(f)); bprintf(&b, "</a> <span class=m>(%d)</span>", na); free(l.p);
        int ids[64], ns = f < EX_UNSORTED ? explore_subs(f, ids, 64) : 0;
        if (ns) { bprintf(&b, "<ul>"); for (int i = 0; i < ns; i++) { buf_t s2 = { 0 }; radio_link(&s2, c->host, f, ids[i], NULL);
            bprintf(&b, "<li><a href='"); bxml(&b, s2.p); bprintf(&b, "'>"); bxml(&b, explore_sub_name(ids[i]));
            bprintf(&b, "</a> <span class=m>(%d)</span>", explore_sub_artists(ids[i])); free(s2.p); } bprintf(&b, "</ul>"); } }
    bprintf(&b, "</ul><p><a href='/'>&larr; library</a></p>");
    respond_buf(c, 200, "text/html; charset=utf-8", &b); }
/* UPnP: "R3 Radio" > genres > subgenres; a leaf is a fresh mix (kept for a while, so a client
 * paging through it sees one list). ids: radio:  radio:f<F>  leaves radio:x  radio:a<F>  radio:s<S> */
static struct { char id[32]; mix_t m; long long at; } RC[4]; static int rc_next;
static mix_t *radio_cached(const char *id, int fam, int sub, int fresh) {
    long long t = now_ms();
    for (int i = 0; i < 4; i++) if (!strcmp(RC[i].id, id) && (!fresh || t - RC[i].at < 15000) && t - RC[i].at < 30 * 60 * 1000) return &RC[i].m;
    int k = -1; for (int i = 0; i < 4; i++) if (!strcmp(RC[i].id, id)) k = i;
    if (k < 0) { k = rc_next; rc_next = (rc_next + 1) % 4; }
    snprintf(RC[k].id, sizeof RC[k].id, "%s", id); RC[k].at = t; radio_mix(&RC[k].m, fam, sub, NULL, 3); return &RC[k].m; }
static void didl_container(buf_t *d, const char *host, const char *id, const char *parent, const char *title, int childs, const char *art);
static void didl_item(buf_t *d, const char *host, const char *rel, const char *parent, const char *title, const char *cover);
static int art_for(const char *rel, int is_dir, char *out, size_t cap);
/* children (or metadata) of a radio id into d; returns how many were returned, *total set */
static int radio_browse(buf_t *d, const char *host, const char *id, int meta, int start, int count, int *total) {
    *total = 0; if (!explore_refresh()) return 0;
    char cid[40], title[160]; int returned = 0;
    if (meta) { *total = 1; const char *t = "R3 Radio"; int f = -1, s = -1;
        if (id[6] == 'f') { f = atoi(id + 7); t = explore_fam_name(f); }
        else if (id[6] == 'a') { f = atoi(id + 7); snprintf(title, sizeof title, "All of %s", explore_fam_name(f)); t = title; }
        else if (id[6] == 's') { s = atoi(id + 7); t = explore_sub_name(s); }
        else if (id[6] == 'x') t = "Surprise me";
        didl_container(d, host, id, id[6] ? (id[6] == 's' ? "radio:" : "radio:") : "0", t, 0, NULL); return 1; }
    if (!id[6]) {                                                     /* radio: -> surprise + families */
        int fams[EX_NFAM], nf = 0; for (int f = 0; f < EX_NFAM; f++) if (explore_fam_artists(f)) fams[nf++] = f;
        *total = nf + 1;
        for (int i = start; i < nf + 1 && returned < count; i++, returned++) {
            if (!i) { didl_container(d, host, "radio:x", "radio:", "Surprise me", 0, NULL); continue; }
            int f = fams[i - 1]; snprintf(cid, sizeof cid, "radio:f%d", f);
            didl_container(d, host, cid, "radio:", explore_fam_name(f), 0, NULL); }
        return returned; }
    if (id[6] == 'f') { int f = atoi(id + 7), ids[64], ns = f < EX_UNSORTED ? explore_subs(f, ids, 64) : 0;   /* family -> all + subs */
        *total = ns + 1;
        for (int i = start; i < ns + 1 && returned < count; i++, returned++) {
            if (!i) { snprintf(cid, sizeof cid, "radio:a%d", f); snprintf(title, sizeof title, "All of %s", explore_fam_name(f));
                didl_container(d, host, cid, id, title, 0, NULL); continue; }
            snprintf(cid, sizeof cid, "radio:s%d", ids[i - 1]); didl_container(d, host, cid, id, explore_sub_name(ids[i - 1]), 0, NULL); }
        return returned; }
    int fam = -1, sub = -1;                                           /* a leaf: the mix itself */
    if (id[6] == 'a') fam = atoi(id + 7); else if (id[6] == 's') { sub = atoi(id + 7); fam = explore_sub_fam(sub); }
    mix_t *m = radio_cached(id, fam, sub, start == 0);
    *total = m->n;
    for (int i = start; i < m->n && returned < count; i++, returned++) { char art[1700];
        didl_item(d, host, m->rel[i], id, m->title[i], art_for(m->rel[i], 0, art, sizeof art) ? art : NULL); }
    return returned; }

/* ---------------------------------------------------------------- request dispatch */
static const char *header(const char *req, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name); const char *p = req;
    while ((p = strchr(p, '\n'))) { p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') { p += nl + 1; while (*p == ' ') p++;
            size_t n = strcspn(p, "\r\n"); if (n >= cap) n = cap - 1; memcpy(out, p, n); out[n] = 0; return out; } }
    return NULL;
}
/* ---- R3-to-R3 playlist sharing ---- */
static void page_playlists(conn_t *c) {
    buf_t b = { 0 };
    char dir[1024]; snprintf(dir, sizeof dir, "%s/Playlists", ROOT);
    DIR *d = opendir(dir);
    if (d) { struct dirent *e;
        while ((e = readdir(d))) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".m3u")) continue;
            char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            int cnt = 0; FILE *f = fopen(p, "r");
            if (f) { char ln[1024]; while (fgets(ln, sizeof ln, f)) if (ln[0] && ln[0] != '#') cnt++; fclose(f); }
            bprintf(&b, "%.*s\t%d\n", (int)(dot - e->d_name), e->d_name, cnt);
        }
        closedir(d);
    }
    respond_buf(c, 200, "text/plain; charset=utf-8", &b);
}
static void serve_playlist(conn_t *c, const char *name) {
    char nm[256]; snprintf(nm, sizeof nm, "%s", name);
    char *dot = strrchr(nm, '.'); if (dot && !strcasecmp(dot, ".m3u")) *dot = 0;
    if (!nm[0] || strstr(nm, "..") || strchr(nm, '/')) { respond(c, 403, "text/plain", "no\n", 3, NULL); return; }
    char p[1200]; snprintf(p, sizeof p, "%s/Playlists/%s.m3u", ROOT, nm);
    FILE *f = fopen(p, "r");
    if (!f) { respond(c, 404, "text/plain", "not found\n", 10, NULL); return; }
    buf_t b = { 0 }; char ln[1024];
    while (fgets(ln, sizeof ln, f)) bput(&b, ln, strlen(ln));
    fclose(f);
    respond_buf(c, 200, "text/plain; charset=utf-8", &b);
}

static void handle(conn_t *c) {
    char method[8] = "", path[4096] = "";
    if (sscanf(c->req, "%7s %4095s", method, path) != 2) { respond(c, 400, "text/plain", "bad request\n", 12, NULL); return; }
    char *q = strchr(path, '?'); char query[1024] = ""; if (q) { snprintf(query, sizeof query, "%s", q + 1); *q = 0; }
    url_decode(path);
    char range[128], sa[256], *body = strstr(c->req, "\r\n\r\n"); body = body ? body + 4 : "";
    int head = !strcmp(method, "HEAD");
    if (!strcmp(path, "/desc.xml")) { upnp_desc(c); return; }
    if (!strcmp(path, "/cds.xml")) { respond(c, 200, "text/xml; charset=\"utf-8\"", CDS_SCPD, sizeof CDS_SCPD - 1, NULL); return; }
    if (!strcmp(path, "/cms.xml")) { respond(c, 200, "text/xml; charset=\"utf-8\"", CMS_SCPD, sizeof CMS_SCPD - 1, NULL); return; }
    if (!strcmp(path, "/ctl/cds")) { cds_control(c, header(c->req, "SOAPACTION", sa, sizeof sa) ? sa : "", body); return; }
    if (!strcmp(path, "/ctl/cms")) { cms_control(c, header(c->req, "SOAPACTION", sa, sizeof sa) ? sa : ""); return; }
    if (!strncmp(path, "/evt/", 5)) {   /* no eventing: accept subscriptions so clients stay happy */
        char ex[128]; snprintf(ex, sizeof ex, "SID: uuid:%s-evt\r\nTIMEOUT: Second-1800\r\n", UUID); respond(c, 200, "text/plain", "", 0, ex); return; }
    if (!strcmp(path, "/playlists")) { page_playlists(c); return; }
    if (!strcmp(path, "/radio") || !strcmp(path, "/radio/")) { radio_page(c); return; }
    if (!strcmp(path, "/radio.m3u")) { radio_m3u(c, query); return; }
    if (!strncmp(path, "/pl/", 4)) { serve_playlist(c, path + 4); return; }
    const char *rel = NULL; int kind = 0;
    if (!strcmp(path, "/") || !strcmp(path, "/browse") || !strcmp(path, "/browse/")) { rel = ""; kind = 1; }
    else if (!strncmp(path, "/browse/", 8)) { rel = path + 8; kind = 1; }
    else if (!strncmp(path, "/m3u/", 5)) { rel = path + 5; kind = 2; }
    else if (!strncmp(path, "/f/", 3)) { rel = path + 3; kind = 3; }
    else if (!strncmp(path, "/art/", 5)) { rel = path + 5; kind = 4; }
    if (!kind) { respond(c, 404, "text/plain", "not found\n", 10, NULL); return; }
    char clean[4096]; snprintf(clean, sizeof clean, "%s", rel);
    size_t L = strlen(clean); while (L && clean[L - 1] == '/') clean[--L] = 0;
    if (!safe_rel(clean)) { respond(c, 404, "text/plain", "not found\n", 10, NULL); return; }
    if (kind == 1) page_browse(c, clean);
    else if (kind == 2) page_m3u(c, clean);
    else if (kind == 4) serve_art(c, clean, head);
    else serve_file(c, clean, header(c->req, "Range", range, sizeof range), head);
}

/* ---------------------------------------------------------------- SSDP */
static int ssdp_fd;
static const char *NTS[] = { "upnp:rootdevice", "urn:schemas-upnp-org:device:MediaServer:1",
    "urn:schemas-upnp-org:service:ContentDirectory:1", "urn:schemas-upnp-org:service:ConnectionManager:1", NULL };
static int local_ip_for(struct sockaddr_in *peer, char *ip, size_t cap) {
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return 0;
    struct sockaddr_in a = *peer; if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return 0; }
    struct sockaddr_in me; socklen_t ml = sizeof me; getsockname(s, (struct sockaddr *)&me, &ml); close(s);
    inet_ntop(AF_INET, &me.sin_addr, ip, cap); return 1;
}
static void ssdp_send(const char *msg, struct sockaddr_in *to) { sendto(ssdp_fd, msg, strlen(msg), 0, (struct sockaddr *)to, sizeof *to); }
static void ssdp_notify(const char *nts) {
    struct sockaddr_in g = { 0 }; g.sin_family = AF_INET; g.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &g.sin_addr);
    char ip[64] = "0.0.0.0"; local_ip_for(&g, ip, sizeof ip);
    for (int i = -1; NTS[i + 1] || i < 0; i++) {
        char nt[160], usn[256], msg[1024];
        if (i < 0) { snprintf(nt, sizeof nt, "uuid:%s", UUID); snprintf(usn, sizeof usn, "uuid:%s", UUID); }
        else { snprintf(nt, sizeof nt, "%s", NTS[i]); snprintf(usn, sizeof usn, "uuid:%s::%s", UUID, NTS[i]); }
        snprintf(msg, sizeof msg, "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
                 "LOCATION: http://%s:%d/desc.xml\r\nNT: %s\r\nNTS: %s\r\nSERVER: Linux/3.x UPnP/1.0 r3srv/1.0\r\nUSN: %s\r\n\r\n",
                 ip, PORT, nt, nts, usn);
        ssdp_send(msg, &g);
        if (!NTS[i + 1] && i >= 0) break; }
}
static void ssdp_handle(void) {
    char buf[2048]; struct sockaddr_in from; socklen_t fl = sizeof from;
    int n = (int)recvfrom(ssdp_fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl); if (n <= 0) return; buf[n] = 0;
    if (strncmp(buf, "M-SEARCH", 8)) return;
    char st[200] = ""; if (!header(buf, "ST", st, sizeof st)) return;
    char ip[64]; if (!local_ip_for(&from, ip, sizeof ip)) return;
    const char *targets[8]; int nt = 0; char uu[80]; snprintf(uu, sizeof uu, "uuid:%s", UUID);
    if (!strcmp(st, "ssdp:all")) { targets[nt++] = uu; for (int i = 0; NTS[i]; i++) targets[nt++] = NTS[i]; }
    else if (!strcmp(st, uu)) targets[nt++] = uu;
    else for (int i = 0; NTS[i]; i++) if (!strcmp(st, NTS[i])) targets[nt++] = NTS[i];
    for (int i = 0; i < nt; i++) { char usn[256], msg[1024];
        if (targets[i] == uu) snprintf(usn, sizeof usn, "%s", uu); else snprintf(usn, sizeof usn, "uuid:%s::%s", UUID, targets[i]);
        snprintf(msg, sizeof msg, "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: http://%s:%d/desc.xml\r\n"
                 "SERVER: Linux/3.x UPnP/1.0 r3srv/1.0\r\nST: %s\r\nUSN: %s\r\n\r\n", ip, PORT, targets[i], usn);
        ssdp_send(msg, &from); }
}

/* ---------------------------------------------------------------- main loop */
static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }
static void load_uuid(void) {
    FILE *f = fopen(UUIDF, "r");
    if (f) { if (fscanf(f, "%47s", UUID) == 1 && strlen(UUID) > 30) { fclose(f); return; } fclose(f); }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    snprintf(UUID, sizeof UUID, "%08x-%04x-4%03x-a%03x-%04x%08x", rand(), rand() & 0xffff, rand() & 0xfff, rand() & 0xfff, rand() & 0xffff, rand());
    f = fopen(UUIDF, "w"); if (f) { fprintf(f, "%s\n", UUID); fclose(f); }
}
int main(int argc, char **argv) {
    int o; while ((o = getopt(argc, argv, "r:p:n:m:u:s:")) != -1) switch (o) {
        case 'r': ROOT = optarg; break; case 'p': PORT = atoi(optarg); break; case 'n': NAME = optarg; break;
        case 'm': MAXSTREAMS = atoi(optarg); break; case 'u': UUIDF = optarg; break; case 's': STATF = optarg; break; }
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, on_term); signal(SIGINT, on_term);
    load_uuid();
    int ls = socket(AF_INET, SOCK_STREAM, 0); if (ls < 0) die("socket");
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) die("bind http");
    listen(ls, 16); fcntl(ls, F_SETFL, O_NONBLOCK);
    ssdp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(ssdp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { 0 }; sa.sin_family = AF_INET; sa.sin_port = htons(1900); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ssdp_fd, (struct sockaddr *)&sa, sizeof sa) < 0) perror("bind ssdp (discovery off)");
    struct ip_mreq mr; inet_pton(AF_INET, "239.255.255.250", &mr.imr_multiaddr); mr.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(ssdp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0) perror("multicast join");
    unsigned char ttl = 4; setsockopt(ssdp_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    int disc_fd = socket(AF_INET, SOCK_DGRAM, 0);   /* R3-to-R3 discovery beacon (UDP broadcast :8201) */
    { int bc = 1; setsockopt(disc_fd, SOL_SOCKET, SO_BROADCAST, &bc, sizeof bc); }
    fprintf(stderr, "[r3srv] '%s' on :%d, root %s, max %d streams, uuid %s\n", NAME, PORT, ROOT, MAXSTREAMS, UUID);
    status_write();
    ssdp_notify("ssdp:alive"); long long last_notify = now_ms(), last_beacon = 0;
    while (!quit) {
        struct pollfd pf[MAXC + 2]; int map[MAXC + 2], np = 0;
        pf[np].fd = ls; pf[np].events = POLLIN; map[np++] = -1;
        pf[np].fd = ssdp_fd; pf[np].events = POLLIN; map[np++] = -2;
        for (int i = 0; i < MAXC; i++) if (C[i].fd > 0) {
            pf[np].fd = C[i].fd; pf[np].events = C[i].state == C_READ ? POLLIN : POLLOUT; map[np++] = i; }
        if (poll(pf, np, 1000) < 0) { if (errno == EINTR) continue; break; }
        long long t = now_ms();
        if (t - last_beacon > 5000 && disc_fd >= 0) { last_beacon = t;   /* announce ourselves to R3 peers */
            char msg[160]; int ml = snprintf(msg, sizeof msg, "R3PLAY\t%s\t%d", NAME, PORT);
            struct sockaddr_in ba = { 0 }; ba.sin_family = AF_INET; ba.sin_port = htons(8201);
            ba.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(disc_fd, msg, ml, 0, (struct sockaddr *)&ba, sizeof ba); }
        if (t - last_notify > 10 * 60 * 1000) { ssdp_notify("ssdp:alive"); last_notify = t; }
        for (int k = 0; k < np; k++) {
            if (!pf[k].revents) continue;
            if (map[k] == -1) {
                struct sockaddr_in peer; socklen_t pl = sizeof peer;
                int fd = accept(ls, (struct sockaddr *)&peer, &pl); if (fd < 0) continue;
                int slot = -1; for (int i = 0; i < MAXC; i++) if (!C[i].fd) { slot = i; break; }
                if (slot < 0) { close(fd); continue; }
                fcntl(fd, F_SETFL, O_NONBLOCK);
                conn_t *c = &C[slot]; memset(c, 0, sizeof *c); c->fd = fd; c->state = C_READ; c->last = t;
                struct sockaddr_in me; socklen_t ml = sizeof me; getsockname(fd, (struct sockaddr *)&me, &ml);
                char ip[64]; inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip); snprintf(c->host, sizeof c->host, "%s:%d", ip, PORT);
                continue; }
            if (map[k] == -2) { ssdp_handle(); continue; }
            conn_t *c = &C[map[k]]; c->last = t;
            if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) { conn_close(c); continue; }
            if (c->state == C_READ) {
                int n = (int)read(c->fd, c->req + c->rlen, sizeof c->req - 1 - c->rlen);
                if (n <= 0) { conn_close(c); continue; }
                c->rlen += n; c->req[c->rlen] = 0;
                char *hend = strstr(c->req, "\r\n\r\n");
                if (!hend) { if (c->rlen >= sizeof c->req - 1) conn_close(c); continue; }
                char cl[32]; long need = header(c->req, "Content-Length", cl, sizeof cl) ? atol(cl) : 0;
                if ((long)(c->rlen - (hend + 4 - c->req)) < need && c->rlen < sizeof c->req - 1) continue;   /* wait for SOAP body */
                handle(c);
            } else {   /* write head/body, then the file */
                if (c->off < c->out.n) {
                    int n = (int)write(c->fd, c->out.p + c->off, c->out.n - c->off);
                    if (n < 0) { if (errno != EAGAIN) conn_close(c); continue; }
                    c->off += n; if (c->off < c->out.n) continue; }
                if (c->ffd > 0 && c->fpos < c->fend) {
                    size_t chunk = (size_t)(c->fend - c->fpos); if (chunk > 256 * 1024) chunk = 256 * 1024;
                    ssize_t n = sendfile(c->fd, c->ffd, &c->fpos, chunk);
                    if (n < 0) { if (errno != EAGAIN) conn_close(c); continue; }
                    if (n == 0) { conn_close(c); continue; }
                    if (c->fpos < c->fend) continue; }
                conn_close(c);
            }
        }
        for (int i = 0; i < MAXC; i++)   /* drop idle half-open connections */
            if (C[i].fd > 0 && C[i].state == C_READ && t - C[i].last > 15000) conn_close(&C[i]);
    }
    ssdp_notify("ssdp:byebye");
    if (STATF) unlink(STATF);
    fprintf(stderr, "[r3srv] stopped\n");
    return 0;
}
