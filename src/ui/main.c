/* r3ii-player — LVGL UI for HiBy R3 Pro II (Ingenic X1600, 480x720 RGB565, goodix touch)
 * Stage 1: fbdev + evdev drivers + Home menu (Music / Podcasts / Games / Settings). */
#include "lvgl.h"
#include "r3_tick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

/* ---- tick ---- */
uint32_t r3_tick_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint32_t)(ts.tv_sec*1000u + ts.tv_nsec/1000000u);
}

/* ---- framebuffer display ---- */
static FILE*dbgf; /* debug log file (reliable over adb backgrounding) */
#define DBG(...) do{ if(dbgf){ fprintf(dbgf,__VA_ARGS__); fflush(dbgf);} }while(0)
static int fbfd; static uint8_t *fbmem; static int FBW,FBH,FBSTRIDE,FBPAGE2;
static void fb_init(void){
    fbfd=open("/dev/fb0",O_RDWR);
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fbfd,FBIOGET_VSCREENINFO,&v); ioctl(fbfd,FBIOGET_FSCREENINFO,&f);
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);          /* wake panel + touch */
    v.xoffset=0; v.yoffset=0; ioctl(fbfd,FBIOPAN_DISPLAY,&v);  /* show page 0 */
    FBW=v.xres; FBH=v.yres; FBSTRIDE=f.line_length;
    fbmem=mmap(0,f.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
    if(v.yres_virtual>=v.yres*2) FBPAGE2=(int)v.yres*FBSTRIDE; /* 2nd page offset */
    printf("fb_init %dx%d stride=%d smem=%u page2=%d\n",FBW,FBH,FBSTRIDE,f.smem_len,FBPAGE2);
}
static void disp_flush(lv_disp_drv_t*drv,const lv_area_t*a,lv_color_t*cp){
    static int n=0;
    if(n<3){ printf("flush #%d area %d,%d-%d,%d\n",n,a->x1,a->y1,a->x2,a->y2); n++; }
    int w=(a->x2-a->x1+1)*2;
    for(int y=a->y1;y<=a->y2;y++){
        long off=(long)y*FBSTRIDE+(long)a->x1*2;
        memcpy(fbmem+off,cp,w);
        if(FBPAGE2) memcpy(fbmem+FBPAGE2+off,cp,w); /* write both pages */
        cp+=(a->x2-a->x1+1);
    }
    lv_disp_flush_ready(drv);
}

/* ---- goodix touch (event1, MT type-A, pixel coords) ---- */
static int tsfd; static int t_x,t_y,t_down;
static void ts_init(void){ tsfd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK); }
static void ts_read(lv_indev_drv_t*drv,lv_indev_data_t*d){
    struct input_event e; int nx=t_x,ny=t_y;
    while(read(tsfd,&e,sizeof e)==sizeof e){
        if(e.type==EV_ABS){
            if(e.code==ABS_MT_POSITION_X||e.code==ABS_X)nx=e.value;
            else if(e.code==ABS_MT_POSITION_Y||e.code==ABS_Y)ny=e.value;
        }else if(e.type==EV_KEY&&e.code==BTN_TOUCH)t_down=e.value;
    }
    t_x=nx; t_y=ny;
    static int last=-1;
    if(t_down!=last){ DBG("touch %s x=%d y=%d\n",t_down?"DOWN":"up",t_x,t_y); last=t_down; }
    d->point.x=t_x; d->point.y=t_y;
    d->state=t_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}

/* ---- retro palette (cream theme) ---- */
#define C_SCR   0xF2E9D8
#define C_INK   0x241F1C
#define C_PANEL 0xFBF6EA
#define C_TEAL  0x2F8F8F
#define C_ORANGE 0xE07A3F
#define C_MAROON 0x9C3B4E
#define C_BARBG 0x241F1C

static lv_obj_t *clock_lbl;

static void tile_cb(lv_event_t*e){
    const char*name=lv_event_get_user_data(e);
    DBG("tile tapped: %s\n",name);
}

static lv_obj_t* make_tile(lv_obj_t*grid,const char*txt,uint32_t accent,int col,int row){
    lv_obj_t*b=lv_btn_create(grid);
    lv_obj_set_grid_cell(b,LV_GRID_ALIGN_STRETCH,col,1,LV_GRID_ALIGN_STRETCH,row,1);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_radius(b,0,0);
    lv_obj_set_style_border_color(b,lv_color_hex(C_INK),0);
    lv_obj_set_style_border_width(b,3,0);
    lv_obj_set_style_shadow_color(b,lv_color_hex(C_INK),0);
    lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_shadow_ofs_x(b,4,0); lv_obj_set_style_shadow_ofs_y(b,4,0);
    lv_obj_set_style_shadow_opa(b,LV_OPA_COVER,0);
    /* obvious pressed feedback: fill with accent, sink the tile */
    lv_obj_set_style_bg_color(b,lv_color_hex(accent),LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(b,4,LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(b,4,LV_STATE_PRESSED);
    lv_obj_set_style_shadow_ofs_x(b,0,LV_STATE_PRESSED);
    lv_obj_set_style_shadow_ofs_y(b,0,LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,tile_cb,LV_EVENT_CLICKED,(void*)txt);
    lv_obj_t*l=lv_label_create(b);
    lv_label_set_text(l,txt);
    lv_obj_set_style_text_color(l,lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0);
    lv_obj_center(l);
    return b;
}

static void build_home(void){
    lv_obj_t*scr=lv_scr_act();
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_SCR),0);
    lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);

    /* status bar */
    lv_obj_t*bar=lv_obj_create(scr);
    lv_obj_set_size(bar,480,50); lv_obj_set_pos(bar,0,0);
    lv_obj_set_style_bg_color(bar,lv_color_hex(C_BARBG),0);
    lv_obj_set_style_radius(bar,0,0); lv_obj_set_style_border_width(bar,0,0);
    lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*name=lv_label_create(bar); lv_label_set_text(name,"r3ii");
    lv_obj_set_style_text_color(name,lv_color_hex(C_SCR),0);
    lv_obj_align(name,LV_ALIGN_LEFT_MID,0,0);
    clock_lbl=lv_label_create(bar); lv_label_set_text(clock_lbl,"--:--");
    lv_obj_set_style_text_color(clock_lbl,lv_color_hex(C_SCR),0);
    lv_obj_align(clock_lbl,LV_ALIGN_RIGHT_MID,0,0);

    /* title */
    lv_obj_t*h=lv_label_create(scr); lv_label_set_text(h,"HELLO");
    lv_obj_set_style_text_color(h,lv_color_hex(C_INK),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0);
    lv_obj_align(h,LV_ALIGN_TOP_MID,0,70);

    /* 2x2 tile grid */
    static lv_coord_t cols[]={LV_GRID_FR(1),LV_GRID_FR(1),LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[]={LV_GRID_FR(1),LV_GRID_FR(1),LV_GRID_TEMPLATE_LAST};
    lv_obj_t*grid=lv_obj_create(scr);
    lv_obj_set_style_bg_opa(grid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(grid,0,0);
    lv_obj_set_style_pad_all(grid,0,0);
    lv_obj_set_size(grid,440,440); lv_obj_align(grid,LV_ALIGN_TOP_MID,0,130);
    lv_obj_set_style_pad_row(grid,20,0); lv_obj_set_style_pad_column(grid,20,0);
    lv_obj_set_grid_dsc_array(grid,cols,rows);
    lv_obj_clear_flag(grid,LV_OBJ_FLAG_SCROLLABLE);
    make_tile(grid,"Music",   C_TEAL,  0,0);
    make_tile(grid,"Podcasts",C_ORANGE,1,0);
    make_tile(grid,"Games",   C_INK,   0,1);
    make_tile(grid,"Settings",C_MAROON,1,1);
}

static void tick_clock(void){
    static uint32_t last=0; uint32_t now=r3_tick_ms();
    if(now-last<1000)return; last=now;
    time_t t=time(0); struct tm*lt=localtime(&t); char b[8];
    strftime(b,sizeof b,"%H:%M",lt);
    if(clock_lbl)lv_label_set_text(clock_lbl,b);
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    dbgf=fopen("/usr/data/r3dbg.log","w"); /* own log; survives adb backgrounding */
    fb_init();
    lv_init();
    static lv_disp_draw_buf_t db; static lv_color_t buf[480*120];
    lv_disp_draw_buf_init(&db,buf,NULL,480*120);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.draw_buf=&db; dd.flush_cb=disp_flush; dd.hor_res=480; dd.ver_res=720;
    lv_disp_drv_register(&dd);
    ts_init();
    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type=LV_INDEV_TYPE_POINTER; id.read_cb=ts_read; lv_indev_drv_register(&id);
    build_home();
    printf("r3player UI up (480x720)\n");
    for(;;){ lv_timer_handler(); tick_clock(); usleep(5000); }
    return 0;
}
