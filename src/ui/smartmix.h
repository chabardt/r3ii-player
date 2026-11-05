#ifndef R3_SMARTMIX_H
#define R3_SMARTMIX_H
/* On-device "smart mix": suggests songs from the local library similar to a seed track.
 * Uses a bliss acoustic-feature index when present (built on a PC, see scripts/), otherwise
 * falls back to tag similarity (genre + era + artist) from an index built on the device. */

void smartmix_start(const char *music_root);   /* kick off background indexing (once) */
int  smartmix_ready(void);                      /* index built yet? */
int  smartmix_count(void);                      /* tracks indexed */
/* Build a mix of up to `cap` FULL track paths similar to seed_full (seed first).
 * out is caller-allocated char[cap][1024]. Returns the number of tracks written. */
int  smartmix_build(const char *seed_full, char out[][1024], int cap);
/* "For You" mix from the listening profile (favorites + plays - skips). */
int  smartmix_for_you(char out[][1024], int cap);
void smartmix_note(const char *full, int completed); /* record a play (1) or skip (0) */
void smartmix_flush(void);                      /* persist stats if changed */

/* --- index-backed browsing (the SD is slow; serve Music menus from the index) --- */
/* Immediate children of `dir` (subfolders + audio files), from the index.
 * names[cap][256] + isdir[cap] are caller-allocated. Returns count, or -1 if the
 * index isn't ready yet (caller should fall back to a live directory read). */
int  smartmix_listdir(const char *dir, char names[][256], char *isdir, int cap);
/* Full paths of audio files directly in `dir` (unsorted). Returns count, or -1 if
 * the index isn't ready. */
int  smartmix_folder(const char *dir, char out[][1024], int cap);
/* Forget the cache and re-scan the SD in the background (Settings "Rescan library"). */
void smartmix_rescan(void);
#endif
