#ifndef R3_SAPPER_H
#define R3_SAPPER_H
#include "lvgl.h"
/* Sapper — minesweeper-style game: clear the field without digging up a mine. */
lv_obj_t* sapper_create(void (*on_exit)(void),int resume);
int  sapper_has_save(int*cleared);   /* a game was left unfinished (squares cleared so far) */
void sapper_save(void);
int  sapper_key(int code);           /* R3 media keys while shown; 1 = consumed */
int  sapper_pad(int action);         /* controller action (pad.h PA_*); 1 = consumed */
int  sapper_running(void);           /* shown with a game under way */
#endif
