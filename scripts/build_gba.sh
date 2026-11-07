#!/bin/sh
# Build r3gba (Game Boy Advance player): gpSP libretro core (GPLv2, vendor/gpsp, with its
# MIPS dynarec — the GCW Zero build flags) + R3 frontend src/gba/r3gba.c, linked static.
set -e
TC=toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
G=vendor/gpsp
LC=$G/libretro/libretro-common
DEFS="-D__LIBRETRO__ -DINLINE=inline -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DFRONTEND_SUPPORTS_RGB565
      -DHAVE_DYNAREC -DMIPS_ARCH -DMIPS_HAS_R2_INSTS -DMMAP_JIT_CACHE -DSMALL_TRANSLATION_CACHE -DNDEBUG"
INC="-I $G -I $G/libretro -I $LC/include"
CORE="$ARCH -O3 -fomit-frame-pointer -ffast-math -fno-pie -ffunction-sections -fdata-sections $DEFS $INC -include src/gba/r3gba_cfg.h"
mkdir -p build/gba

CSRC="$G/main.c $G/gba_memory.c $G/savestate.c $G/input.c $G/sound.c $G/cheats.c $G/memmap.c $G/serial.c $G/gbp.c
      $G/rfu.c $G/serial_proto.c $G/libretro/libretro.c $G/gba_cc_lut.c $G/cpu_threaded.c
      $LC/compat/compat_posix_string.c $LC/compat/compat_strl.c $LC/compat/fopen_utf8.c $LC/encodings/encoding_utf.c
      $LC/file/file_path.c $LC/file/file_path_io.c $LC/streams/file_stream.c $LC/string/stdstring.c
      $LC/time/rtime.c $LC/vfs/vfs_implementation.c"
CCSRC="$G/video.cc $G/cpu.cc"
TA="vendor/tinyalsa/src/pcm.c vendor/tinyalsa/src/pcm_hw.c vendor/tinyalsa/src/pcm_plugin.c vendor/tinyalsa/src/snd_card_plugin.c vendor/tinyalsa/src/mixer.c vendor/tinyalsa/src/mixer_hw.c vendor/tinyalsa/src/mixer_plugin.c vendor/tinyalsa/src/limits.c"

OBJS=""
N=$(nproc 2>/dev/null || echo 4); i=0
obj(){ echo "build/gba/$(echo "$1" | tr '/' '_' | sed 's/\.[a-zA-Z]*$//').o"; }
for s in $CSRC; do o=$(obj "$s"); OBJS="$OBJS $o"
  ( ${TC}gcc $CORE -w -c "$s" -o "$o" ) & i=$((i+1)); [ $((i % N)) -eq 0 ] && wait; done
for s in $CCSRC; do o=$(obj "$s"); OBJS="$OBJS $o"
  ( ${TC}g++ $CORE -fno-rtti -fno-exceptions -std=c++11 -w -c "$s" -o "$o" ) & i=$((i+1)); [ $((i % N)) -eq 0 ] && wait; done
# The stub keeps a GBA register in $gp, so it must call C with plain jal, not through the
# GOT: assemble it without abicalls (musl's default). Same for the JIT, which emits jal.
for s in $G/mips/mips_stub.S $G/bios_data.S; do o=$(obj "$s"); OBJS="$OBJS $o"
  ${TC}gcc $ARCH -fno-pie -mno-abicalls $DEFS $INC -Wa,-I$G -c "$s" -o "$o"; done
wait
for o in $OBJS; do [ -s "$o" ] || { echo "missing $o (compile error above)"; exit 1; }; done

FE="$ARCH -O2 -fno-pie -ffunction-sections -fdata-sections -I $G/libretro -I $LC/include -I src/ui -I src/common -I vendor/tinyalsa/include"
${TC}gcc $FE -Wall -Wno-misleading-indentation -c src/gba/r3gba.c -o build/gba/r3gba.o
${TC}gcc $FE -c src/ui/pad.c -o build/gba/pad.o
OBJS="$OBJS build/gba/r3gba.o build/gba/pad.o"
for s in $TA; do o=build/gba/ta_$(basename "$s" .c).o; ${TC}gcc $FE -w -c "$s" -o "$o"; OBJS="$OBJS $o"; done
${TC}g++ $ARCH -fno-pie -o build/r3gba $OBJS -static -no-pie -Wl,--gc-sections -lm -lpthread
${TC}strip build/r3gba
ls -la build/r3gba && file build/r3gba
