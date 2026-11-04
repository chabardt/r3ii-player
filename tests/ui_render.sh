#!/bin/sh
# Build the host UI render harness (LVGL on the PC) and render screens to build/shots/*.png
set -e
mkdir -p build/hostobj build/shots
INC="-DLV_CONF_INCLUDE_SIMPLE -I vendor/lvgl -I src/ui"
OBJS=""
for s in $(find vendor/lvgl/src -name '*.c'); do o=build/hostobj/$(echo "$s" | tr '/' '_').o; OBJS="$OBJS $o"
  [ -f "$o" ] || gcc -O1 -w $INC -c "$s" -o "$o"; done
SAVE=build/shots/pet.sav
gcc -O1 -w $INC -DPET_FILE=\"$SAVE\" '-Dtime(x)=fake_time(x)' -c src/ui/pet.c -o build/hostobj/pet.o
gcc -O1 -w $INC -c src/ui/pet_core.c -o build/hostobj/pet_core.o
gcc -O1 -w $INC '-Dtime(x)=fake_time(x)' -c src/ui/brick.c -o build/hostobj/brick.o
gcc -O1 -w $INC -c src/ui/brick_core.c -o build/hostobj/brick_core.o
gcc -O1 -w $INC '-Dtime(x)=fake_time(x)' -c src/ui/coil.c -o build/hostobj/coil.o
gcc -O1 -w $INC -c src/ui/coil_core.c -o build/hostobj/coil_core.o
gcc -O1 -w $INC -c src/ui/hiscore.c -o build/hostobj/hiscore.o
gcc -O1 -w $INC -DPET_FILE=\"$SAVE\" -o build/ui_render tests/ui_render.c $OBJS \
    build/hostobj/pet.o build/hostobj/pet_core.o build/hostobj/brick.o build/hostobj/brick_core.o build/hostobj/coil.o build/hostobj/coil_core.o build/hostobj/hiscore.o -lz -lm
for s in egg baby child day bramble grub night dark elder brick coil; do build/ui_render $s build/shots/$s.png; done
