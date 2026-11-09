/* drum_music — find the beat of a song (pure C, integer maths: the R3 has no FPU; host-tested:
 * tests/drum_music_test.c). Feed mono samples, then ask for the tempo and phase.
 *
 * Onset envelope: mean |x| over 5 ms hops, rectified rise against a short running mean.
 * Tempo: autocorrelation of the envelope over 80..170 BPM (coarse, with a 2x-period bonus so the
 * base pulse wins over its halves), then a fine search of the period in 1/100-sample steps that
 * maximises the onset energy found on a beat grid across the whole analysed span — that is what
 * keeps a 5-minute mission from drifting. Phase comes from the same search. */
#ifndef DRUM_MUSIC_H
#define DRUM_MUSIC_H
#include <stdint.h>

#define DM_MAXHOPS (200*60*8)            /* 8 minutes of 5 ms hops */
typedef struct {
    int rate, hop;                       /* sample rate, samples per hop */
    int64_t acc; int fill;               /* current hop */
    int32_t prev, avg;                   /* envelope history */
    int n;                               /* hops so far */
    uint16_t on[DM_MAXHOPS];             /* onset strength per hop */
} dm_t;
typedef struct { int ok; int64_t period_x100; int64_t phase; int bpm_x10; int confidence; } dm_beat_t;
/* period_x100: samples per beat x100 (at dm.rate); phase: sample (from the start) of a beat;
 * confidence: 0..100, ~>25 means a steady beat */

void dm_init(dm_t*d,int rate);
void dm_feed(dm_t*d,const int16_t*mono,int n);
dm_beat_t dm_estimate(const dm_t*d);
#endif
