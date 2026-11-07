#ifndef R3_FONTS_H
#define R3_FONTS_H
#include "lvgl.h"
/* UI fonts with accents and other non-ASCII letters.
 * Each one is LVGL's built-in Montserrat (fast bitmaps: ASCII + icon symbols) with a fallback that
 * renders every other letter (é ç ß ł ж ...) from Montserrat-Medium.ttf — the font the built-ins were
 * made from — via tiny_ttf. Use these instead of lv_font_montserrat_N. */
extern lv_font_t r3_font_12, r3_font_14, r3_font_16, r3_font_20, r3_font_28;
void fonts_init(void);   /* call once after lv_init(), before building screens */
/* Scripts Montserrat lacks (Chinese, Japanese, Korean...): the first .ttf in Fonts/ on the SD card
 * (e.g. DroidSansFallbackFull.ttf) is chained after it, streamed from the card (not loaded in RAM).
 * attach: call when the card is mounted (no-op once attached or if there is no font);
 * detach: when the card is gone, so nothing reads from a missing file. */
int  fonts_sd_attach(void);
void fonts_sd_detach(void);
#endif
