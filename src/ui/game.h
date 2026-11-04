#ifndef R3_GAME_H
#define R3_GAME_H
#include "lvgl.h"
/* Blockfall — original falling-block puzzle. */
lv_obj_t* game_create(void (*on_exit)(void),int resume);   /* new screen (caller loads it) */
int  game_has_save(int*score);   /* a game was left unfinished */
void game_save(void);            /* save now (e.g. before power-off) */
int  game_key(int code);    /* media key while the game is shown; 1 = consumed */
int  game_running(void);    /* shown and not over (suppresses the idle watch face) */
#endif
