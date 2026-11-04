#!/bin/sh
# Build r3doom: doomgeneric (GPLv2, vendor/doomgeneric) + the R3 backend (src/doom/).
#   sh scripts/build_doom.sh            -> build/r3doom  (static mipsel, for the R3)
#   sh scripts/build_doom.sh host T.c   -> build/dg_host (native, with test backend T.c)
set -e
DG=vendor/doomgeneric
SRCS=""
for f in $DG/*.c; do case "$f" in *doomgeneric_*) ;; *) SRCS="$SRCS $f";; esac; done
DEFS="-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200"
mkdir -p build
if [ "$1" = host ]; then
  gcc -O1 -w $DEFS -I $DG -o build/dg_host $SRCS "$2" -lm
  ls -la build/dg_host; exit 0
fi
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
CFLAGS="$ARCH -O2 -fno-pie -ffunction-sections -fdata-sections -w $DEFS -I $DG -I vendor/tinyalsa/include"
N=$(nproc 2>/dev/null || echo 4); i=0; OBJS=""
for s in $SRCS; do o=build/dg_$(basename "$s" .c).o; OBJS="$OBJS $o"
  ( ${TC}gcc $CFLAGS -c "$s" -o "$o" ) & i=$((i+1)); [ $((i % N)) -eq 0 ] && wait
done
wait
${TC}gcc $CFLAGS -Wall -c src/doom/r3doom.c -o build/r3doom_main.o
${TC}gcc $ARCH -fno-pie -o build/r3doom $OBJS build/r3doom_main.o -static -no-pie -Wl,--gc-sections -lm
${TC}strip build/r3doom
ls -la build/r3doom && file build/r3doom
