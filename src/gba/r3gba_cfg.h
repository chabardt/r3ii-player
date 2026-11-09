/* Force-included into every gpSP C/C++ file (scripts/build_gba.sh: -include).
 * The R3 has 56 MB of RAM (about 25 MB free while r3player waits in the background),
 * so the ROM buffer is sized at run time by r3gba.c before retro_init(): the whole
 * ROM when it fits, otherwise gpSP pages 32 KB blocks from the file (LRU). */
#ifndef R3GBA_CFG_H
#define R3GBA_CFG_H
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned r3gba_rom_mb;
#ifdef __cplusplus
}
#endif
#define ROM_BUFFER_SIZE r3gba_rom_mb
#endif
