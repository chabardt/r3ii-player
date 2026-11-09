/* drum_decode — read a song (MP3 / FLAC) as 16-bit stereo frames, for Drumfolk's MY MUSIC beat.
 * Same decoders as r3player (minimp3, dr_flac). */
#ifndef DRUM_DECODE_H
#define DRUM_DECODE_H
#include <stdint.h>
typedef struct { int kind, rate, ch; void*h; int16_t*tmp; } dec_t;   /* own scratch: two threads decode at once */
int  dec_open(dec_t*d,const char*path);              /* 0 on success */
int  dec_read(dec_t*d,int16_t*stereo,int frames);    /* frames read (0 at the end) */
void dec_close(dec_t*d);
int  dec_is_song(const char*name);                   /* .mp3 / .flac */
#endif
