#!/bin/sh
# scripts/shot.sh OUT.png — grab the R3 framebuffer (480x720 RGB565, first page) as PNG.
# (stock adbd has no exec-out: dd to /tmp on the device, then pull)
# Inject touches with src/bringup/tap.c:  adb shell /usr/data/r3tap X Y [hold_ms]
set -e
OUT=${1:-shot.png}; RAW=$(mktemp)
adb shell "dd if=/dev/fb0 of=/tmp/fb.raw bs=691200 count=1" >/dev/null 2>&1
adb pull /tmp/fb.raw "$RAW" >/dev/null
python3 -I - "$RAW" "$OUT" <<'EOF'
import sys,struct,zlib
raw=open(sys.argv[1],'rb').read()[:480*720*2]
rows=[]
for y in range(720):
    r=bytearray(b'\0')
    for x in range(480):
        v=raw[(y*480+x)*2]|raw[(y*480+x)*2+1]<<8
        r+=bytes(((v>>11&31)*255//31,(v>>5&63)*255//63,(v&31)*255//31))
    rows.append(bytes(r))
def ch(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',480,720,8,2,0,0,0))+ch(b'IDAT',zlib.compress(b''.join(rows)))+ch(b'IEND',b'')
open(sys.argv[2],'wb').write(png)
EOF
rm -f "$RAW"; echo "$OUT"
