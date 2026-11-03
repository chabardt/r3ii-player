# r3ii-player — cross-build the LVGL UI for HiBy R3 Pro II (Ingenic X1600, mipsel)
TC   := toolchain/mipsel-linux-musl-cross/bin/mipsel-linux-musl-
CC   := $(TC)gcc
ARCH := -march=mips32r2 -mabi=32 -mhard-float -EL
CFLAGS := $(ARCH) -O2 -ffunction-sections -fdata-sections -fno-pie \
          -DLV_CONF_INCLUDE_SIMPLE -I vendor/lvgl -I src/ui
LDFLAGS := -static -no-pie -Wl,--gc-sections -lm

SRC := $(shell find vendor/lvgl/src -name '*.c') src/ui/main.c
OBJ := $(SRC:.c=.o)
OUT := build/r3player

$(OUT): $(OBJ)
	@mkdir -p build
	$(CC) $(ARCH) -fno-pie -o $@ $(OBJ) $(LDFLAGS)
	$(TC)strip $@ || true
	@ls -la $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: clean
