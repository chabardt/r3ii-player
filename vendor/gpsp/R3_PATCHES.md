# gpSP for r3gba

Source: https://github.com/libretro/gpsp at `5819380c2ffb0900219d700a382ee68c464ebb99`
(GPLv2, see COPYING). Only the files the MIPS Linux build needs are kept
(no 3ds/x86/arm/jni/tests). The open-source BIOS in `bios/` is GPLv2 too (see bios/README.md).
Built by `scripts/build_gba.sh` with the GCW Zero flags (MIPS dynarec), plus
`-DMMAP_JIT_CACHE -DSMALL_TRANSLATION_CACHE` and `-include src/gba/r3gba_cfg.h`
(run-time ROM buffer size: the R3 has 56 MB of RAM).

Local changes (all worth sending upstream):

1. `memmap.c` — `_VALIDATE_BLOCK_FN` for MIPS passed 2 arguments to the 3-argument
   `validate_addr_section_mips()`: MIPS + `MMAP_JIT_CACHE` did not compile.
2. `mips/mips_emit.h` `arm_psr_load_new_imm()` — MSR with an immediate ignored the
   immediate's rotation (`msr cpsr_f, #0x40000000` stored 0x01), so flags written by
   MSR were lost. x86 and ARM32 backends rotate; ARM64 has the same bug.
   jsmolka gba-tests arm.gba: failed test 001 -> passes it.
3. `mips/mips_emit.h` `generate_shift_reg()` — register-specified shifts must use only
   the low byte of Rs (`1 lsl 0xF10` = `1 lsl 16`); the emitters compared the whole
   register with 32. Now masked into `a1` first. arm.gba: failed 167 -> reaches 224
   (gpSP's own interpreter stops at 225; 224/225 are PC-as-shifted-register oddities).

Test ROMs used: https://github.com/jsmolka/gba-tests (MIT), on the device:
`r3gba arm.gba --fresh --bench 300` writes the last frame to /tmp/r3gba.ppm.
