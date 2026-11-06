/* fonts.c — see fonts.h */
#include "fonts.h"

/* the TTF, embedded in the binary (path is relative to the repo root, where the build runs) */
__asm__(".section .rodata\n.balign 4\n.global r3_ttf_data\nr3_ttf_data:\n"
        ".incbin \"vendor/lvgl/scripts/built_in_font/Montserrat-Medium.ttf\"\n"
        ".global r3_ttf_end\nr3_ttf_end:\n.previous\n");
extern const unsigned char r3_ttf_data[], r3_ttf_end[];

lv_font_t r3_font_12, r3_font_14, r3_font_16, r3_font_20, r3_font_28;

static void make(lv_font_t*out,const lv_font_t*builtin,int px){
    *out=*builtin;                                       /* built-in first: ASCII + symbols stay fast */
    /* cache size is in BYTES (~128 glyph bitmaps of px*px) — a glyph count would make an empty hash table */
    lv_font_t*ttf=lv_tiny_ttf_create_data_ex(r3_ttf_data,(size_t)(r3_ttf_end-r3_ttf_data),px,(size_t)128*px*px);
    if(!ttf) return;
    /* line metrics come from the built-in so mixed text lines up */
    out->fallback=ttf;
}
void fonts_init(void){
    static int done; if(done) return; done=1;
    make(&r3_font_12,&lv_font_montserrat_12,12);
    make(&r3_font_14,&lv_font_montserrat_14,14);
    make(&r3_font_16,&lv_font_montserrat_16,16);
    make(&r3_font_20,&lv_font_montserrat_20,20);
    make(&r3_font_28,&lv_font_montserrat_28,28);
}
