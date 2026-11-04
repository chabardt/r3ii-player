#ifndef R3_BRICK_CORE_H
#define R3_BRICK_CORE_H
/* Brickstorm game logic (no UI): paddle, balls, bricks, power-ups, levels, score.
 * Field coordinates in pixels, origin top-left. Pure C so it can be tested on the host. */
#include <stdint.h>

#define BK_W   480
#define BK_H   666
#define BK_NC  10
#define BK_NR  8
#define BK_BW  44          /* brick size; columns are 48 px apart */
#define BK_BH  18
#define BK_X0  2
#define BK_Y0  44
#define BK_DY  24
#define BK_PW  96          /* paddle (normal width) */
#define BK_PH  14
#define BK_PY  (BK_H-44)
#define BK_R   7           /* ball radius */
#define BK_MAXB 12

enum { BT_NORMAL, BT_MULTI, BT_BOMB, BT_FAST, BT_SLOW, BT_WIDE, BT_TINY };
typedef struct { int8_t hp, type, col; } bk_brick_t;
typedef struct { float x,y,vx,vy; int8_t on, stuck, bomb; } bk_ball_t;

typedef struct {
    bk_brick_t br[BK_NR][BK_NC];
    bk_ball_t  b[BK_MAXB];
    float px, pw;             /* paddle left edge, width (W/N power-ups) */
    float speed;              /* power-up multiplier (0.55..1.9) */
    int level, score, left, over;
    float quiet;              /* seconds since a brick was last hit (wider bounces after 15 s) */
    /* events for the UI (consumed by the caller) */
    int dirty_bricks;         /* bricks changed: redraw them */
    int toast;                /* 0, or BT_* of the last power-up (and 100 = level clear) */
} bk_t;

void bk_new_game(bk_t*g, unsigned seed);
void bk_step(bk_t*g, float dt);          /* advance; dt in seconds (sub-stepped inside) */
void bk_set_paddle(bk_t*g, float center_x);
void bk_launch(bk_t*g);                  /* release balls stuck on the paddle */
int  bk_balls(const bk_t*g);             /* balls in play (incl. stuck) */
int  bk_any_stuck(const bk_t*g);
float bk_base_speed(const bk_t*g);
static inline float bk_brick_x(int c){ return BK_X0 + c*48.0f; }
static inline float bk_brick_y(int r){ return BK_Y0 + r*(float)BK_DY; }
#endif
