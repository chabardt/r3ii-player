#ifndef R3_GAME_H
#define R3_GAME_H
#include "lvgl.h"
/* Blockfall — original falling-block puzzle. */
lv_obj_t* game_create(void (*on_exit)(void));   /* new screen (caller loads it) */
int  game_key(int code);    /* media key while the game is shown; 1 = consumed */
int  game_running(void);    /* shown and not over (suppresses the idle watch face) */
#endif
