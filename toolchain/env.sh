# Source this to build r3ii-player for the HiBy R3 II.
#   . toolchain/env.sh
# Toolchain: musl mipsel cross-gcc (see toolchain/README.md to fetch).
# Target verified against stock hiby_player ABI: mips32r2 / O32 / hard-float / LE.

R3_TC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/mipsel-linux-musl-cross"
export PATH="$R3_TC_DIR/bin:$PATH"
export CROSS=mipsel-linux-musl-
export CC=${CROSS}gcc
export CXX=${CROSS}g++
export AR=${CROSS}ar
export STRIP=${CROSS}strip

# ABI-matching flags (produce e_flags 0x70001007, EXEC, static).
export R3_ARCH="-march=mips32r2 -mabi=32 -mhard-float -EL"
export R3_CFLAGS="$R3_ARCH -O2 -ffunction-sections -fdata-sections -fno-pie"
export R3_LDFLAGS="-static -no-pie -Wl,--gc-sections"

echo "r3ii toolchain ready: $($CC --version | head -1)"
echo "CFLAGS=$R3_CFLAGS"
echo "LDFLAGS=$R3_LDFLAGS"
