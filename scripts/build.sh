#!/bin/sh
set -e
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
CC=${TC}gcc
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
CFLAGS="$ARCH -O2 -ffunction-sections -fdata-sections -fno-pie -DLV_CONF_INCLUDE_SIMPLE -I vendor/lvgl -I src/ui"
mkdir -p build
SRCS="$(find vendor/lvgl/src -name '*.c') src/ui/main.c"
N=$(nproc 2>/dev/null || echo 4); i=0; fail=0
for s in $SRCS; do
  ( $CC $CFLAGS -c "$s" -o "$s.o" ) &
  i=$((i+1)); [ $((i % N)) -eq 0 ] && wait
done
wait
OBJS=""; for s in $SRCS; do OBJS="$OBJS $s.o"; done
$CC $ARCH -fno-pie -o build/r3player $OBJS -static -no-pie -Wl,--gc-sections -lm
${TC}strip build/r3player 2>/dev/null || true
ls -la build/r3player && file build/r3player
