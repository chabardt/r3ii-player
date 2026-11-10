/* smartmix.c — on-device song suggestions (see smartmix.h).
 * Index: one entry per track (path relative to the SD root) with artist (from the folder),
 * genre + year (from tags), listening stats, and an optional bliss acoustic vector.
 * Similarity: acoustic (bliss) when both tracks have a vector, else tag affinity. */
#include "smartmix.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define SD_ROOT  "/usr/data/mnt/sd_0"
#define IDX_CACHE "/usr/data/r3_mix.idx"
#define FULL_FLAG "/usr/data/r3_mix.full"    /* present once genre/year enrichment has finished */
#define TAGS_VER 2      /* in FULL_FLAG: 2 = every genre value kept ("a;b"), M4A tags read */
static volatile int g_version;   /* bumped whenever tags / the index change (explore.c rebuilds) */
static int tags_ver(void){ FILE*f=fopen(FULL_FLAG,"r"); int v=0; if(!f) return -1;
    if(fscanf(f,"%d",&v)!=1) v=1; fclose(f); return v; }   /* old builds left an empty flag = v1 */
static void mark_full(void){ FILE*f=fopen(FULL_FLAG,"w"); if(f){ fprintf(f,"%d\n",TAGS_VER); fclose(f); } }
#define STATS_FILE "/usr/data/r3_stats"
#define FAV_M3U  SD_ROOT "/Playlists/Favorites.m3u"
#define BLISS_FILE SD_ROOT "/.r3bliss"     /* PC-built: <rel>\t f0 f1 ... per line */
#define MAXTRK 40000
#define MAXSTR 4000
#define VDIM 20

typedef struct { char *rel; short artist, genre, yr; unsigned short plays, skips; unsigned char fav; float *vec; } ent_t;
static ent_t E[MAXTRK]; static int NE;
static char *ART[MAXSTR]; static int NART; static char *GEN[MAXSTR]; static int NGEN;
static float *VEC;                      /* VDIM*NE pool when a bliss file is loaded */
static volatile int g_ready, g_dirty;
static int g_cache_has_genre;   /* a loaded cache already carried genres -> no re-enrich */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char g_root[512];

static int intern(char **tab, int *n, const char *s){
    for(int i=0;i<*n;i++) if(!strcasecmp(tab[i],s)) return i;
    if(*n>=MAXSTR) return -1; tab[*n]=strdup(s); return (*n)++;
}
static const char* rel_of(const char*full){ size_t n=strlen(SD_ROOT); return !strncmp(full,SD_ROOT "/",n+1)?full+n+1:full; }
static int y4(const char*s){ while(*s&&!isdigit((unsigned char)*s))s++; int y=atoi(s); return (y>=1900&&y<=2100)?y:0; }

/* --- read genre + year from the first part of a track --- */
static unsigned sid(const unsigned char*p){ return ((p[0]&0x7f)<<21)|((p[1]&0x7f)<<14)|((p[2]&0x7f)<<7)|(p[3]&0x7f); }
static void id3_text(const unsigned char*b,int n,const char*id,char*out,int cap){ out[0]=0;
    if(n<10||memcmp(b,"ID3",3)) return; int ver=b[3]; long sz=sid(b+6); int i=10; if(sz>n)sz=n;
    while(i+10<=10+sz){ if(!b[i]) break;
        long fs = ver==4 ? (long)sid(b+i+4) : (((long)b[i+4]<<24)|((long)b[i+5]<<16)|((long)b[i+6]<<8)|b[i+7]);
        if(fs<=0||i+10+fs>n) break;
        if(!memcmp(b+i,id,4)){ const unsigned char*d=b+i+10; int enc=d[0]; const unsigned char*t=d+1; int tl=fs-1;
            /* ID3v2.4 separates several values (e.g. genres) with NUL: keep them all, as
             * "Heavy Metal;Power Metal" (glued, they used to read "Heavy MetalPower Metal") */
            int j=0, sep=0; const int w=(enc==1||enc==2)?2:1;
            if(w==2 && tl>=2 && ((t[0]==0xFF&&t[1]==0xFE)||(t[0]==0xFE&&t[1]==0xFF))){ if(enc==1) enc=t[0]==0xFF?1:2; t+=2; tl-=2; }  /* BOM */
            for(int k=0;k+w-1<tl&&j<cap-2;k+=w){
                unsigned char c = w==1 ? t[k] : enc==1 ? t[k] : t[k+1];
                unsigned char hi = w==1 ? 0 : enc==1 ? t[k+1] : t[k];
                if(c==0&&hi==0){ sep=1; if(w==2 && k+3<tl && t[k+2]==0xFF && t[k+3]==0xFE) k+=2; continue; }  /* next value (maybe with a BOM) */
                if(hi || c<32) continue;          /* UTF-16 beyond Latin-1 / control chars: skip */
                if(sep && j){ out[j++]=';'; } sep=0;
                if(c<0x80) out[j++]=(char)c;
                else if(j<cap-3){ out[j++]=(char)(0xC0|(c>>6)); out[j++]=(char)(0x80|(c&0x3F)); } }   /* Latin-1 -> UTF-8 */
            out[j]=0; return; }
        i+=10+fs; }
}
/* value of a Vorbis comment; a key present several times (GENRE=Metal, GENRE=Rock) gives "Metal;Rock" */
static void vorbis_field(const char*b,int n,const char*key,char*out,int cap){ out[0]=0;
    int kl=strlen(key), j=0;
    for(int i=0;i+kl<n;i++){ if(strncasecmp(b+i,key,kl)||b[i+kl]!='=') continue;
        if(i>0 && isalpha((unsigned char)b[i-1])) continue;          /* SUBGENRE= is not GENRE= */
        if(j && j<cap-1) out[j++]=';';
        int k=i+kl+1; while(k<n&&(unsigned char)b[k]>=32&&j<cap-1) out[j++]=b[k++];
        i=k; }
    out[j]=0;
}
/* MP4/M4A: the text of an ilst atom ("\xA9gen", "\xA9day") — size, name, then a "data" atom */
static void mp4_text(const unsigned char*b,int n,const char*name,char*out,int cap){ out[0]=0;
    for(int i=4;i+24<n;i++){ if(memcmp(b+i,name,4)||memcmp(b+i+8,"data",4)) continue;
        long dsz=((long)b[i+4]<<24)|((long)b[i+5]<<16)|((long)b[i+6]<<8)|b[i+7];
        int len=(int)dsz-16, j=0; const unsigned char*t=b+i+20;
        if(len<=0||t+len>b+n) continue;
        for(int k=0;k<len&&j<cap-1;k++) if(t[k]>=32||t[k]>=0x80) out[j++]=(char)t[k];
        out[j]=0; return; }
}
static void read_meta(const char*full,char*genre,int gcap,int*year){
    genre[0]=0; *year=0;
    FILE*f=fopen(full,"rb"); if(!f) return;
    static unsigned char buf[48*1024];   /* single-threaded indexer */
    int n=(int)fread(buf,1,sizeof buf,f); fclose(f); if(n<16) return;
    if(!memcmp(buf,"ID3",3)){ char yr[16];
        id3_text(buf,n,"TCON",genre,gcap);
        id3_text(buf,n,"TDRC",yr,sizeof yr); if(!yr[0]) id3_text(buf,n,"TYER",yr,sizeof yr); *year=y4(yr);
        /* strip ID3v1 "(17)" numeric genre wrapper if that's all there is */
        if(genre[0]=='('){ char*e=strchr(genre,')'); if(e&&!e[1]) genre[0]=0; }
    } else if(!memcmp(buf,"fLaC",4)){ char yr[16];
        vorbis_field((char*)buf,n,"GENRE",genre,gcap);
        vorbis_field((char*)buf,n,"DATE",yr,sizeof yr); if(!yr[0]) vorbis_field((char*)buf,n,"YEAR",yr,sizeof yr); *year=y4(yr);
    } else if(n>12 && !memcmp(buf+4,"ftyp",4)){ char yr[16];   /* M4A: tags in moov/udta/meta/ilst */
        mp4_text(buf,n,"\xA9gen",genre,gcap); mp4_text(buf,n,"\xA9day",yr,sizeof yr);
        if(!genre[0] && !yr[0]){                                /* moov at the end of the file */
            f=fopen(full,"rb"); if(f){ if(fseek(f,-(long)sizeof buf,SEEK_END)!=0) fseek(f,0,SEEK_SET);
                n=(int)fread(buf,1,sizeof buf,f); fclose(f);
                mp4_text(buf,n,"\xA9gen",genre,gcap); mp4_text(buf,n,"\xA9day",yr,sizeof yr); } }
        *year=y4(yr);
    }
}
static int is_audio(const char*n){ const char*d=strrchr(n,'.'); if(!d)return 0;
    return !strcasecmp(d,".mp3")||!strcasecmp(d,".mpga")||!strcasecmp(d,".flac")||!strcasecmp(d,".wav")||!strcasecmp(d,".m4a"); }

/* PHASE 1 — paths only (no file opens): artist = first path component under the music root.
 * Uses d_type to skip a stat() per entry, so this runs at raw directory-listing speed and
 * makes browsing usable long before the (slow) tag-reading pass finishes. */
static void walk_paths(const char*dir,int artist){
    DIR*dp=opendir(dir); if(!dp) return; struct dirent*de;
    while((de=readdir(dp)) && NE<MAXTRK){ if(de->d_name[0]=='.') continue;
        char p[1024]; snprintf(p,sizeof p,"%s/%s",dir,de->d_name);
        int isdir=0,isreg=0;
        if(de->d_type==DT_DIR) isdir=1;
        else if(de->d_type==DT_REG) isreg=1;
        else { struct stat st; if(stat(p,&st)!=0) continue; isdir=S_ISDIR(st.st_mode); isreg=S_ISREG(st.st_mode); }
        if(isdir){ int a=artist; if(artist<0) a=intern(ART,&NART,de->d_name); walk_paths(p,a); }
        else if(isreg && is_audio(de->d_name)){
            ent_t*e=&E[NE]; memset(e,0,sizeof *e);
            e->rel=strdup(rel_of(p)); e->artist=artist; e->genre=-1; e->yr=0;
            NE++;
        }
    }
    closedir(dp);
}
/* PHASE 2 — open each track and read genre + year (the slow pass). Runs in the background
 * after phase 1; browsing + artist-based mixes already work while this fills in tags. */
#define PROG_FILE "/usr/data/r3_mix.prog"   /* tag pass in progress: "<next track> <track count>" */
static void save_cache(void);
/* The tag pass reads ~24k files (tens of minutes on the R3's SD): checkpoint it, so a reboot
 * resumes where it stopped instead of starting over. */
static void enrich_all(int from){
    for(int i=from;i<NE;i++){
        char full[1024]; snprintf(full,sizeof full,"%s/%s",SD_ROOT,E[i].rel);
        char genre[200]; int yr; read_meta(full,genre,sizeof genre,&yr);
        E[i].genre = genre[0]?intern(GEN,&NGEN,genre):-1; E[i].yr=yr;
        /* leave the SD to playback: a track starting waits behind these reads */
        if((i&7)==7) usleep(15000);
        if(i%1500==1499){ save_cache(); FILE*f=fopen(PROG_FILE,"w"); if(f){ fprintf(f,"%d %d\n",i+1,NE); fclose(f); } }
    }
    remove(PROG_FILE);
}
static int enrich_resume_at(void){ FILE*f=fopen(PROG_FILE,"r"); int at=0,n=0; if(!f) return 0;
    if(fscanf(f,"%d %d",&at,&n)!=2 || n!=NE || at<0 || at>NE) at=0; fclose(f); return at; }

static void load_stats(void){ FILE*f=fopen(STATS_FILE,"r"); if(!f) return; char l[1200];
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\n")]=0; char*t1=strchr(l,'\t'); if(!t1)continue; *t1=0;
        int pl=0,sk=0; sscanf(t1+1,"%d\t%d",&pl,&sk);
        for(int i=0;i<NE;i++) if(!strcmp(E[i].rel,l)){ E[i].plays=pl; E[i].skips=sk; break; } }
    fclose(f);
}
static void save_stats(void){ FILE*f=fopen(STATS_FILE,"w"); if(!f) return;
    for(int i=0;i<NE;i++) if(E[i].plays||E[i].skips) fprintf(f,"%s\t%d\t%d\n",E[i].rel,E[i].plays,E[i].skips);
    fclose(f); g_dirty=0;
}
static void load_fav(void){ FILE*f=fopen(FAV_M3U,"r"); if(!f) return; char l[1200];
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\r\n")]=0; if(!l[0]||l[0]=='#')continue;
        for(int i=0;i<NE;i++) if(!strcmp(E[i].rel,l)){ E[i].fav=1; break; } }
    fclose(f);
}
static void load_bliss(void){ FILE*f=fopen(BLISS_FILE,"r"); if(!f) return;
    VEC=calloc((size_t)NE*VDIM,sizeof(float)); if(!VEC){ fclose(f); return; }
    char l[2048];
    while(fgets(l,sizeof l,f)){ char*t=strchr(l,'\t'); if(!t)continue; *t=0;
        int idx=-1; for(int i=0;i<NE;i++) if(!strcmp(E[i].rel,l)){ idx=i; break; } if(idx<0)continue;
        float*v=VEC+(size_t)idx*VDIM; char*s=t+1; for(int d=0;d<VDIM;d++){ v[d]=strtof(s,&s); } E[idx].vec=v; }
    fclose(f);
}

/* --- index cache: the SD scan is slow (minutes on a big card), so persist the
 * tag index to /usr/data and reload it instantly on later boots. Stored by NAME
 * (rel \t artist \t genre \t year) so it survives intern-order changes. Delete the
 * file to force a rescan after adding/removing music. --- */
static int load_cache(void){ FILE*f=fopen(IDX_CACHE,"r"); if(!f) return 0;
    char l[1200]; int n=0;
    while(fgets(l,sizeof l,f) && NE<MAXTRK){ l[strcspn(l,"\n")]=0; if(!l[0]) continue;
        char*t1=strchr(l,'\t'); if(!t1)continue; *t1=0;
        char*t2=strchr(t1+1,'\t'); if(!t2)continue; *t2=0;
        char*t3=strchr(t2+1,'\t'); if(!t3)continue; *t3=0;
        ent_t*e=&E[NE]; memset(e,0,sizeof *e);
        e->rel=strdup(l);
        e->artist = t1[1] ? intern(ART,&NART,t1+1) : -1;
        e->genre  = t2[1] ? intern(GEN,&NGEN,t2+1) : -1;
        if(t2[1]) g_cache_has_genre=1;
        e->yr=atoi(t3+1); NE++; n++; }
    fclose(f); return n;
}
static void save_cache(void){ FILE*f=fopen(IDX_CACHE,"w"); if(!f) return;
    for(int i=0;i<NE;i++){ ent_t*e=&E[i];
        fprintf(f,"%s\t%s\t%s\t%d\n", e->rel,
                 e->artist>=0?ART[e->artist]:"",
                 e->genre >=0?GEN[e->genre ]:"", e->yr); }
    fclose(f);
}

static void* idx_thread(void*a){ (void)a;
    int cached = load_cache();        /* instant on later boots */
    if(!cached) walk_paths(g_root,-1);/* phase 1: paths only, at listing speed */
    load_stats(); load_fav(); load_bliss();
    g_ready=1;                        /* browsing + artist mixes available from here */
    { FILE*r=fopen("/tmp/r3mix.ready","w"); if(r){ fprintf(r,"%d tracks ready\n",NE); fclose(r); } }
    if(!cached) save_cache();         /* persist paths right away */
    /* phase 2: enrich genre/year unless the cache already had them from this tag reader
     * (an older reader dropped or glued multi-value genres: re-read them once) */
    int full = tags_ver()>=TAGS_VER;
    g_version++;
    if(!full && NE>0){ enrich_all(cached?enrich_resume_at():0); save_cache(); g_version++; }
    if(NE>0) mark_full();
    FILE*m=fopen("/tmp/r3mix.done","w"); if(m){ fprintf(m,"%d tracks (%s), %d with vectors\n",NE,cached?"cached":"scanned",VEC?1:0); fclose(m); }
    return 0;
}
void smartmix_start(const char*music_root){
    snprintf(g_root,sizeof g_root,"%s",music_root);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,256*1024);
    pthread_create(&t,&at,idx_thread,0); pthread_attr_destroy(&at); pthread_detach(t);
}
int smartmix_ready(void){ return g_ready; }
int smartmix_version(void){ return g_ready ? g_version : 0; }
int smartmix_tags_done(void){ return tags_ver()>=TAGS_VER; }
int smartmix_each(smartmix_track_fn fn,void*u){
    if(!g_ready) return -1;
    pthread_mutex_lock(&mu);
    for(int i=0;i<NE;i++){ const ent_t*e=&E[i];
        fn(u,e->rel, e->artist>=0?ART[e->artist]:"", e->genre>=0?GEN[e->genre]:"", e->yr, e->plays, e->skips, e->fav); }
    pthread_mutex_unlock(&mu);
    return NE; }
int smartmix_count(void){ return NE; }

/* --- index-backed browsing: enumerate the index instead of hitting the slow SD --- */
/* case-insensitive FNV-1a */
static unsigned hash_ci(const char*s,int n){ unsigned h=2166136261u;
    for(int i=0;i<n;i++){ h^=(unsigned char)tolower((unsigned char)s[i]); h*=16777619u; } return h; }
/* Entries of one folder, from the index. The first path component of every track below
 * `dir` is one entry; duplicates (all the tracks of an artist) are dropped. This used to
 * compare each track with every entry found so far — O(tracks x entries), ~0.9 s for the
 * Music root with 600+ artists on the R3. Now: same as the previous track (tracks of a
 * folder are consecutive in the index) or a hash-set lookup. */
int smartmix_listdir(const char*dir,char names[][256],char*isdir,int cap){
    if(!g_ready) return -1;
    const char*r=rel_of(dir); char prefix[1024];
    if(!r[0]) prefix[0]=0; else snprintf(prefix,sizeof prefix,"%s/",r);
    size_t pfl=strlen(prefix); int n=0, last=-1;
    enum { HB=8192 };                       /* > 2 x cap: open addressing stays short */
    short *slot=calloc(HB,sizeof *slot); if(!slot) return -1;     /* entry index + 1, 0 = free */
    for(int i=0;i<NE;i++){ const char*rel=E[i].rel;
        if(pfl && strncmp(rel,prefix,pfl)) continue;
        const char*rest=rel+pfl; if(!*rest) continue;
        const char*sl=strchr(rest,'/');
        int d=sl?1:0, cl=sl?(int)(sl-rest):(int)strlen(rest); if(cl>255)cl=255;
        if(last>=0 && isdir[last]==d && !strncasecmp(names[last],rest,cl) && !names[last][cl]) continue;
        unsigned k=(hash_ci(rest,cl)*2u+(unsigned)d)&(HB-1); int dup=-1;
        while(slot[k]){ int j=slot[k]-1; if(isdir[j]==d && !strncasecmp(names[j],rest,cl) && !names[j][cl]){ dup=j; break; }
            k=(k+1)&(HB-1); }
        if(dup>=0){ last=dup; continue; }
        if(n>=cap || n>=HB/2) break;
        memcpy(names[n],rest,cl); names[n][cl]=0; isdir[n]=(char)d; slot[k]=(short)(n+1); last=n; n++;
    }
    free(slot);
    return n;
}
int smartmix_folder(const char*dir,char out[][1024],int cap){
    if(!g_ready) return -1;
    const char*r=rel_of(dir); char prefix[1024]; snprintf(prefix,sizeof prefix,"%s/",r);
    size_t pfl=strlen(prefix); int n=0;
    for(int i=0;i<NE && n<cap;i++){ if(strncmp(E[i].rel,prefix,pfl)) continue;
        if(strchr(E[i].rel+pfl,'/')) continue;   /* in a deeper subfolder */
        snprintf(out[n++],1024,"%s/%s",SD_ROOT,E[i].rel); }
    return n;
}

static void reset_index(void){
    pthread_mutex_lock(&mu);
    for(int i=0;i<NE;i++) free(E[i].rel); NE=0;
    for(int i=0;i<NART;i++) free(ART[i]); NART=0;
    for(int i=0;i<NGEN;i++) free(GEN[i]); NGEN=0;
    free(VEC); VEC=0;
    pthread_mutex_unlock(&mu);
}
static void* rescan_thread(void*a){ (void)a;
    g_ready=0;              /* browser falls back to live SD reads while we rebuild */
    reset_index(); g_cache_has_genre=0;
    remove(IDX_CACHE); remove(FULL_FLAG); remove(PROG_FILE);
    walk_paths(g_root,-1);                 /* phase 1 */
    load_stats(); load_fav(); load_bliss();
    g_ready=1; save_cache();
    g_version++;
    enrich_all(0); save_cache();           /* phase 2 */
    mark_full(); g_version++;
    FILE*m=fopen("/tmp/r3mix.done","w"); if(m){ fprintf(m,"%d tracks (rescan), %d with vectors\n",NE,VEC?1:0); fclose(m); }
    return 0;
}
void smartmix_rescan(void){
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,256*1024);
    pthread_create(&t,&at,rescan_thread,0); pthread_attr_destroy(&at); pthread_detach(t);
}
static int find(const char*full){ const char*r=rel_of(full); for(int i=0;i<NE;i++) if(!strcmp(E[i].rel,r)) return i; return -1; }
void smartmix_note(const char*full,int completed){
    int i=find(full); if(i<0) return;
    if(completed) E[i].plays++; else E[i].skips++;
    g_dirty=1;
}
void smartmix_flush(void){ if(g_dirty) save_stats(); }

static float vdist(const float*a,const float*b){ float s=0; for(int d=0;d<VDIM;d++){ float x=a[d]-b[d]; s+=x*x; } return s; }

/* rank by score (higher=better) and emit up to cap full paths, limiting repeats per artist */
typedef struct { int idx; float sc; } cand_t;
static int cmp_c(const void*a,const void*b){ float d=((cand_t*)b)->sc-((cand_t*)a)->sc; return d<0?-1:d>0?1:0; }
static int emit(cand_t*c,int nc,int seed,char out[][1024],int cap){
    qsort(c,nc,sizeof*c,cmp_c);
    int used[MAXSTR]={0}, k=0;
    for(int i=0;i<nc && k<cap;i++){ int e=c[i].idx; if(e==seed) continue;
        if(E[e].artist>=0 && E[e].artist<MAXSTR){ if(used[E[e].artist]>=3) continue; used[E[e].artist]++; }
        snprintf(out[k],1024,"%s/%s",SD_ROOT,E[e].rel); k++; }
    return k;
}
int smartmix_build(const char*seed_full,char out[][1024],int cap){
    if(NE==0) return 0;   /* works on a partial index too */
    int s=find(seed_full); if(s<0) return 0;
    snprintf(out[0],1024,"%s",seed_full);     /* seed first */
    cand_t*c=malloc(sizeof(cand_t)*NE); int nc=0;
    for(int i=0;i<NE;i++){ if(i==s) continue; float sc;
        if(E[s].vec && E[i].vec) sc = 1000.0f - vdist(E[s].vec,E[i].vec);
        else { sc=0; if(E[i].genre==E[s].genre && E[s].genre>=0) sc+=3;
               if(E[i].artist==E[s].artist) sc+=1;
               if(E[s].yr && E[i].yr && abs(E[i].yr-E[s].yr)<=5) sc+=1; }
        sc += (rand()%100)/100.0f;            /* tie-break + variety */
        c[nc].idx=i; c[nc].sc=sc; nc++; }
    int k=emit(c,nc,s,out+1,cap-1); free(c); return k+1;
}
int smartmix_for_you(char out[][1024],int cap){
    if(NE==0) return 0;   /* works on a partial index too */
    /* taste weights from favorites + plays (minus skips) */
    float gw[MAXSTR]={0}, aw[MAXSTR]={0}; double ysum=0,yn=0; int havevec=0; static float centroid[VDIM];
    for(int d=0;d<VDIM;d++) centroid[d]=0; double vw=0;
    for(int i=0;i<NE;i++){ float w=E[i].fav*3.0f + E[i].plays*1.0f - E[i].skips*0.5f; if(w<=0) continue;
        if(E[i].genre>=0&&E[i].genre<MAXSTR) gw[E[i].genre]+=w;
        if(E[i].artist>=0&&E[i].artist<MAXSTR) aw[E[i].artist]+=w;
        if(E[i].yr){ ysum+=(double)E[i].yr*w; yn+=w; }
        if(E[i].vec){ for(int d=0;d<VDIM;d++) centroid[d]+=E[i].vec[d]*w; vw+=w; havevec=1; } }
    if(vw>0) for(int d=0;d<VDIM;d++) centroid[d]/=vw;
    int meanyr = yn>0 ? (int)(ysum/yn) : 0;
    cand_t*c=malloc(sizeof(cand_t)*NE); int nc=0;
    for(int i=0;i<NE;i++){ float sc;
        if(havevec && E[i].vec) sc = 1000.0f - vdist(centroid,E[i].vec);
        else { sc = (E[i].genre>=0?gw[E[i].genre]:0)*1.0f + (E[i].artist>=0?aw[E[i].artist]:0)*0.5f;
               if(meanyr&&E[i].yr) sc += (abs(E[i].yr-meanyr)<=6?1.0f:0); }
        sc -= E[i].plays*0.3f;                 /* freshness: favour less-played */
        sc += (rand()%100)/100.0f;
        c[nc].idx=i; c[nc].sc=sc; nc++; }
    int k=emit(c,nc,-1,out,cap); free(c); return k;
}
