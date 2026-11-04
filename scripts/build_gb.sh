#!/bin/sh
# Build r3gb (Game Boy player): Peanut-GB + minigb_apu (MIT, vendor/peanut_gb) + R3 frontend.
set -e
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
CFLAGS="$ARCH -O2 -fno-pie -ffunction-sections -fdata-sections -I vendor/peanut_gb -I src/ui -I vendor/tinyalsa/include"
DEFS="-DMINIGB_APU_AUDIO_FORMAT_S16SYS -DAUDIO_SAMPLE_RATE=48000"
mkdir -p build
TA="vendor/tinyalsa/src/pcm.c vendor/tinyalsa/src/pcm_hw.c vendor/tinyalsa/src/pcm_plugin.c vendor/tinyalsa/src/snd_card_plugin.c vendor/tinyalsa/src/mixer.c vendor/tinyalsa/src/mixer_hw.c vendor/tinyalsa/src/mixer_plugin.c vendor/tinyalsa/src/limits.c"
${TC}gcc $CFLAGS $DEFS -w -c vendor/peanut_gb/minigb_apu.c -o build/gb_apu.o
${TC}gcc $CFLAGS -c src/ui/pad.c -o build/gb_pad.o
${TC}gcc $CFLAGS $DEFS -Wall -Wno-misleading-indentation -c src/gb/r3gb.c -o build/gb_main.o
OBJS="build/gb_apu.o build/gb_pad.o build/gb_main.o"
for s in $TA; do o=build/gb_$(basename "$s" .c).o; ${TC}gcc $CFLAGS -w -c "$s" -o "$o"; OBJS="$OBJS $o"; done
${TC}gcc $ARCH -fno-pie -o build/r3gb $OBJS -static -no-pie -Wl,--gc-sections -lm -lpthread
${TC}strip build/r3gb
ls -la build/r3gb && file build/r3gb
