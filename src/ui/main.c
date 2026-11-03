/* r3ii-player — LVGL UI for HiBy R3 Pro II (480x720 RGB565, goodix touch, X1600).
 * Home menu -> Music directory browser -> Now Playing with audio playback.
 * Dark theme (black + orange), playlist w/ auto-advance + prev/next, physical keys. */
#include "lvgl.h"
#include "r3_tick.h"
#include "audio.h"
#include "bt.h"
#include "cover.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/mount.h>

static void null_clock(void);
static uint32_t g_last_input;   /* last touch/key (idle -> watch face / Now Playing) */

#define MUSIC_ROOT "/usr/data/mnt/sd_0/Music"
#define PODCAST_ROOT "/usr/data/mnt/sd_0/Others"
#define RESUME_FILE "/usr/data/r3_resume"

/* current top-level section being browsed (so "back" at its root returns Home) */
static char g_root[1024]=MUSIC_ROOT;
static char g_root_label[32]="Music";

uint32_t r3_tick_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint32_t)(ts.tv_sec*1000u+ts.tv_nsec/1000000u); }

static FILE*dbgf;
#define DBG(...) do{ if(dbgf){fprintf(dbgf,__VA_ARGS__);fflush(dbgf);} }while(0)

/* ---- framebuffer ---- */
static int fbfd; static uint8_t*fbmem; static int FBW,FBH,FBSTRIDE,FBPAGE2;
static void fb_init(void){
    fbfd=open("/dev/fb0",O_RDWR);
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fbfd,FBIOGET_VSCREENINFO,&v); ioctl(fbfd,FBIOGET_FSCREENINFO,&f);
    ioctl(fbfd,FBIOBLANK,FB_BLANK_UNBLANK);
    v.xoffset=0; v.yoffset=0; ioctl(fbfd,FBIOPAN_DISPLAY,&v);
    FBW=v.xres; FBH=v.yres; FBSTRIDE=f.line_length;
    fbmem=mmap(0,f.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
    if(v.yres_virtual>=v.yres*2) FBPAGE2=(int)v.yres*FBSTRIDE;
}
static void disp_flush(lv_disp_drv_t*drv,const lv_area_t*a,lv_color_t*cp){
    int w=(a->x2-a->x1+1)*2;
    for(int y=a->y1;y<=a->y2;y++){ long off=(long)y*FBSTRIDE+(long)a->x1*2;
        memcpy(fbmem+off,cp,w); /* page 0 only (we pan to it); dual-page write risked mmap-edge SIGBUS */
        cp+=(a->x2-a->x1+1); }
    lv_disp_flush_ready(drv);
}
/* ---- touch ---- */
static int tsfd,t_x,t_y,t_down;
static void ts_read(lv_indev_drv_t*drv,lv_indev_data_t*d){ (void)drv;
    struct input_event e;
    while(read(tsfd,&e,sizeof e)==sizeof e){
        if(e.type==EV_ABS){ if(e.code==ABS_MT_POSITION_X||e.code==ABS_X)t_x=e.value;
            else if(e.code==ABS_MT_POSITION_Y||e.code==ABS_Y)t_y=e.value; }
        else if(e.type==EV_KEY&&e.code==BTN_TOUCH)t_down=e.value;
        g_last_input=r3_tick_ms();
    }
    d->point.x=t_x; d->point.y=t_y;
    d->state=t_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}

/* ---- dark palette (black + orange) ---- */
#define C_BG      0x0A0A0A   /* app background (near-black) */
#define C_BAR     0x000000   /* top bars */
#define C_PANEL   0x1A1510   /* cards, list rows */
#define C_PANEL2  0x241B12   /* pressed / highlighted row */
#define C_FG      0xF4E4D0   /* primary text (warm white) */
#define C_DIM     0x8A6A4A   /* secondary text */
#define C_ACCENT  0xE8731C   /* orange */
#define C_ACCENT2 0xB85518   /* deep orange */

/* live-updated widgets — NULL when the screen that owns them is gone */
static lv_obj_t *clock_lbl, *np_pos, *np_play, *np_title, *np_prog;
static lv_obj_t *np_cover;    /* the cover container on Now Playing (NULL elsewhere) */
static lv_obj_t *set_list, *set_status;   /* Settings screen (NULL when not shown) */
static void null_live(void){ clock_lbl=np_pos=np_play=np_title=np_prog=NULL; set_list=set_status=NULL; np_cover=NULL; null_clock(); }
/* persistent volume overlay (lives on the top layer, survives screen changes) */
static lv_obj_t *g_volov, *g_volbar; static uint32_t g_vol_hide;
static uint32_t g_prog_hold;   /* don't auto-update the seek bar until this time (after a user seek) */

/* ---- screen state (for the physical power button) ---- */
enum { SCR_HOME, SCR_BROWSER, SCR_PLAYER, SCR_SETTINGS, SCR_CLOCK };
static int g_scr=SCR_HOME;

/* switch screens now, but defer deleting the old one to the main loop (never mid-event) */
static lv_obj_t *g_to_del;
static void load_screen(lv_obj_t*ns){ lv_obj_t*old=lv_scr_act(); lv_scr_load(ns);
    if(old&&old!=ns) g_to_del=old; }
static void show_home(void);
static void show_browser(const char*path);
static void show_player(const char*title,const char*dir);
static void show_settings(void);
static void np_set_cover(void);

/* ---- helpers ---- */
/* The built-in Montserrat fonts are ASCII-only: fold UTF-8 Latin accents to ASCII in
 * place (é->e, Œ->O, ’->') so "Théo" doesn't render as "Th□o"; unknown glyphs -> '?'. */
static void fold(char*str){
    static const char L1[]="AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    unsigned char*r=(unsigned char*)str,*w=r;
    while(*r){
        if(*r<0x80){ *w++=*r++; continue; }
        if((r[0]&0xE0)==0xC0 && r[1]){ unsigned cp=((r[0]&0x1Fu)<<6)|(r[1]&0x3Fu); r+=2;
            if(cp>=0xC0&&cp<=0xFF) *w++=L1[cp-0xC0];
            else if(cp==0x152) *w++='O'; else if(cp==0x153) *w++='o';
            else if(cp==0xA0) *w++=' ';
            else *w++='?';
            continue; }
        if((r[0]&0xF0)==0xE0 && r[1] && r[2]){ unsigned cp=((r[0]&0x0Fu)<<12)|((r[1]&0x3Fu)<<6)|(r[2]&0x3Fu); r+=3;
            if(cp==0x2018||cp==0x2019) *w++='\'';
            else if(cp==0x201C||cp==0x201D) *w++='"';
            else if(cp==0x2013||cp==0x2014) *w++='-';
            else *w++='?';
            continue; }
        r++; while((*r&0xC0)==0x80) r++;   /* 4-byte / stray: skip */
        *w++='?';
    }
    *w=0;
}

static int is_audio(const char*n){ const char*d=strrchr(n,'.'); if(!d)return 0;
    return !strcasecmp(d,".mp3")||!strcasecmp(d,".mpga")||!strcasecmp(d,".flac")
         ||!strcasecmp(d,".wav")||!strcasecmp(d,".m4a"); }
static void pretty(const char*in,char*out,int cap){ /* strip leading "NN. " and audio ext */
    const char*s=in; while(*s>='0'&&*s<='9')s++;
    if(*s=='.'||*s=='-'||*s==' '){ while(*s=='.'||*s=='-'||*s==' ')s++; } else s=in;
    strncpy(out,s,cap-1); out[cap-1]=0;
    char*d=strrchr(out,'.'); if(d&&is_audio(out))*d=0;
    fold(out);
}

/* ---- playlist (siblings in the directory of the chosen song) ---- */
static char **g_pl=0; static int g_pln=0, g_pli=-1;
static char g_pldir[1024]="";
static char g_pltitle[256]="";
static void pl_free(void){ if(g_pl){ for(int i=0;i<g_pln;i++)free(g_pl[i]); free(g_pl);} g_pl=0; g_pln=0; g_pli=-1; }
static int cmp_pstr(const void*a,const void*b){ return strcasecmp(*(char*const*)a,*(char*const*)b); }
static void pl_build(const char*dir,const char*selname){
    pl_free();
    strncpy(g_pldir,dir,sizeof g_pldir-1); g_pldir[sizeof g_pldir-1]=0;
    DIR*dp=opendir(dir); if(!dp)return;
    char **tmp=malloc(sizeof(char*)*4096); int n=0;
    struct dirent*de;
    while((de=readdir(dp))&&n<4096){ if(de->d_name[0]=='.')continue;
        if(!is_audio(de->d_name))continue; tmp[n++]=strdup(de->d_name); }
    closedir(dp);
    qsort(tmp,n,sizeof(char*),cmp_pstr);
    g_pl=tmp; g_pln=n; g_pli=0;
    if(selname) for(int i=0;i<n;i++) if(!strcmp(tmp[i],selname)){ g_pli=i; break; }
}
static void pl_play(int i){
    if(i<0||i>=g_pln)return; g_pli=i;
    char full[1600]; snprintf(full,sizeof full,"%s/%s",g_pldir,g_pl[i]);
    pretty(g_pl[i],g_pltitle,sizeof g_pltitle);
    if(bt_receiver() && !bt_output()[0]) bt_set_receiver(0);   /* both need the DAC */
    audio_play(full);
    if(np_title) lv_label_set_text(np_title,g_pltitle);
    if(np_play)  lv_label_set_text(np_play,LV_SYMBOL_PAUSE);
    np_set_cover();
}
static void pl_next(void){ if(g_pli+1<g_pln) pl_play(g_pli+1); }
static void pl_prev(void){ if(audio_pos_ms()>3000) pl_play(g_pli);   /* >3s: restart track */
    else if(g_pli>0) pl_play(g_pli-1); else pl_play(0); }

/* ---- resume across power-off ---- */
static void save_resume(void){
    if(g_pln<=0||g_pli<0) return;
    FILE*f=fopen(RESUME_FILE,"w"); if(!f)return;
    fprintf(f,"%s\n%s\n%ld\n",g_pldir,g_pl[g_pli],audio_pos_ms());
    fclose(f);
}

/* ---- album art (see cover.c) ---- */
#define COVER_W 480
#define COVER_H 360
static void np_set_cover(void){
    if(!np_cover) return;
    lv_obj_clean(np_cover);
    char trk[1700]; const char*tp=0;
    if(g_pln>0 && g_pli>=0){ snprintf(trk,sizeof trk,"%s/%s",g_pldir,g_pl[g_pli]); tp=trk; }
    const lv_img_dsc_t*img = g_pldir[0] ? cover_load(g_pldir,tp,COVER_W,COVER_H) : 0;
    if(img){ lv_obj_t*o=lv_img_create(np_cover); lv_img_set_src(o,img); lv_obj_center(o); }
    else { lv_obj_t*cl=lv_label_create(np_cover); lv_label_set_text(cl,LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(cl,lv_color_hex(C_ACCENT),0);
        lv_obj_set_style_text_font(cl,&lv_font_montserrat_28,0); lv_obj_center(cl); }
}

/* ---- Now Playing ---- */
static void np_back_cb(lv_event_t*e){ (void)e; show_browser(g_pldir[0]?g_pldir:MUSIC_ROOT); }
static void np_playpause_cb(lv_event_t*e){ (void)e;
    if(!audio_is_playing() && g_pln>0){ pl_play(g_pli); return; }   /* resume-ready: start it */
    audio_toggle_pause();
    if(np_play) lv_label_set_text(np_play, audio_is_paused()?LV_SYMBOL_PLAY:LV_SYMBOL_PAUSE); }
static void np_next_cb(lv_event_t*e){ (void)e; pl_next(); }
static void np_prev_cb(lv_event_t*e){ (void)e; pl_prev(); }
static void np_prog_cb(lv_event_t*e){ (void)e; long dur=audio_dur_ms();   /* user released the seek bar */
    long v = np_prog?lv_slider_get_value(np_prog):-1;
    DBG("prog_cb ev=%d val=%ld dur=%ld -> seek=%ld\n",lv_event_get_code(e),v,dur,dur>0?(long)((long long)v*dur/1000):-1);
    if(dur>0 && np_prog){ audio_seek((long)((long long)v*dur/1000));
        g_prog_hold=r3_tick_ms()+1200; } }   /* let the seek take before auto-updates resume */

static lv_obj_t* mk_tbtn(lv_obj_t*p,int w,int h,int x,int y,uint32_t bg,const char*sym,lv_event_cb_t cb){
    lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,w,h); lv_obj_align(b,LV_ALIGN_TOP_MID,x,y);
    lv_obj_set_style_bg_color(b,lv_color_hex(bg),0);
    lv_obj_set_style_border_color(b,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_border_width(b,2,0); lv_obj_set_style_radius(b,0,0);
    if(cb) lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,sym);
    lv_obj_set_style_text_color(l,lv_color_hex(bg==(uint32_t)C_ACCENT?C_BG:C_FG),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l);
    return b;
}
static void show_player(const char*title,const char*dir){
    if(dir && dir!=g_pldir){ strncpy(g_pldir,dir,sizeof g_pldir-1); g_pldir[sizeof g_pldir-1]=0; }
    null_live();
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0);
    lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    /* cover placeholder (full width) */
    lv_obj_t*cov=lv_obj_create(scr); lv_obj_set_size(cov,COVER_W,COVER_H); lv_obj_set_pos(cov,0,60);
    lv_obj_set_style_radius(cov,0,0); lv_obj_set_style_border_width(cov,0,0);
    lv_obj_set_style_bg_color(cov,lv_color_hex(C_PANEL),0);
    lv_obj_clear_flag(cov,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cov,0,0);
    np_cover=cov; np_set_cover();
    /* back chip over the cover */
    lv_obj_t*bk=lv_btn_create(scr); lv_obj_set_size(bk,48,40); lv_obj_set_pos(bk,8,8);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_ACCENT),0);
    lv_obj_add_event_cb(bk,np_back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl,lv_color_hex(C_ACCENT),0); lv_obj_center(bl);
    /* title */
    np_title=lv_label_create(scr); lv_label_set_text(np_title,title);
    lv_label_set_long_mode(np_title,LV_LABEL_LONG_DOT); lv_obj_set_width(np_title,440);
    lv_obj_set_style_text_align(np_title,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(np_title,lv_color_hex(C_FG),0);
    lv_obj_set_style_text_font(np_title,&lv_font_montserrat_20,0);
    lv_obj_align(np_title,LV_ALIGN_TOP_MID,0,436);
    /* position */
    np_pos=lv_label_create(scr); lv_label_set_text(np_pos,"0:00");
    lv_obj_set_style_text_color(np_pos,lv_color_hex(C_DIM),0);
    lv_obj_align(np_pos,LV_ALIGN_TOP_MID,0,474);
    /* transport row: prev | play/pause | next  (np_play = the symbol inside play/pause) */
    mk_tbtn(scr, 90,76,-150,530, C_PANEL,  LV_SYMBOL_PREV, np_prev_cb);
    lv_obj_t*ppb=mk_tbtn(scr,120,86,0,525, C_ACCENT, LV_SYMBOL_PAUSE, np_playpause_cb);
    np_play=lv_obj_get_child(ppb,0);   /* the label mk_tbtn created inside it */
    if(!audio_is_playing()||audio_is_paused()) lv_label_set_text(np_play,LV_SYMBOL_PLAY);   /* resume-ready / paused */
    mk_tbtn(scr, 90,76, 150,530, C_PANEL,  LV_SYMBOL_NEXT, np_next_cb);
    /* progress / seek bar (volume lives in the top overlay now) */
    np_prog=lv_slider_create(scr); lv_obj_set_size(np_prog,440,14); lv_obj_align(np_prog,LV_ALIGN_TOP_MID,0,660);
    lv_slider_set_range(np_prog,0,1000); lv_slider_set_value(np_prog,0,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(np_prog,lv_color_hex(C_PANEL),LV_PART_MAIN);
    lv_obj_set_style_bg_color(np_prog,lv_color_hex(C_ACCENT),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(np_prog,lv_color_hex(C_FG),LV_PART_KNOB);
    lv_obj_set_style_pad_all(np_prog,8,LV_PART_KNOB);      /* bigger knob hit area */
    lv_obj_set_ext_click_area(np_prog,16);                 /* easier to grab the bar */
    lv_obj_add_event_cb(np_prog,np_prog_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(np_prog,np_prog_cb,LV_EVENT_PRESS_LOST,NULL);
    g_scr=SCR_PLAYER;
    load_screen(scr);
}

/* ---- Browser ---- */
typedef struct { char name[256]; int dir; } entry_t;
static int ecmp(const void*a,const void*b){ const entry_t*x=a,*y=b;
    if(x->dir!=y->dir)return y->dir-x->dir; return strcasecmp(x->name,y->name); }

static void entry_cb(lv_event_t*e){
    char*full=lv_event_get_user_data(e); /* strdup'd full path */
    struct stat st;
    if(stat(full,&st)==0 && S_ISDIR(st.st_mode)){ show_browser(full); return; }
    /* a song: build the playlist from its folder, play it, open Now Playing */
    char dir[1024]; strncpy(dir,full,sizeof dir-1); dir[sizeof dir-1]=0;
    char*sl=strrchr(dir,'/'); const char*name = sl?sl+1:full; if(sl)*sl=0;
    pl_build(dir,name);
    pl_play(g_pli);
    show_player(g_pltitle,dir);
}
static void br_back_cb(lv_event_t*e){ char*parent=lv_event_get_user_data(e);
    if(!parent||!parent[0]||!strcmp(parent,"/")) show_home(); else show_browser(parent); }
static void free_cb(lv_event_t*e){ void*p=lv_event_get_user_data(e); if(p)free(p); }

/* A-Z rail: tap a letter -> scroll the list to the first entry starting with it */
static lv_obj_t *g_firstof[27];   /* 0..25 = A..Z, 26 = other/#  (buttons in the current list) */
static void az_cb(lv_event_t*e){
    long idx=(long)lv_event_get_user_data(e);
    for(long k=idx;k<27;k++) if(g_firstof[k]){ lv_obj_scroll_to_view(g_firstof[k],LV_ANIM_OFF); return; }
    for(long k=idx;k>=0;k--) if(g_firstof[k]){ lv_obj_scroll_to_view(g_firstof[k],LV_ANIM_OFF); return; }
}
static int letter_index(const char*disp){ char c=disp[0];
    if(c>='a'&&c<='z')return c-'a'; if(c>='A'&&c<='Z')return c-'A'; return 26; }

static void show_browser(const char*path){
    char cur[1024]; strncpy(cur,path,sizeof cur-1); cur[sizeof cur-1]=0; /* own copy */
    null_live();
    for(int i=0;i<27;i++) g_firstof[i]=NULL;
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0);
    lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    /* header */
    lv_obj_t*hdr=lv_obj_create(scr); lv_obj_set_size(hdr,480,54); lv_obj_set_pos(hdr,0,0);
    lv_obj_set_style_bg_color(hdr,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(hdr,0,0);
    lv_obj_set_style_border_width(hdr,0,0); lv_obj_clear_flag(hdr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bk=lv_btn_create(hdr); lv_obj_set_size(bk,46,40); lv_obj_align(bk,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_ACCENT),0);
    char*parent=malloc(1024);
    if(!strcmp(cur,g_root)) parent[0]=0;
    else { strncpy(parent,cur,1023); parent[1023]=0; char*sl=strrchr(parent,'/'); if(sl)*sl=0; }
    lv_obj_add_event_cb(bk,br_back_cb,LV_EVENT_CLICKED,parent);
    lv_obj_add_event_cb(bk,free_cb,LV_EVENT_DELETE,parent);
    lv_obj_t*bkl=lv_label_create(bk); lv_label_set_text(bkl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bkl,lv_color_hex(C_ACCENT),0); lv_obj_center(bkl);
    lv_obj_t*ttl=lv_label_create(hdr); const char*base=strrchr(cur,'/');
    { char tt[256]; snprintf(tt,sizeof tt,"%s",!strcmp(cur,g_root)?g_root_label:(base?base+1:cur)); fold(tt);
      lv_label_set_text(ttl,tt); }
    lv_label_set_long_mode(ttl,LV_LABEL_LONG_DOT); lv_obj_set_width(ttl,370);
    lv_obj_set_style_text_color(ttl,lv_color_hex(C_ACCENT),0); lv_obj_align(ttl,LV_ALIGN_LEFT_MID,56,0);
    /* list (leave 30px on the right for the A-Z rail) */
    lv_obj_t*list=lv_list_create(scr); lv_obj_set_size(list,450,720-54); lv_obj_set_pos(list,0,54);
    lv_obj_set_style_bg_color(list,lv_color_hex(C_BG),0); lv_obj_set_style_border_width(list,0,0);
    lv_obj_set_style_pad_all(list,0,0);
    /* read dir */
    DIR*dp=opendir(cur);
    if(dp){
        entry_t*es=malloc(sizeof(entry_t)*4096); int ne=0;
        struct dirent*de;
        while((de=readdir(dp)) && ne<4096){
            if(de->d_name[0]=='.')continue;
            int isdir;
            if(de->d_type==DT_DIR) isdir=1;
            else if(de->d_type==DT_REG) isdir=0;
            else { char full[1600]; snprintf(full,sizeof full,"%s/%s",cur,de->d_name);
                   struct stat st; isdir=(stat(full,&st)==0 && S_ISDIR(st.st_mode)); }
            if(!isdir && !is_audio(de->d_name)) continue;
            strncpy(es[ne].name,de->d_name,255); es[ne].name[255]=0; es[ne].dir=isdir; ne++;
        }
        closedir(dp);
        qsort(es,ne,sizeof(entry_t),ecmp);
        for(int i=0;i<ne;i++){
            char disp[256];
            if(es[i].dir){ strncpy(disp,es[i].name,sizeof disp-1); disp[sizeof disp-1]=0; fold(disp); }
            else pretty(es[i].name,disp,sizeof disp);
            lv_obj_t*b=lv_list_add_btn(list, es[i].dir?LV_SYMBOL_DIRECTORY:LV_SYMBOL_AUDIO, disp);
            lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0);
            lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL2),LV_STATE_PRESSED);
            lv_obj_set_style_text_color(b,lv_color_hex(C_FG),0);
            lv_obj_set_style_border_color(b,lv_color_hex(0x000000),0);
            lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,0,0);
            int li=letter_index(disp); if(!g_firstof[li]) g_firstof[li]=b;
            char*full=malloc(1600); snprintf(full,1600,"%s/%s",cur,es[i].name);
            lv_obj_add_event_cb(b,entry_cb,LV_EVENT_CLICKED,full);
            lv_obj_add_event_cb(b,free_cb,LV_EVENT_DELETE,full);
        }
        free(es);
    }
    /* A-Z rail */
    lv_obj_t*rail=lv_obj_create(scr); lv_obj_set_size(rail,30,720-54); lv_obj_set_pos(rail,450,54);
    lv_obj_set_style_bg_color(rail,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(rail,0,0);
    lv_obj_set_style_border_width(rail,0,0); lv_obj_set_style_pad_all(rail,0,0);
    lv_obj_set_style_pad_row(rail,0,0); lv_obj_clear_flag(rail,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(rail,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail,LV_FLEX_ALIGN_SPACE_AROUND,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    static const char LET[27][2]={"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","#"};
    for(long k=0;k<27;k++){
        lv_obj_t*ll=lv_label_create(rail); lv_label_set_text(ll,LET[k]);
        lv_obj_set_style_text_color(ll,lv_color_hex(g_firstof[k]?C_ACCENT:C_DIM),0);
        lv_obj_set_style_text_font(ll,&lv_font_montserrat_12,0);
        lv_obj_add_flag(ll,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(ll,8);
        lv_obj_add_event_cb(ll,az_cb,LV_EVENT_CLICKED,(void*)k);
    }
    g_scr=SCR_BROWSER;
    load_screen(scr);
}


/* ---- Settings: Bluetooth ----
 * OUTPUT   jack | remembered speakers (tap = use it, long-press = forget)
 * RECEIVE  phone -> R3 (A2DP sink) toggle + known phones
 * ADD      scan, tap a result to pair (a speaker becomes the output)
 * The list is rebuilt from bt.c snapshots whenever bt_version() changes. */
static char set_macs[2*BT_MAX][18];
static int  set_ver=-1;
static void set_back_cb(lv_event_t*e){ (void)e; show_home(); }
static void set_out_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e);
    bt_select_output(i<0?"":set_macs[i]); }
static void set_forget_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e);
    if(i>=0) bt_forget(set_macs[i]); }
static void set_pair_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e); bt_pair(set_macs[i]); }
static void set_scan_cb(lv_event_t*e){ (void)e; if(!bt_busy()) bt_scan(); }
static void set_rx_cb(lv_event_t*e){ lv_obj_t*sw=lv_event_get_target(e);
    bt_set_receiver(lv_obj_has_state(sw,LV_STATE_CHECKED)); }

static void set_section(lv_obj_t*l,const char*t){
    lv_obj_t*x=lv_label_create(l); lv_label_set_text(x,t);
    lv_obj_set_style_text_color(x,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_text_font(x,&lv_font_montserrat_14,0);
    lv_obj_set_style_pad_top(x,18,0); lv_obj_set_style_pad_left(x,12,0); lv_obj_set_style_pad_bottom(x,6,0);
}
static lv_obj_t* set_row(lv_obj_t*l,const char*icon,const char*title,const char*sub,int on){
    lv_obj_t*b=lv_btn_create(l); lv_obj_set_size(b,LV_PCT(100),sub?72:60);
    lv_obj_set_style_bg_color(b,lv_color_hex(on?C_PANEL2:C_PANEL),0);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL2),LV_STATE_PRESSED);
    lv_obj_set_style_radius(b,0,0); lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_border_width(b,on?3:0,0); lv_obj_set_style_border_side(b,LV_BORDER_SIDE_LEFT,0);
    lv_obj_set_style_border_color(b,lv_color_hex(C_ACCENT),0);
    lv_obj_clear_flag(b,LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t*ic=lv_label_create(b); lv_label_set_text(ic,icon);
    lv_obj_set_style_text_color(ic,lv_color_hex(on?C_ACCENT:C_DIM),0); lv_obj_align(ic,LV_ALIGN_LEFT_MID,4,0);
    lv_obj_t*t=lv_label_create(b); { char tt[96]; snprintf(tt,sizeof tt,"%s",title); fold(tt); lv_label_set_text(t,tt); }
    lv_label_set_long_mode(t,LV_LABEL_LONG_DOT); lv_obj_set_width(t,330);
    lv_obj_set_style_text_color(t,lv_color_hex(C_FG),0); lv_obj_set_style_text_font(t,&lv_font_montserrat_20,0);
    lv_obj_align(t,LV_ALIGN_LEFT_MID,40,sub?-12:0);
    if(sub){ lv_obj_t*s2=lv_label_create(b); lv_label_set_text(s2,sub);
        lv_obj_set_style_text_color(s2,lv_color_hex(C_DIM),0); lv_obj_align(s2,LV_ALIGN_LEFT_MID,40,14); }
    if(on){ lv_obj_t*ck=lv_label_create(b); lv_label_set_text(ck,LV_SYMBOL_OK);
        lv_obj_set_style_text_color(ck,lv_color_hex(C_ACCENT),0); lv_obj_align(ck,LV_ALIGN_RIGHT_MID,-6,0); }
    return b;
}
static void settings_fill(void){
    if(!set_list) return;
    set_ver=bt_version();
    lv_coord_t sy=lv_obj_get_scroll_y(set_list);
    lv_obj_clean(set_list);
    bt_dev_t d[BT_MAX], f[BT_MAX]; int nd=bt_devices(d,BT_MAX), nf=bt_found(f,BT_MAX), k=0;
    const char*out=bt_output();

    set_section(set_list,"OUTPUT");
    lv_obj_t*r=set_row(set_list,LV_SYMBOL_AUDIO,"Headphone jack",NULL,!out[0]);
    lv_obj_add_event_cb(r,set_out_cb,LV_EVENT_CLICKED,(void*)-1L);
    for(int i=0;i<nd;i++){ if(d[i].phone) continue;
        snprintf(set_macs[k],18,"%s",d[i].mac);
        int sel=!strcmp(out,d[i].mac);
        r=set_row(set_list,LV_SYMBOL_BLUETOOTH,d[i].name,
                  d[i].connected?"connected":(sel?"selected - not connected":"hold to forget"),sel);
        lv_obj_add_event_cb(r,set_out_cb,LV_EVENT_SHORT_CLICKED,(void*)(long)k);
        lv_obj_add_event_cb(r,set_forget_cb,LV_EVENT_LONG_PRESSED,(void*)(long)k);
        k++; }

    set_section(set_list,"RECEIVE FROM PHONE");
    lv_obj_t*rx=set_row(set_list,LV_SYMBOL_DOWNLOAD,"Bluetooth receiver","phone -> R3 -> jack",0);
    lv_obj_clear_flag(rx,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t*sw=lv_switch_create(rx); lv_obj_align(sw,LV_ALIGN_RIGHT_MID,-4,0);
    lv_obj_set_style_bg_color(sw,lv_color_hex(C_ACCENT),LV_PART_INDICATOR|LV_STATE_CHECKED);
    if(bt_receiver()) lv_obj_add_state(sw,LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw,set_rx_cb,LV_EVENT_VALUE_CHANGED,NULL);
    for(int i=0;i<nd;i++){ if(!d[i].phone) continue;
        snprintf(set_macs[k],18,"%s",d[i].mac);
        r=set_row(set_list,LV_SYMBOL_CALL,d[i].name,d[i].connected?"connected":"hold to forget",d[i].connected);
        lv_obj_add_event_cb(r,set_forget_cb,LV_EVENT_LONG_PRESSED,(void*)(long)k);
        k++; }

    set_section(set_list,"ADD A DEVICE");
    r=set_row(set_list,LV_SYMBOL_REFRESH,bt_busy()?"Working...":"Scan for devices",NULL,0);
    lv_obj_add_event_cb(r,set_scan_cb,LV_EVENT_CLICKED,NULL);
    for(int i=0;i<nf && k<2*BT_MAX;i++){
        snprintf(set_macs[k],18,"%s",f[i].mac);
        r=set_row(set_list,LV_SYMBOL_PLUS,f[i].name,"tap to pair",0);
        lv_obj_add_event_cb(r,set_pair_cb,LV_EVENT_CLICKED,(void*)(long)k);
        k++; }
    lv_obj_t*pad=lv_obj_create(set_list); lv_obj_set_size(pad,10,40);
    lv_obj_set_style_bg_opa(pad,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(pad,0,0);

    lv_obj_update_layout(set_list);
    lv_obj_scroll_to_y(set_list,sy,LV_ANIM_OFF);
    if(set_status){ char st[96]; bt_status(st,sizeof st); lv_label_set_text(set_status,st); }
}
static void show_settings(void){
    null_live();
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*hdr=lv_obj_create(scr); lv_obj_set_size(hdr,480,54); lv_obj_set_pos(hdr,0,0);
    lv_obj_set_style_bg_color(hdr,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(hdr,0,0);
    lv_obj_set_style_border_width(hdr,0,0); lv_obj_clear_flag(hdr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bk=lv_btn_create(hdr); lv_obj_set_size(bk,46,40); lv_obj_align(bk,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_ACCENT),0);
    lv_obj_add_event_cb(bk,set_back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bkl=lv_label_create(bk); lv_label_set_text(bkl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bkl,lv_color_hex(C_ACCENT),0); lv_obj_center(bkl);
    lv_obj_t*ttl=lv_label_create(hdr); lv_label_set_text(ttl,"Bluetooth");
    lv_obj_set_style_text_color(ttl,lv_color_hex(C_ACCENT),0); lv_obj_align(ttl,LV_ALIGN_LEFT_MID,56,0);

    set_list=lv_obj_create(scr); lv_obj_set_size(set_list,480,720-54-36); lv_obj_set_pos(set_list,0,54);
    lv_obj_set_style_bg_color(set_list,lv_color_hex(C_BG),0); lv_obj_set_style_border_width(set_list,0,0);
    lv_obj_set_style_radius(set_list,0,0); lv_obj_set_style_pad_all(set_list,0,0);
    lv_obj_set_style_pad_row(set_list,2,0);
    lv_obj_set_flex_flow(set_list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(set_list,LV_DIR_VER);

    lv_obj_t*sb=lv_obj_create(scr); lv_obj_set_size(sb,480,36); lv_obj_set_pos(sb,0,720-36);
    lv_obj_set_style_bg_color(sb,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(sb,0,0);
    lv_obj_set_style_border_width(sb,0,0); lv_obj_clear_flag(sb,LV_OBJ_FLAG_SCROLLABLE);
    set_status=lv_label_create(sb); lv_label_set_text(set_status,"");
    lv_label_set_long_mode(set_status,LV_LABEL_LONG_DOT); lv_obj_set_width(set_status,460);
    lv_obj_set_style_text_color(set_status,lv_color_hex(C_DIM),0); lv_obj_align(set_status,LV_ALIGN_LEFT_MID,0,0);

    settings_fill();
    bt_refresh();
    g_scr=SCR_SETTINGS;
    load_screen(scr);
}


/* ---- Idle watch face ----
 * After IDLE_MS without touch/keys: playing -> Now Playing; otherwise a classic analog
 * watch (12/6/9 numerals, date window at 3, weekday + date under the dial).
 * Tap anywhere to leave. Hands are lv_line objects updated once a second. */
#define IDLE_MS 30000
#define CK_CX 240
#define CK_CY 330
#define CK_R  210
static lv_obj_t *ck_h,*ck_m,*ck_s,*ck_day,*ck_date;
static lv_point_t ck_hp[2],ck_mp[2],ck_sp[2];
static lv_point_t ck_tick[60][2];
static int ck_last_sec=-1;
static void null_clock(void){ ck_h=ck_m=ck_s=ck_day=ck_date=NULL; ck_last_sec=-1; }
static void ck_hand(lv_point_t*p,double frac,int len,int tail){
    double a=frac*2*M_PI;                       /* 0 = 12 o'clock, clockwise */
    double sx=sin(a), cy=-cos(a);
    p[0].x=(lv_coord_t)(CK_CX - sx*tail); p[0].y=(lv_coord_t)(CK_CY - cy*tail);
    p[1].x=(lv_coord_t)(CK_CX + sx*len);  p[1].y=(lv_coord_t)(CK_CY + cy*len);
}
static lv_obj_t* ck_line(lv_obj_t*scr,lv_point_t*p,int w,uint32_t col){
    lv_obj_t*l=lv_line_create(scr); lv_line_set_points(l,p,2);
    lv_obj_set_style_line_width(l,w,0); lv_obj_set_style_line_color(l,lv_color_hex(col),0);
    lv_obj_set_style_line_rounded(l,1,0); lv_obj_clear_flag(l,LV_OBJ_FLAG_CLICKABLE);
    return l;
}
static void clock_update(void){
    if(!ck_h) return;
    time_t t=time(0); struct tm tm; localtime_r(&t,&tm);
    if(tm.tm_sec==ck_last_sec) return; ck_last_sec=tm.tm_sec;
    double s=tm.tm_sec, m=tm.tm_min+s/60.0, h=(tm.tm_hour%12)+m/60.0;
    ck_hand(ck_hp,h/12.0,120,22); lv_line_set_points(ck_h,ck_hp,2);
    ck_hand(ck_mp,m/60.0,180,26); lv_line_set_points(ck_m,ck_mp,2);
    ck_hand(ck_sp,s/60.0,192,40); lv_line_set_points(ck_s,ck_sp,2);
    char b[48];
    snprintf(b,sizeof b,"%d",tm.tm_mday); lv_label_set_text(ck_day,b);
    { char wd[16],mo[16]; strftime(wd,sizeof wd,"%A",&tm); strftime(mo,sizeof mo,"%B",&tm);
      snprintf(b,sizeof b,"%s %d %s %d",wd,tm.tm_mday,mo,tm.tm_year+1900); lv_label_set_text(ck_date,b); }
}
static void clock_tap_cb(lv_event_t*e){ (void)e; g_last_input=r3_tick_ms(); show_home(); }
static void show_clock(void){
    null_live();
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(0x000000),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr,clock_tap_cb,LV_EVENT_CLICKED,NULL);
    /* bezel */
    lv_obj_t*face=lv_obj_create(scr); lv_obj_set_size(face,2*CK_R+16,2*CK_R+16);
    lv_obj_set_pos(face,CK_CX-CK_R-8,CK_CY-CK_R-8);
    lv_obj_set_style_radius(face,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_bg_color(face,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_border_color(face,lv_color_hex(C_ACCENT),0); lv_obj_set_style_border_width(face,4,0);
    lv_obj_clear_flag(face,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    /* minute + hour ticks */
    for(int i=0;i<60;i++){ double a=i*M_PI/30.0, sx=sin(a), cy=-cos(a);
        int hour=(i%5==0); int r0=hour?CK_R-28:CK_R-12;
        ck_tick[i][0].x=(lv_coord_t)(CK_CX+sx*r0);    ck_tick[i][0].y=(lv_coord_t)(CK_CY+cy*r0);
        ck_tick[i][1].x=(lv_coord_t)(CK_CX+sx*(CK_R-4)); ck_tick[i][1].y=(lv_coord_t)(CK_CY+cy*(CK_R-4));
        ck_line(scr,ck_tick[i],hour?6:2,hour?C_FG:C_DIM); }
    /* numerals 12 / 6 / 9 and the date window at 3 */
    static const struct { const char*t; int dx,dy; } num[]={{"12",0,-150},{"6",0,150},{"9",-150,0}};
    for(int i=0;i<3;i++){ lv_obj_t*l=lv_label_create(scr); lv_label_set_text(l,num[i].t);
        lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0);
        lv_obj_set_style_text_color(l,lv_color_hex(C_FG),0);
        lv_obj_align(l,LV_ALIGN_TOP_LEFT,0,0); lv_obj_update_layout(l);
        lv_obj_set_pos(l,CK_CX+num[i].dx-lv_obj_get_width(l)/2,CK_CY+num[i].dy-lv_obj_get_height(l)/2); }
    lv_obj_t*win=lv_obj_create(scr); lv_obj_set_size(win,52,38); lv_obj_set_pos(win,CK_CX+150-26,CK_CY-19);
    lv_obj_set_style_radius(win,4,0); lv_obj_set_style_bg_color(win,lv_color_hex(C_FG),0);
    lv_obj_set_style_border_color(win,lv_color_hex(C_DIM),0); lv_obj_set_style_border_width(win,2,0);
    lv_obj_set_style_pad_all(win,0,0); lv_obj_clear_flag(win,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    ck_day=lv_label_create(win); lv_obj_set_style_text_color(ck_day,lv_color_hex(C_BG),0);
    lv_obj_set_style_text_font(ck_day,&lv_font_montserrat_20,0); lv_label_set_text(ck_day,""); lv_obj_center(ck_day);
    lv_obj_set_style_text_align(ck_day,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_width(ck_day,52);
    /* hands (hour, minute, second) + hub */
    ck_h=ck_line(scr,ck_hp,12,C_FG);
    ck_m=ck_line(scr,ck_mp,8,C_FG);
    ck_s=ck_line(scr,ck_sp,3,C_ACCENT);
    lv_obj_t*hub=lv_obj_create(scr); lv_obj_set_size(hub,20,20); lv_obj_set_pos(hub,CK_CX-10,CK_CY-10);
    lv_obj_set_style_radius(hub,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(hub,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_border_width(hub,0,0); lv_obj_clear_flag(hub,LV_OBJ_FLAG_CLICKABLE);
    /* full date under the dial */
    ck_date=lv_label_create(scr); lv_label_set_text(ck_date,"");
    lv_obj_set_width(ck_date,480); lv_obj_set_style_text_align(ck_date,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(ck_date,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_text_font(ck_date,&lv_font_montserrat_20,0);
    lv_obj_set_pos(ck_date,0,CK_CY+CK_R+40);
    clock_update();
    g_scr=SCR_CLOCK;
    load_screen(scr);
}
/* called from the main loop */
static void idle_poll(void){
    uint32_t now=r3_tick_ms();
    int playing = audio_is_playing() && !audio_is_paused();
    if(g_scr==SCR_CLOCK){ if(playing) show_player(g_pltitle,g_pldir); else clock_update(); return; }
    if(now-g_last_input < IDLE_MS) return;
    if(playing){ if(g_scr!=SCR_PLAYER) show_player(g_pltitle,g_pldir); }
    else if(!bt_busy()) show_clock();
}

/* ---- Home ---- */
static void tile_cb(lv_event_t*e){ const char*name=lv_event_get_user_data(e);
    if(!strcmp(name,"Music")){ strcpy(g_root,MUSIC_ROOT); strcpy(g_root_label,"Music"); show_browser(MUSIC_ROOT); }
    else if(!strcmp(name,"Podcasts")){ strcpy(g_root,PODCAST_ROOT); strcpy(g_root_label,"Podcasts"); show_browser(PODCAST_ROOT); }
    else if(!strcmp(name,"Settings")) show_settings();
    else DBG("tile (todo): %s\n",name); }
static void make_tile(lv_obj_t*g,const char*txt,uint32_t accent,int col,int row){
    lv_obj_t*b=lv_btn_create(g);
    lv_obj_set_grid_cell(b,LV_GRID_ALIGN_STRETCH,col,1,LV_GRID_ALIGN_STRETCH,row,1);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(b,0,0);
    lv_obj_set_style_border_color(b,lv_color_hex(accent),0); lv_obj_set_style_border_width(b,3,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(accent),LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,tile_cb,LV_EVENT_CLICKED,(void*)txt);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,txt);
    lv_obj_set_style_text_color(l,lv_color_hex(accent),0);
    lv_obj_set_style_text_color(l,lv_color_hex(C_BG),LV_STATE_PRESSED);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0); lv_obj_center(l);
}
static void show_home(void){
    null_live();
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bar=lv_obj_create(scr); lv_obj_set_size(bar,480,50); lv_obj_set_pos(bar,0,0);
    lv_obj_set_style_bg_color(bar,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(bar,0,0);
    lv_obj_set_style_border_width(bar,0,0); lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*nm=lv_label_create(bar); lv_label_set_text(nm,"r3ii");
    lv_obj_set_style_text_color(nm,lv_color_hex(C_ACCENT),0); lv_obj_align(nm,LV_ALIGN_LEFT_MID,4,0);
    clock_lbl=lv_label_create(bar); lv_label_set_text(clock_lbl,"--:--");
    lv_obj_set_style_text_color(clock_lbl,lv_color_hex(C_FG),0); lv_obj_align(clock_lbl,LV_ALIGN_RIGHT_MID,-4,0);
    lv_obj_t*h=lv_label_create(scr); lv_label_set_text(h,"HELLO");
    lv_obj_set_style_text_color(h,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0); lv_obj_align(h,LV_ALIGN_TOP_MID,0,70);
    static lv_coord_t cols[]={LV_GRID_FR(1),LV_GRID_FR(1),LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[]={LV_GRID_FR(1),LV_GRID_FR(1),LV_GRID_TEMPLATE_LAST};
    lv_obj_t*g=lv_obj_create(scr); lv_obj_set_style_bg_opa(g,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(g,0,0); lv_obj_set_style_pad_all(g,0,0);
    lv_obj_set_size(g,440,440); lv_obj_align(g,LV_ALIGN_TOP_MID,0,130);
    lv_obj_set_style_pad_row(g,20,0); lv_obj_set_style_pad_column(g,20,0);
    lv_obj_set_grid_dsc_array(g,cols,rows); lv_obj_clear_flag(g,LV_OBJ_FLAG_SCROLLABLE);
    make_tile(g,"Music",C_ACCENT,0,0); make_tile(g,"Podcasts",C_ACCENT2,1,0);
    make_tile(g,"Games",C_DIM,0,1); make_tile(g,"Settings",C_ACCENT2,1,1);
    g_scr=SCR_HOME;
    load_screen(scr);
}

/* ---- physical keys ---- */
static int kf_pwr=-1, kf_media=-1;      /* event0 (power/prev), event2 (vol/play/next) */
static uint32_t pwr_down=0; static int pwr_held=0;
static void bump_vol(int d){ int v=audio_get_volume()+d; if(v<0)v=0; if(v>255)v=255;
    audio_set_volume(v);
    if(g_volbar){ lv_bar_set_value(g_volbar,v,LV_ANIM_OFF);
        lv_obj_clear_flag(g_volov,LV_OBJ_FLAG_HIDDEN); g_vol_hide=r3_tick_ms()+2000; } }
static void make_vol_overlay(void){   /* top-layer bar shown only while volume is changing */
    lv_obj_t*top=lv_layer_top();
    g_volov=lv_obj_create(top); lv_obj_set_size(g_volov,480,54); lv_obj_set_pos(g_volov,0,0);
    lv_obj_set_style_bg_color(g_volov,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(g_volov,0,0);
    lv_obj_set_style_border_color(g_volov,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_border_width(g_volov,2,0); lv_obj_set_style_border_side(g_volov,LV_BORDER_SIDE_BOTTOM,0);
    lv_obj_clear_flag(g_volov,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*ic=lv_label_create(g_volov); lv_label_set_text(ic,LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(ic,lv_color_hex(C_ACCENT),0); lv_obj_align(ic,LV_ALIGN_LEFT_MID,10,0);
    g_volbar=lv_bar_create(g_volov); lv_obj_set_size(g_volbar,390,14); lv_obj_align(g_volbar,LV_ALIGN_RIGHT_MID,-14,0);
    lv_bar_set_range(g_volbar,0,255); lv_bar_set_value(g_volbar,audio_get_volume(),LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_volbar,lv_color_hex(C_PANEL),LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_volbar,lv_color_hex(C_ACCENT),LV_PART_INDICATOR);
    lv_obj_add_flag(g_volov,LV_OBJ_FLAG_HIDDEN);
}
static void power_short(void){
    if(g_scr==SCR_PLAYER) show_browser(g_pldir[0]?g_pldir:MUSIC_ROOT);
    else if(g_pln>0)      show_player(g_pltitle,g_pldir);
    else                  show_browser(MUSIC_ROOT);
}
static void do_poweroff(void){ save_resume(); audio_stop(); sync(); system("poweroff"); }
static void keys_poll(void){
    struct input_event e;
    if(kf_pwr>=0) while(read(kf_pwr,&e,sizeof e)==sizeof e){
        if(e.type!=EV_KEY) continue;
        g_last_input=r3_tick_ms();
        if(e.code==116){ /* power */
            if(e.value==1){ pwr_down=r3_tick_ms(); pwr_held=1; }
            else if(e.value==0 && pwr_held){ pwr_held=0;
                if(r3_tick_ms()-pwr_down < 1200) power_short(); }
        } else if(e.code==165 && e.value==1){ pl_prev(); }  /* previous */
    }
    if(kf_media>=0) while(read(kf_media,&e,sizeof e)==sizeof e){
        if(e.type!=EV_KEY) continue;
        g_last_input=r3_tick_ms();
        if(e.code==115 && e.value){ bump_vol(+12); }        /* vol up (incl. autorepeat) */
        else if(e.code==114 && e.value){ bump_vol(-12); }   /* vol down */
        else if(e.code==163 && e.value==1){ pl_next(); }    /* next */
        else if(e.code==164 && e.value==1){                 /* play/pause */
            if(!audio_is_playing() && g_pln>0) pl_play(g_pli);
            else { audio_toggle_pause();
                   if(np_play) lv_label_set_text(np_play,audio_is_paused()?LV_SYMBOL_PLAY:LV_SYMBOL_PAUSE); }
        }
    }
    /* long-press power -> off (fires while still held) */
    if(pwr_held && r3_tick_ms()-pwr_down>=1200){ pwr_held=0; do_poweroff(); }
}

static void heartbeat(void){ static uint32_t lh=0; uint32_t n=r3_tick_ms();
    if(n-lh>=2000){ lh=n; printf("hb %lu\n",(unsigned long)n); } }
static void fmt_ms(long ms,char*o,int c){ long s=ms/1000; snprintf(o,c,"%ld:%02ld",s/60,s%60); }
static void tick_ui(void){
    static uint32_t lc=0,lp=0; uint32_t now=r3_tick_ms();
    if(now-lc>=1000){ lc=now; if(clock_lbl){ time_t t=time(0); char b[8];
        strftime(b,sizeof b,"%H:%M",localtime(&t)); lv_label_set_text(clock_lbl,b);} }
    if(now-lp>=300 && (np_pos||np_prog)){ lp=now;
        long pos=audio_pos_ms(), dur=audio_dur_ms();
        if(np_pos){ char p[16],d[16]; fmt_ms(pos,p,sizeof p); fmt_ms(dur,d,sizeof d);
            char line[40]; snprintf(line,sizeof line,"%s / %s",p,d); lv_label_set_text(np_pos,line); }
        if(np_prog && dur>0 && !lv_obj_has_state(np_prog,LV_STATE_PRESSED) && now>g_prog_hold)
            lv_slider_set_value(np_prog,(int32_t)((long long)pos*1000/dur),LV_ANIM_OFF);
    }
    if(set_list && bt_version()!=set_ver) settings_fill();
    static uint32_t lr=0;
    if(bt_receiver() && now-lr>=120){ lr=now; audio_dac_follow(); }   /* cheap: cached mixer ctls */
    if(audio_bt_failed() && np_play) lv_label_set_text(np_play,LV_SYMBOL_PLAY);
    if(g_volov && !lv_obj_has_flag(g_volov,LV_OBJ_FLAG_HIDDEN) && now>g_vol_hide)
        lv_obj_add_flag(g_volov,LV_OBJ_FLAG_HIDDEN);   /* auto-hide the volume overlay */
}

/* resume: pre-load last track so Now Playing opens ready (press play to resume) */
static void load_resume(void){
    FILE*f=fopen(RESUME_FILE,"r"); if(!f)return;
    char dir[1024]={0},name[512]={0};
    if(!fgets(dir,sizeof dir,f)){ fclose(f); return; }  dir[strcspn(dir,"\n")]=0;
    if(!fgets(name,sizeof name,f)){ fclose(f); return; } name[strcspn(name,"\n")]=0;
    fclose(f);
    if(!dir[0]||!name[0]) return;
    /* browse-back from Now Playing must stop at the section the track lives in */
    if(!strncmp(dir,PODCAST_ROOT,strlen(PODCAST_ROOT))){ strcpy(g_root,PODCAST_ROOT); strcpy(g_root_label,"Podcasts"); }
    else { strcpy(g_root,MUSIC_ROOT); strcpy(g_root_label,"Music"); }
    pl_build(dir,name);
    pretty(name,g_pltitle,sizeof g_pltitle);
    show_player(g_pltitle,dir);   /* not playing yet */
}

int main(int argc,char**argv){
    if(argc>1 && !strcmp(argv[1],"-d")){
        if(fork()>0)_exit(0); setsid();
        if(fork()>0)_exit(0);
        int fd=open("/usr/data/r3out.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd>=0){ dup2(fd,1); dup2(fd,2); if(fd>2)close(fd); }
        int nf=open("/dev/null",O_RDONLY); if(nf>=0){ dup2(nf,0); if(nf>0)close(nf); }
    }
    setvbuf(stdout,NULL,_IONBF,0);
    signal(SIGHUP,SIG_IGN); signal(SIGPIPE,SIG_IGN); signal(SIGCHLD,SIG_IGN);
    dbgf=fopen("/usr/data/r3dbg.log","w");
    mkdir("/usr/data/mnt",0755); mkdir("/usr/data/mnt/sd_0",0755);
    mount("/dev/mmcblk0p1","/usr/data/mnt/sd_0","vfat",0,"codepage=936,iocharset=utf8");
    /* NOTE: no hiby_player DAC-init — the "it makes sound loud" was a volume red herring;
       the codec needs no stock init, so we boot straight into our UI (fast, no stock flash). */
    fb_init(); lv_init();
    static lv_disp_draw_buf_t db; static lv_color_t buf[480*120];
    lv_disp_draw_buf_init(&db,buf,NULL,480*120);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.draw_buf=&db; dd.flush_cb=disp_flush; dd.hor_res=480; dd.ver_res=720; lv_disp_drv_register(&dd);
    tsfd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
    kf_pwr=open("/dev/input/event0",O_RDONLY|O_NONBLOCK);
    kf_media=open("/dev/input/event2",O_RDONLY|O_NONBLOCK);
    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type=LV_INDEV_TYPE_POINTER; id.read_cb=ts_read; lv_indev_drv_register(&id);
    make_vol_overlay();
    show_home();
    load_resume();                 /* if a track was playing at power-off, open it ready */
    if(argc>2 && (!strcmp(argv[1],"-t")||!strcmp(argv[1],"-a")||!strcmp(argv[1],"-u"))){
        const char*b=strrchr(argv[2],'/');
        if(strcmp(argv[1],"-u")) audio_play(argv[2]);
        if(strcmp(argv[1],"-a")) show_player(b?b+1:argv[2],"/usr/data/mnt/sd_0/Music");
    }
    g_last_input=r3_tick_ms();
    bt_start();                    /* BT worker: daemons, auto-reconnect remembered speaker */
    printf("r3player UI up\n");
    for(;;){ lv_timer_handler();
        if(g_to_del){ lv_obj_del(g_to_del); g_to_del=NULL; }
        keys_poll();
        if(audio_finished()) pl_next();     /* album auto-advance */
        tick_ui(); idle_poll(); heartbeat(); usleep(15000); }
    return 0;
}
