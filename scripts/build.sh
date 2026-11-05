#!/bin/sh
# Build r3player (LVGL UI + audio) for HiBy R3 Pro II — no make/cmake needed.
set -e
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
CC=${TC}gcc
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
INC="-DLV_CONF_INCLUDE_SIMPLE -I vendor/lvgl -I src/ui -I vendor/tinyalsa/include -I vendor/minimp3 -I vendor/dr_libs -I vendor/minimp4 -I vendor/faad2/include -I vendor/faad2/libfaad -I vendor/lvgl/src/extra/libs/sjpg -I vendor/stb"
CFLAGS="$ARCH -DHAVE_CONFIG_H -O2 -ffunction-sections -fdata-sections -fno-pie $INC"
mkdir -p build

TA="vendor/tinyalsa/src/pcm.c vendor/tinyalsa/src/pcm_hw.c vendor/tinyalsa/src/pcm_plugin.c vendor/tinyalsa/src/snd_card_plugin.c vendor/tinyalsa/src/mixer.c vendor/tinyalsa/src/mixer_hw.c vendor/tinyalsa/src/mixer_plugin.c vendor/tinyalsa/src/limits.c"
FAAD="$(find vendor/faad2/libfaad -name '*.c')"
SRCS="$(find vendor/lvgl/src -name '*.c') $TA $FAAD src/ui/audio.c src/ui/m4a.c src/ui/bt.c src/ui/cover.c src/ui/game.c src/ui/hiscore.c src/ui/brick_core.c src/ui/brick.c src/ui/coil_core.c src/ui/coil.c src/ui/pet_core.c src/ui/pet.c src/ui/pad.c src/ui/wifi.c src/ui/share.c src/ui/heart_img.c src/ui/main.c"

N=$(nproc 2>/dev/null || echo 4); i=0
for s in $SRCS; do
  ( $CC $CFLAGS -c "$s" -o "$s.o" ) &
  i=$((i+1)); [ $((i % N)) -eq 0 ] && wait
done
wait
# recompile our own files in foreground so errors abort the build (no stale relink)
$CC $CFLAGS -c src/ui/audio.c -o src/ui/audio.c.o
$CC $CFLAGS -c src/ui/bt.c   -o src/ui/bt.c.o
$CC $CFLAGS -c src/ui/cover.c -o src/ui/cover.c.o
$CC $CFLAGS -c src/ui/game.c  -o src/ui/game.c.o
for f in hiscore brick_core brick coil_core coil pet_core pet pad wifi share heart_img; do $CC $CFLAGS -c src/ui/$f.c -o src/ui/$f.c.o; done
$CC $CFLAGS -c src/ui/main.c  -o src/ui/main.c.o

OBJS=""; for s in $SRCS; do OBJS="$OBJS $s.o"; done
$CC $ARCH -fno-pie -o build/r3player $OBJS -static -no-pie -Wl,--gc-sections -lpthread -lm
${TC}strip build/r3player 2>/dev/null || true
ls -la build/r3player && file build/r3player
