/* Host render harness: draws an r3player screen with LVGL into memory and writes a PNG,
 * so UI can be checked without the device. Fake clock (fake_time / r3_tick_ms).
 *   sh tests/ui_render.sh   (builds and renders the scenes below into build/shots/) */
#include "lvgl.h"
#include "pet.h"
#include "pet_core.h"
#include "brick.h"
#include "coil.h"
#include "sapper.h"
#include "fonts.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t fake_ms; static time_t fake_now;
uint32_t r3_tick_ms(void){ return fake_ms; }
time_t fake_time(time_t*t){ if(t)*t=fake_now; return fake_now; }
float pad_stick_x(void){ return 0; } int pad_held(int a){ (void)a; return 0; }   /* no controller on the host */

static uint16_t fb[480*720];
static void flush(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*c){
    for(int y=a->y1;y<=a->y2;y++) for(int x=a->x1;x<=a->x2;x++) fb[y*480+x]=(c++)->full;
    lv_disp_flush_ready(d); }
static void png(const char*path){
    static unsigned char raw[720*(1+480*3)]; unsigned char*p=raw;
    for(int y=0;y<720;y++){ *p++=0; for(int x=0;x<480;x++){ uint16_t v=fb[y*480+x];
        *p++=(v>>11)*255/31; *p++=((v>>5)&63)*255/63; *p++=(v&31)*255/31; } }
    uLongf zl=compressBound(sizeof raw); unsigned char*z=malloc(zl); compress(z,&zl,raw,sizeof raw);
    FILE*f=fopen(path,"wb"); unsigned char hdr[]={0x89,'P','N','G','\r','\n',0x1a,'\n'}; fwrite(hdr,1,8,f);
    #define BE(v) (unsigned char)((v)>>24),(unsigned char)((v)>>16),(unsigned char)((v)>>8),(unsigned char)(v)
    unsigned char ih[]={'I','H','D','R',BE(480),BE(720),8,2,0,0,0};
    unsigned char len[4]={BE(13)}; fwrite(len,1,4,f); fwrite(ih,1,17,f); uLong crc=crc32(0,ih,17); unsigned char cb[4]={BE(crc)}; fwrite(cb,1,4,f);
    unsigned char l2[4]={BE((uint32_t)zl)}; fwrite(l2,1,4,f); fwrite("IDAT",1,4,f); fwrite(z,1,zl,f);
    crc=crc32(crc32(0,(const unsigned char*)"IDAT",4),z,zl); unsigned char c2[4]={BE(crc)}; fwrite(c2,1,4,f);
    unsigned char l3[4]={0}; fwrite(l3,1,4,f); fwrite("IEND",1,4,f); crc=crc32(0,(const unsigned char*)"IEND",4); unsigned char c3[4]={BE(crc)}; fwrite(c3,1,4,f);
    fclose(f); free(z); printf("wrote %s\n",path); }
static void run(int ms){ for(int i=0;i<ms;i+=20){ fake_ms+=20; lv_timer_handler(); } }
static void noexit(void){}

int main(int argc,char**argv){
    const char*scene=argc>1?argv[1]:"day", *out=argc>2?argv[2]:"shot.png";
    lv_init(); fonts_init();
    { const char*lg=getenv("R3_LANG"); if(lg) for(int i=1;i<i18n_count();i++) if(!strcmp(lg,i18n_code(i))) i18n_set(i); }   /* R3_LANG=fr */
    static lv_disp_draw_buf_t db; static lv_color_t buf[480*120]; lv_disp_draw_buf_init(&db,buf,NULL,480*120);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd); dd.draw_buf=&db; dd.flush_cb=flush; dd.hor_res=480; dd.ver_res=720;
    lv_disp_drv_register(&dd);
    if(!strncmp(scene,"brick",5)){ lv_scr_load(brick_create(noexit,0)); run(400); png(out); return 0; }
    if(!strncmp(scene,"coil",4)){ lv_scr_load(coil_create(noexit,0)); run(900); png(out); return 0; }
    if(!strncmp(scene,"sapper",6)){ lv_scr_load(sapper_create(noexit,0)); run(200); png(out); return 0; }
    /* pet scenes: write a save file, then open the screen */
    struct tm tm={0}; tm.tm_year=126; tm.tm_mon=10; tm.tm_mday=4; tm.tm_isdst=-1;
    pet_t p; pet_new(&p,0,3);
    p.stage=PS_ADULT; p.form=PF_LUMI; p.hunger=3; p.happy=4; p.age_days=7; p.weight=22; p.lights=1;
    tm.tm_hour=13;
    if(!strcmp(scene,"egg")){ p.stage=PS_EGG; p.stage_min=3; }
    if(!strcmp(scene,"baby")){ p.stage=PS_BABY; p.age_days=0; }
    if(!strcmp(scene,"child")){ p.stage=PS_CHILD; p.age_days=1; p.happy=1; }
    if(!strcmp(scene,"bramble")){ p.form=PF_BRAMBLE; p.poop=3; tm.tm_hour=18; }
    if(!strcmp(scene,"grub")){ p.form=PF_GRUB; p.sick=1; p.hunger=0; p.happy=1; }
    if(!strcmp(scene,"night")){ tm.tm_hour=23; p.asleep=1; p.lights=1; }
    if(!strcmp(scene,"dark")){ tm.tm_hour=23; p.asleep=1; p.lights=0; }
    if(!strcmp(scene,"elder")){ p.stage=PS_ELDER; p.form=PF_BRAMBLE; p.age_days=11; }
    fake_now=mktime(&tm); p.born=fake_now-p.age_days*86400; p.last=fake_now;
    FILE*f=fopen(PET_FILE,"wb"); fwrite(&p,sizeof p,1,f); fclose(f);
    lv_scr_load(pet_create(noexit)); run(700); png(out); return 0;
}
