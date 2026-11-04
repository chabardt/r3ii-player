#ifndef R3_PET_H
#define R3_PET_H
#include "lvgl.h"
/* Moko — original virtual pet (screen + background life). */
lv_obj_t* pet_create(void (*on_exit)(void));
void pet_tick(void);              /* advance life + autosave; call every ~30 s */
int  pet_needs_you(void);         /* calling for attention (bell on Home) */
int  pet_shown(void);             /* the Moko screen is open (no idle watch face) */
const char* pet_summary(void);    /* one line for the Games menu */
#endif
