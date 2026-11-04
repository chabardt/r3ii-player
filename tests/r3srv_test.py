#!/usr/bin/env python3
"""Exercise r3srv like a UPnP client (VLC): SSDP discovery, description, Browse, file range.
   python3 -I tests/r3srv_test.py PORT"""
import socket, sys, urllib.request, re, html

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8200
ok = True
def check(cond, what):
    global ok
    print(("PASS " if cond else "FAIL ") + what); ok &= bool(cond)

# 1. SSDP M-SEARCH for media servers
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
       "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n").encode()
s.sendto(msg, ("239.255.255.250", 1900))
loc = None
try:
    while True:
        data, _ = s.recvfrom(2048); t = data.decode(errors="replace")
        m = re.search(r"LOCATION: (http://\S+)", t, re.I)
        if m and "r3srv" in t: loc = m.group(1); break
except socket.timeout:
    pass
check(loc is not None, f"SSDP answer with LOCATION ({loc})")
if not loc:
    loc = f"http://127.0.0.1:{port}/desc.xml"

# 2. device description
d = urllib.request.urlopen(loc, timeout=5).read().decode()
check("MediaServer:1" in d and "/ctl/cds" in d, "device description advertises ContentDirectory")
base = loc.rsplit("/", 1)[0]

def browse(oid, flag="BrowseDirectChildren"):
    body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            '<u:Browse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">'
            f'<ObjectID>{html.escape(oid)}</ObjectID><BrowseFlag>{flag}</BrowseFlag><Filter>*</Filter>'
            '<StartingIndex>0</StartingIndex><RequestedCount>0</RequestedCount><SortCriteria></SortCriteria>'
            '</u:Browse></s:Body></s:Envelope>').encode()
    r = urllib.request.Request(base + "/ctl/cds", data=body, headers={
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPACTION": '"urn:schemas-upnp-org:service:ContentDirectory:1#Browse"'})
    try:
        x = urllib.request.urlopen(r, timeout=5).read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    return 200, html.unescape(re.search(r"<Result>(.*)</Result>", x, re.S).group(1))

code, root = browse("0")
check(code == 200 and 'id="Music"' in root and 'id="Others"' in root, "Browse root lists Music and Others")
code, album = browse("Music/Air/Moon Safari")
check(code == 200 and "musicTrack" in album, "Browse album lists tracks")
print("note: album art " + ("advertised" if "albumArtURI" in album else "not advertised (no cover file next to the tracks)"))
res = re.search(r"<res[^>]*>([^<]+)</res>", album)
check(res is not None, "track has a <res> URL")
if res:
    req = urllib.request.Request(res.group(1), headers={"Range": "bytes=0-99"})
    r = urllib.request.urlopen(req, timeout=5)
    check(r.status == 206 and len(r.read()) == 100, "track URL streams with Range")
code, meta = browse("Music/Air", "BrowseMetadata")
check(code == 200 and "storageFolder" in meta, "BrowseMetadata on a folder")
code, bad = browse("../etc")
check(code == 500 and "701" in bad, "Browse of an unsafe ID -> UPnP error 701")
print("ALL OK" if ok else "FAILED"); sys.exit(0 if ok else 1)
