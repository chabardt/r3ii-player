/* drum_core — the rhythm rules of Drumfolk (pure C, host-tested: tests/drum_core_test.c).
 *
 * Time is in audio samples at DR_RATE. Beat b sounds at t0 + b*beat. The player plays four
 * drum hits on four consecutive beats; if they spell a command, the squad performs it during
 * the next four beats (the "response"), and the next command must start on the beat right
 * after the response to keep the streak. A streak of DR_FEVER_AT commands is "fever".
 * Hits during a response, off-beat hits, wrong patterns or a missed restart break the streak. */
#ifndef DRUM_CORE_H
#define DRUM_CORE_H
#include <stdint.h>

#define DR_RATE 48000
enum { DR_DUM, DR_KA, DR_TOK, DR_CHI, DR_NDRUMS };
enum { CMD_NONE=-1, CMD_MARCH, CMD_STRIKE, CMD_GUARD, CMD_RETREAT, CMD_RALLY, CMD_SPECIAL, CMD_N };
enum { J_NONE, J_PERFECT, J_GOOD, J_MISS };
#define DR_FEVER_AT 4
#define DR_SUPER_AT 8           /* super fever; SPECIAL (CHI KA CHI KA) only works then, and spends it */
#define DR_PERFECT_MS 50
#define DR_GOOD_MS 110

extern const unsigned char DR_PATTERN[CMD_N][4];
extern const char *const DR_CMD_NAME[CMD_N];
extern const char *const DR_DRUM_NAME[DR_NDRUMS];

typedef struct {
    int64_t t0, beat;          /* sample of beat 0, samples per beat */
    unsigned char seq[4]; int64_t seq_beat[4]; int nseq;   /* hits of the command being entered */
    int resp_cmd; int64_t resp_beat;                       /* performing resp_cmd on beats resp_beat..+3 */
    int64_t next_start;        /* beat the next command must start on to keep the streak (-1: none) */
    int combo, fever, super;
    /* events for the frontend, set by rc_hit / rc_tick, cleared by the caller */
    int ev_judge;              /* J_* of the last hit */
    int ev_cmd;                /* a command was recognised (CMD_*), its response starts next beat */
    int ev_break;              /* streak broken */
    int ev_fever_on, ev_fever_off, ev_super_on, ev_not_ready;   /* not_ready: SPECIAL drummed without super fever */
} rc_t;

void    rc_init(rc_t*r,int bpm,int64_t t0);
int64_t rc_beat_time(const rc_t*r,int64_t b);
int64_t rc_nearest_beat(const rc_t*r,int64_t t);
int     rc_hit(rc_t*r,int drum,int64_t t);   /* returns J_*; may set ev_cmd */
void    rc_tick(rc_t*r,int64_t t);           /* time passing: streak timeouts */
int     rc_responding(const rc_t*r,int64_t t,int*beat_in);   /* command performed at t (or CMD_NONE) */
int     rc_match(const unsigned char p[4]);  /* CMD_* or CMD_NONE */
#endif
