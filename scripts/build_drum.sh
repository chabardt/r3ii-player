#!/bin/sh
# Build r3drum (Drumfolk, rhythm-marching battles): src/drum + pad.c + tinyalsa, static.
# Host tests first: tests/drum_core_test.c and tests/drum_game_test.c (see their headers).
set -e
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
CFLAGS="$ARCH -O2 -fno-pie -ffunction-sections -fdata-sections -I src/drum -I src/ui -I src/common -I vendor/tinyalsa/include -I vendor/minimp3 -I vendor/dr_libs"
mkdir -p build/drum
TA="vendor/tinyalsa/src/pcm.c vendor/tinyalsa/src/pcm_hw.c vendor/tinyalsa/src/pcm_plugin.c vendor/tinyalsa/src/snd_card_plugin.c vendor/tinyalsa/src/mixer.c vendor/tinyalsa/src/mixer_hw.c vendor/tinyalsa/src/mixer_plugin.c vendor/tinyalsa/src/limits.c"
OBJS=""
for s in src/drum/drum_core.c src/drum/drum_game.c src/drum/drum_music.c src/drum/r3drum.c; do o=build/drum/$(basename "$s" .c).o
  ${TC}gcc $CFLAGS -Wall -Wno-misleading-indentation -Wno-deprecated-declarations -c "$s" -o "$o"; OBJS="$OBJS $o"; done
${TC}gcc $CFLAGS -c src/ui/pad.c -o build/drum/pad.o; OBJS="$OBJS build/drum/pad.o"
${TC}gcc $CFLAGS -w -c src/drum/drum_decode.c -o build/drum/drum_decode.o; OBJS="$OBJS build/drum/drum_decode.o"
for s in $TA; do o=build/drum/ta_$(basename "$s" .c).o; ${TC}gcc $CFLAGS -w -c "$s" -o "$o"; OBJS="$OBJS $o"; done
${TC}gcc $ARCH -fno-pie -o build/r3drum $OBJS -static -no-pie -Wl,--gc-sections -lm -lpthread
${TC}strip build/r3drum
ls -la build/r3drum && file build/r3drum
