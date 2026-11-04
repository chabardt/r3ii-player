#ifndef R3_COIL_H
#define R3_COIL_H
#include "lvgl.h"
/* Coil — snake-style game (grow by eating, avoid walls and yourself). */
lv_obj_t* coil_create(void (*on_exit)(void),int resume);
int  coil_has_save(int*score);
void coil_save(void);
int  coil_key(int code);    /* R3 media keys while shown; 1 = consumed */
int  coil_pad(int action);  /* controller action (pad.h PA_*); 1 = consumed */
int  coil_running(void);
#endif
