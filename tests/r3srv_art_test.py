#!/usr/bin/env python3
"""Embedded album art in r3srv: build tiny FLAC / MP3 (ID3v2.3 + v2.4) / M4A files with known
pictures, run the host server on them and check /art/ returns exactly those bytes and that
Browse advertises it.
   gcc -O2 -o build/r3srv_host src/net/r3srv.c && python3 -I tests/r3srv_art_test.py"""
import os, struct, subprocess, sys, time, urllib.request, html, re, shutil

ROOT = os.path.abspath("build/arttest"); PORT = 18321
JPG = b"\xff\xd8\xff\xe0" + b"JPEGDATA" * 50 + b"\xff\xd9"
PNG = b"\x89PNG\r\n\x1a\n" + b"PNGDATA" * 40
BACK = b"\xff\xd8\xff\xe0" + b"BACKCOVER" * 20 + b"\xff\xd9"
ok = True
def check(c, what):
    global ok; print(("PASS " if c else "FAIL ") + what); ok &= bool(c)

def flac(pics):                      # pics: [(ptype, data)]
    blocks = [(0, b"\0" * 34)]       # STREAMINFO
    for t, d in pics:
        m = b"image/jpeg"; desc = b"a cover"
        body = struct.pack(">II", t, len(m)) + m + struct.pack(">I", len(desc)) + desc + struct.pack(">IIIII", 500, 500, 24, 0, len(d)) + d
        blocks.append((6, body))
    out = b"fLaC"
    for i, (t, b) in enumerate(blocks):
        out += bytes([(0x80 if i == len(blocks) - 1 else 0) | t]) + len(b).to_bytes(3, "big") + b
    return out + b"\xff\xf8" + b"\0" * 64            # fake audio frames

def syncsafe(n): return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])
def id3(ver, pics):                  # pics: [(ptype, enc, data)]
    frames = b"TIT2" + (6).to_bytes(4, "big") + b"\0\0" + b"\0Title"
    for t, enc, d in pics:
        desc = "Cover".encode("utf-16") + b"\0\0" if enc == 1 else b"Cover\0"
        body = bytes([enc]) + b"image/jpeg\0" + bytes([t]) + desc + d
        size = syncsafe(len(body)) if ver == 4 else len(body).to_bytes(4, "big")
        frames += b"APIC" + size + b"\0\0" + body
    frames += b"\0" * 100            # padding
    return b"ID3" + bytes([ver, 0, 0]) + syncsafe(len(frames)) + frames + b"\xff\xfb\x90\x00" + b"\0" * 400

def atom(name, payload): return struct.pack(">I", 8 + len(payload)) + name + payload
def m4a(d):
    data = atom(b"data", struct.pack(">II", 14, 0) + d)
    meta = atom(b"meta", b"\0\0\0\0" + atom(b"hdlr", b"\0" * 25) + atom(b"ilst", atom(b"covr", data)))
    moov = atom(b"moov", atom(b"mvhd", b"\0" * 100) + atom(b"udta", meta))
    return atom(b"ftyp", b"M4A \0\0\0\0M4A mp42isom") + moov + atom(b"mdat", b"\0" * 300)

shutil.rmtree(ROOT, ignore_errors=True)
files = {
    "Music/A/Album1/01 front.flac": (flac([(4, BACK), (3, JPG)]), JPG),     # front cover wins
    "Music/A/Album1/02 none.flac": (flac([]), None),
    "Music/A/Album2/01 v23.mp3": (id3(3, [(3, 1, JPG)]), JPG),               # UTF-16 description
    "Music/A/Album2/02 v24.mp3": (id3(4, [(0, 0, PNG)]), PNG),
    "Music/A/Album3/01 song.m4a": (m4a(PNG), PNG),
    "Music/A/Album4/01 none.mp3": (b"\xff\xfb\x90\x00" + b"\0" * 400, None),
}
for rel, (data, _) in files.items():
    p = os.path.join(ROOT, rel); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "wb").write(data)
open(os.path.join(ROOT, "Music/A/Album4/cover.jpg"), "wb").write(JPG)        # cover file wins for its folder

srv = subprocess.Popen(["build/r3srv_host", "-r", ROOT, "-p", str(PORT), "-u", ROOT + "/uuid"], stderr=subprocess.DEVNULL)
time.sleep(0.5)
try:
    base = f"http://127.0.0.1:{PORT}"
    def get(path):
        try: r = urllib.request.urlopen(base + urllib.request.pathname2url(path), timeout=5); return r.status, r.headers.get("Content-Type"), r.read()
        except urllib.error.HTTPError as e: return e.code, None, b""
    for rel, (_, want) in files.items():
        code, ctype, body = get("/art/" + rel)
        if want is None: check(code == 404, f"/art {rel}: no picture -> 404")
        else: check(code == 200 and body == want and ctype == ("image/png" if want == PNG else "image/jpeg"), f"/art {rel}: exact picture ({ctype})")
    check(get("/art/../etc/passwd")[0] == 404, "/art refuses unsafe paths")

    def browse(oid):
        body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
                '<u:Browse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">'
                f'<ObjectID>{html.escape(oid)}</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter>'
                '<StartingIndex>0</StartingIndex><RequestedCount>0</RequestedCount><SortCriteria></SortCriteria></u:Browse></s:Body></s:Envelope>').encode()
        r = urllib.request.Request(base + "/ctl/cds", data=body, headers={"Content-Type": 'text/xml; charset="utf-8"',
            "SOAPACTION": '"urn:schemas-upnp-org:service:ContentDirectory:1#Browse"'})
        x = urllib.request.urlopen(r, timeout=5).read().decode()
        return html.unescape(re.search(r"<Result>(.*)</Result>", x, re.S).group(1))
    albums = browse("Music/A")
    arts = dict(re.findall(r'<container id="([^"]+)".*?(?:<upnp:albumArtURI>([^<]*)</upnp:albumArtURI>)?</container>', albums, re.S))
    check("/art/Music/A/Album1/01%20front.flac" in arts.get("Music/A/Album1", ""), "album art for a folder from its first track")
    check("/art/Music/A/Album3/01%20song.m4a" in arts.get("Music/A/Album3", ""), "album art from an M4A")
    check("/f/Music/A/Album4/cover.jpg" in arts.get("Music/A/Album4", ""), "cover file preferred for a folder")
    tracks = browse("Music/A/Album1")
    items = re.findall(r'<item id="([^"]+)".*?</item>', tracks, re.S)
    check(len(items) == 2 and tracks.count("<upnp:albumArtURI>") == 1, "per-track art only where a track has one")
    for u in re.findall(r"<upnp:albumArtURI>([^<]*)</upnp:albumArtURI>", albums):
        check(urllib.request.urlopen(u, timeout=5).status == 200, f"advertised URL loads: {u.split('/',3)[3]}")
finally:
    srv.terminate(); srv.wait()
print("ALL OK" if ok else "FAILED"); sys.exit(0 if ok else 1)
