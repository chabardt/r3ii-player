# Toolchain

Cross-compiler for the HiBy R3 II (Ingenic MIPS32r2, O32 **hard-float**, LE,
Linux 3.10, glibc 2.16). We build **fully-static** binaries, so the device's old
glibc is irrelevant.

## Fetch (not committed — ~100 MB tarball, ~400 MB extracted)

```sh
cd toolchain
curl -fL -o tc.tgz https://musl.cc/mipsel-linux-musl-cross.tgz
# verify:
echo "82626533bf7e677c225e7cbedf1d5b0d6bc60c3daaf28249e54f0eb805d89b13  tc.tgz" | sha256sum -c
tar xzf tc.tgz        # -> mipsel-linux-musl-cross/
```

Then, from the repo root:

```sh
. toolchain/env.sh    # sets CC/CXX/AR/STRIP + R3_CFLAGS/R3_LDFLAGS
```

## Verified ABI

A test binary built with `$R3_CFLAGS $R3_LDFLAGS` has `e_flags=0x70001007`
(mips32r2 / O32 / hard-float), static EXEC — matching the stock `hiby_player`
(`0x70001005`; the only difference is the harmless PIC bit).

gcc version used: 11.2.1 (musl.cc build).
