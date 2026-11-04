#ifndef R3_BRICK_H
#define R3_BRICK_H
#include "lvgl.h"
/* Brickstorm — paddle, bouncing balls, breakable bricks with power-ups. */
lv_obj_t* brick_create(void (*on_exit)(void),int resume);   /* new screen (caller loads it) */
int  brick_has_save(int*score);  /* a game was left unfinished */
void brick_save(void);           /* save now (e.g. before power-off) */
int  brick_key(int code);   /* media key while shown; 1 = consumed */
int  brick_running(void);   /* shown and not over */
int  brick_pad(int action);  /* game-controller action (pad.h PA_*); 1 = consumed */
#endif
