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
static int fonts_ready;
void fonts_init(void){
    static int done; if(done) return; done=1;
    make(&r3_font_12,&lv_font_montserrat_12,12);
    make(&r3_font_14,&lv_font_montserrat_14,14);
    make(&r3_font_16,&lv_font_montserrat_16,16);
    make(&r3_font_20,&lv_font_montserrat_20,20);
    make(&r3_font_28,&lv_font_montserrat_28,28);
    fonts_ready=1;
}

/* ---- extra scripts from the SD card ---- */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#define SD_FONTS "/usr/data/mnt/sd_0/Fonts"
static lv_font_t *const UI[5]={&r3_font_12,&r3_font_14,&r3_font_16,&r3_font_20,&r3_font_28};
static const int UI_PX[5]={12,14,16,20,28};
static lv_font_t *sdf[5]; static int sd_on;
/* The file is memory-mapped, not read: the kernel pages in only what glyph lookups touch and
 * can drop those clean pages under memory pressure (a 4 MB CJK font costs no committed RAM).
 * Streaming it through lv_fs instead was ~2 s per screen: stb_truetype hops between tables
 * with 2-4 byte reads. */
static void *map_p; static size_t map_n;
static void unlink_sd(int i){            /* take sdf[i] out of UI[i]'s fallback chain */
    for(lv_font_t*f=UI[i]; f && f->fallback; f=(lv_font_t*)f->fallback)
        if(f->fallback==sdf[i]){ f->fallback=NULL; break; } }
void fonts_sd_detach(void){
    if(!sd_on) return; sd_on=0;
    for(int i=0;i<5;i++) if(sdf[i]){ unlink_sd(i); lv_tiny_ttf_destroy(sdf[i]); sdf[i]=NULL; }
    if(map_p){ munmap(map_p,map_n); map_p=NULL; }
}
int fonts_sd_attach(void){
    /* not before fonts_init(): it rebuilds the UI fonts from the built-ins (fallback chain included);
     * the SD card is mounted earlier at boot, and an early attach was lost (or hit an empty font) */
    if(sd_on || !fonts_ready) return 0;
    DIR*d=opendir(SD_FONTS); if(!d) return 0;
    char path[512]=""; struct dirent*e;
    while((e=readdir(d))){ const char*x=strrchr(e->d_name,'.');
        if(e->d_name[0]!='.' && x && !strcasecmp(x,".ttf")){ snprintf(path,sizeof path,SD_FONTS "/%s",e->d_name); break; } }
    closedir(d); if(!path[0]) return 0;
    int fd=open(path,O_RDONLY); if(fd<0) return 0;
    struct stat st; void*p=MAP_FAILED;
    if(fstat(fd,&st)==0 && st.st_size>0) p=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd); if(p==MAP_FAILED) return 0;
    map_p=p; map_n=(size_t)st.st_size; sd_on=1;
    for(int i=0;i<5;i++){
        /* glyph cache in bytes: ~64 CJK bitmaps per size — names only, not running text */
        sdf[i]=lv_tiny_ttf_create_data_ex(map_p,map_n,UI_PX[i],(size_t)64*UI_PX[i]*UI_PX[i]);
        if(!sdf[i]){ fonts_sd_detach(); return 0; }
        lv_font_t*f=UI[i]; while(f->fallback) f=(lv_font_t*)f->fallback;
        f->fallback=sdf[i]; }
    return 1;
}
