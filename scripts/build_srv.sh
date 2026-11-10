#!/bin/sh
# Build r3srv (UPnP/DLNA + HTTP music server for the library) for the R3.
set -e
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
mkdir -p build
${TC}gcc $ARCH -O2 -fno-pie -no-pie -static -Wall -Wno-misleading-indentation -Wno-format-truncation \
    -I src/ui -o build/r3srv src/net/r3srv.c src/ui/explore.c src/ui/genre.c -lm
${TC}strip build/r3srv
ls -la build/r3srv && file build/r3srv
