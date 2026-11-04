#ifndef R3_COIL_CORE_H
#define R3_COIL_CORE_H
/* Coil — snake-style game logic (no UI). Grid of CW x CH cells, one step per tick.
 * Fruit grows the snake by 1 (+10 pts x level); a golden fruit appears now and then for
 * a few seconds (+50 x level, +2 length); a blue berry trims 3 segments when long.
 * Walls and the body end the game. The tick gets faster every 5 fruits. */
#include <stdint.h>
#define CW 20
#define CH 26
#define CMAX (CW*CH)
enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };
enum { F_NONE, F_FRUIT, F_GOLD, F_BERRY };
typedef struct { int8_t x,y; } cell_t;
typedef struct {
    cell_t body[CMAX]; int head, len, grow;   /* ring buffer: body[(head-i) % CMAX], i<len */
    int dir, next_dir[2], nq;                 /* queued turns (two quick swipes) */
    cell_t fruit, gold, berry; int gold_ticks, berry_on;
    int score, eaten, level, over, ticks;
    uint32_t rng;
    int ate;                                  /* event for the UI: last eaten F_* */
} coil_t;
void coil_new(coil_t*g, unsigned seed);
void coil_turn(coil_t*g, int dir);            /* absolute direction (reverse is ignored) */
void coil_turn_rel(coil_t*g, int right);      /* turn left (0) / right (1) of the current heading */
void coil_step(coil_t*g);
int  coil_tick_ms(const coil_t*g);            /* current speed */
int  coil_at(const coil_t*g, int x, int y);   /* 1 if the snake covers the cell */
static inline cell_t coil_seg(const coil_t*g,int i){ return g->body[(g->head-i+CMAX)%CMAX]; }
#endif
