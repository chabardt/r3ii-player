/* r3ii-player — LVGL UI for HiBy R3 Pro II (480x720 RGB565, goodix touch, X1600).
 * Home menu -> Music directory browser -> Now Playing with audio playback.
 * Dark theme (black + orange), playlist w/ auto-advance + prev/next, physical keys. */
#include "lvgl.h"
#include "r3_tick.h"
#include "audio.h"
#include "bt.h"
#include "cover.h"
#include "game.h"
#include "brick.h"
#include "coil.h"
#include "hiscore.h"
#include "wifi.h"
#include "pet.h"
#include "pad.h"
#include "share.h"
#include "smartmix.h"
#include <pthread.h>
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
#include <sys/time.h>
#include <sys/wait.h>
#include <spawn.h>

static void null_clock(void);
static uint32_t g_last_input;   /* last touch/key (idle -> watch face / Now Playing) */
static void screen_wake(void);
static int g_scr_off;             /* backlight off (screen timeout) */
static volatile int g_wake_swallow;   /* the touch that woke the screen must not click */

#define MUSIC_ROOT "/usr/data/mnt/sd_0/Music"
#define PODCAST_ROOT "/usr/data/mnt/sd_0/Others"
#define RESUME_FILE "/usr/data/r3_resume"
#define VOL_FILE "/usr/data/r3_vol"

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
        else if(e.type==EV_KEY&&e.code==BTN_TOUCH){ t_down=e.value; if(!e.value) g_wake_swallow=0; }
        g_last_input=r3_tick_ms();
    }
    d->point.x=t_x; d->point.y=t_y;
    d->state=(t_down&&!g_wake_swallow)?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
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
#define R_BTN     14         /* rounded buttons, tiles and rows */

/* live-updated widgets — NULL when the screen that owns them is gone */
extern const lv_img_dsc_t heart_img;   /* recolorable heart for the favorite toggle */
static lv_obj_t *clock_lbl, *np_pos, *np_play, *np_title, *np_prog, *np_dur, *np_fav, *g_dl_lbl;
static lv_obj_t *batt_lbl;    /* battery % on Home */
static lv_obj_t *btbat_lbl;   /* connected headset battery on Home */
static lv_obj_t *pet_bell;    /* Moko is calling (Home bar) */
static lv_obj_t *np_cover;    /* the cover container on Now Playing (NULL elsewhere) */
static lv_obj_t *set_list, *set_status;   /* Settings screen (NULL when not shown) */
static void null_live(void){ clock_lbl=np_pos=np_play=np_title=np_prog=np_dur=np_fav=NULL; g_dl_lbl=NULL; set_list=set_status=NULL; np_cover=NULL; batt_lbl=NULL; btbat_lbl=NULL; pet_bell=NULL; null_clock(); }
/* persistent volume overlay (lives on the top layer, survives screen changes) */
static lv_obj_t *g_volov, *g_volbar; static uint32_t g_vol_hide;
static uint32_t g_prog_hold;   /* don't auto-update the seek bar until this time (after a user seek) */

/* ---- screen state (for the physical power button) ---- */
enum { SCR_HOME, SCR_BROWSER, SCR_PLAYER, SCR_SETTINGS, SCR_CLOCK, SCR_GAME, SCR_PLAYLISTS };
static int g_scr=SCR_HOME;
static char g_cur_m3u[1024];                 /* playlist currently being viewed */
static char g_add_track[1600], g_add_title[256];  /* track pending "add to playlist" */
/* track-action sheet (long-press a track in a playlist/Favorites view): move/add/remove */
static char g_act_full[1600], g_act_title[256], g_act_src[1024]; static int g_act_idx, g_act_move;
static lv_obj_t *npl_ta;                      /* new-playlist name textarea */

/* switch screens now, but defer deleting the old one to the main loop (never mid-event) */
static lv_obj_t *g_to_del;
static void load_screen(lv_obj_t*ns){ lv_obj_t*old=lv_scr_act(); lv_scr_load(ns);
    if(old&&old!=ns) g_to_del=old; }
static void show_home(void);
static void show_browser(const char*path);
static void show_player(const char*title,const char*dir);
static void show_settings(void);
static void show_setup(void);
static void show_games(void);
static void show_gb_list(void);
static void np_set_cover(void);
static void show_playlists(void);
static void show_add_to_playlist(void);
static void show_track_actions(void);
static void show_track_pick(void);

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
static long g_resume_ms=-1;   /* where Play continues after a stop (boot resume, speaker lost) */
static char g_pldir[1024]="";
static char g_pltitle[256]="";
static void pl_free(void){ if(g_pl){ for(int i=0;i<g_pln;i++)free(g_pl[i]); free(g_pl);} g_pl=0; g_pln=0; g_pli=-1; }
static int cmp_pstr(const void*a,const void*b){ return strcasecmp(*(char*const*)a,*(char*const*)b); }
static const char* pl_base(const char*full){ const char*b=strrchr(full,'/'); return b?b+1:full; }
/* g_pl holds FULL track paths (so a playlist can span folders / a shared list). */
static void pl_build(const char*dir,const char*selname){
    pl_free();
    strncpy(g_pldir,dir,sizeof g_pldir-1); g_pldir[sizeof g_pldir-1]=0;
    char **tmp=malloc(sizeof(char*)*4096); int n=0;
    if(!strncmp(dir,MUSIC_ROOT,strlen(MUSIC_ROOT)) && smartmix_ready()){
        char (*ff)[1024]=malloc((size_t)1024*1024);   /* up to 1024 tracks in a folder */
        if(ff){ int m=smartmix_folder(dir,ff,1024); if(m>0) for(int i=0;i<m;i++) tmp[n++]=strdup(ff[i]); free(ff); }
    }
    if(n==0){ DIR*dp=opendir(dir); if(dp){ struct dirent*de;
        while((de=readdir(dp))&&n<4096){ if(de->d_name[0]=='.')continue;
            if(!is_audio(de->d_name))continue;
            char fp[1600]; snprintf(fp,sizeof fp,"%s/%s",dir,de->d_name); tmp[n++]=strdup(fp); }
        closedir(dp); } }
    qsort(tmp,n,sizeof(char*),cmp_pstr);
    g_pl=tmp; g_pln=n; g_pli=0;
    if(selname) for(int i=0;i<n;i++) if(!strcmp(pl_base(tmp[i]),selname)){ g_pli=i; break; }
}
static void pl_play_at(int i,long ms);
static void pl_play(int i){ pl_play_at(i,0); }
static void pl_resume(void){ pl_play_at(g_pli, g_resume_ms>0?g_resume_ms:0); }
static void pl_play_at(int i,long ms){
    if(i<0||i>=g_pln)return; g_pli=i; g_resume_ms=-1;
    const char*full=g_pl[i];
    pretty(pl_base(full),g_pltitle,sizeof g_pltitle);
    strncpy(g_pldir,full,sizeof g_pldir-1); g_pldir[sizeof g_pldir-1]=0;   /* track's folder (cover + back) */
    char*sl=strrchr(g_pldir,'/'); if(sl)*sl=0;
    audio_play_at(full,ms);
    if(np_title) lv_label_set_text(np_title,g_pltitle);
    if(np_play)  lv_label_set_text(np_play,LV_SYMBOL_PAUSE);
    np_set_cover();
}
static void pl_next(void){ if(g_pli+1<g_pln) pl_play(g_pli+1); }
static void pl_prev(void){ if(audio_pos_ms()>3000) pl_play(g_pli);   /* >3s: restart track */
    else if(g_pli>0) pl_play(g_pli-1); else pl_play(0); }

/* ---- user playlists + favorites: extended M3U on the SD (track paths relative to the
 * card root, so a list is portable and shareable between devices) ---- */
#define SD_ROOT    "/usr/data/mnt/sd_0"
#define PL_DIR     SD_ROOT "/Playlists"
#define FAV_M3U    PL_DIR "/Favorites.m3u"
static const char* rel_of(const char*full){        /* ".../sd_0/Music/x" -> "Music/x" */
    size_t n=strlen(SD_ROOT);
    return (!strncmp(full,SD_ROOT "/",n+1)) ? full+n+1 : full; }
static int m3u_has(const char*m3u,const char*full){
    FILE*f=fopen(m3u,"r"); if(!f) return 0; const char*rel=rel_of(full);
    char l[1024]; int yes=0;
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\r\n")]=0;
        if(l[0]&&l[0]!='#'&&(!strcmp(l,rel)||!strcmp(l,full))){ yes=1; break; } }
    fclose(f); return yes; }
static void m3u_append(const char*m3u,const char*full,const char*title){
    if(m3u_has(m3u,full)) return;
    mkdir(PL_DIR,0777);
    int isnew=access(m3u,F_OK)!=0;
    FILE*f=fopen(m3u,"a"); if(!f) return;
    if(isnew) fprintf(f,"#EXTM3U\n");
    fprintf(f,"#EXTINF:-1,%s\n%s\n", title&&title[0]?title:pl_base(full), rel_of(full));
    fclose(f); sync(); }
static void m3u_remove(const char*m3u,const char*full){
    FILE*f=fopen(m3u,"r"); if(!f) return; const char*rel=rel_of(full);
    char **a=malloc(sizeof(char*)*8192); int n=0,l_cap=1024; char l[1024];
    while(fgets(l,sizeof l,f)&&n<8192){ l[strcspn(l,"\r\n")]=0; a[n++]=strdup(l); } fclose(f);
    FILE*w=fopen(m3u,"w"); if(!w){ for(int i=0;i<n;i++)free(a[i]); free(a); return; }
    (void)l_cap;
    for(int i=0;i<n;i++){
        int istrk = a[i][0] && a[i][0]!='#' && (!strcmp(a[i],rel)||!strcmp(a[i],full));
        int isext = !strncmp(a[i],"#EXTINF",7) && i+1<n && (!strcmp(a[i+1],rel)||!strcmp(a[i+1],full));
        if(istrk||isext) continue;
        fprintf(w,"%s\n",a[i]);
    }
    fclose(w); for(int i=0;i<n;i++)free(a[i]); free(a); sync(); }
static int m3u_count(const char*m3u){ FILE*f=fopen(m3u,"r"); if(!f)return 0; int n=0; char l[1024];
    while(fgets(l,sizeof l,f)) if(l[0]&&l[0]!='#') n++; fclose(f); return n; }
/* read the idx-th track (0-based) of an m3u: its full path + #EXTINF title. Returns 1 if found. */
static int m3u_nth(const char*m3u,int idx,char*full,int fcap,char*title,int tcap){
    FILE*f=fopen(m3u,"r"); if(!f) return 0; char l[1024],t[256]=""; int i=0,ok=0;
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\r\n")]=0; if(!l[0]) continue;
        if(!strncmp(l,"#EXTINF",7)){ char*c=strchr(l,','); if(c) snprintf(t,sizeof t,"%s",c+1); continue; }
        if(l[0]=='#') continue;
        if(i==idx){ if(l[0]=='/') snprintf(full,fcap,"%s",l); else snprintf(full,fcap,"%s/%s",SD_ROOT,l);
            if(title){ if(t[0]) snprintf(title,tcap,"%s",t); else snprintf(title,tcap,"%s",pl_base(full)); }
            ok=1; break; }
        t[0]=0; i++; }
    fclose(f); return ok; }
static int fav_has(const char*full){ return m3u_has(FAV_M3U,full); }
static void fav_toggle(const char*full,const char*title){
    if(fav_has(full)) m3u_remove(FAV_M3U,full); else m3u_append(FAV_M3U,full,title); }
/* load an m3u's existing tracks into g_pl (full paths); returns count */
static int pl_load_m3u(const char*m3u){
    FILE*f=fopen(m3u,"r"); if(!f) return 0;
    pl_free();
    char **tmp=malloc(sizeof(char*)*4096); int n=0; char l[1024]; struct stat st;
    while(fgets(l,sizeof l,f)&&n<4096){ l[strcspn(l,"\r\n")]=0;
        if(!l[0]||l[0]=='#') continue;
        char full[1600];
        if(l[0]=='/') snprintf(full,sizeof full,"%s",l);
        else snprintf(full,sizeof full,"%s/%s",SD_ROOT,l);
        if(stat(full,&st)!=0) continue;            /* skip missing tracks (Phase 2: offer to fetch) */
        tmp[n++]=strdup(full); }
    fclose(f);
    g_pl=tmp; g_pln=n; g_pli=0; g_pldir[0]=0;
    return n; }

/* ---- resume across power-off ---- */
static void save_resume(void){
    if(g_pln<=0||g_pli<0) return;
    FILE*f=fopen(RESUME_FILE,"w"); if(!f)return;
    fprintf(f,"%s\n%s\n%ld\n",g_pldir,pl_base(g_pl[g_pli]),audio_pos_ms());
    fclose(f);
}

/* ---- album art (see cover.c) ---- */
#define COVER_W 480
#define COVER_H 436
static void np_set_cover(void){
    if(!np_cover) return;
    lv_obj_clean(np_cover);
    const char*tp = (g_pln>0 && g_pli>=0) ? g_pl[g_pli] : 0;
    const lv_img_dsc_t*img = g_pldir[0] ? cover_load(g_pldir,tp,COVER_W,COVER_H) : 0;
    if(img){ lv_obj_t*o=lv_img_create(np_cover); lv_img_set_src(o,img); lv_obj_center(o); }
    else { lv_obj_t*cl=lv_label_create(np_cover); lv_label_set_text(cl,LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(cl,lv_color_hex(C_ACCENT),0);
        lv_obj_set_style_text_font(cl,&lv_font_montserrat_28,0); lv_obj_center(cl); }
}

/* ---- Now Playing ---- */
static void np_back_cb(lv_event_t*e){ (void)e; show_browser(g_pldir[0]?g_pldir:MUSIC_ROOT); }
static void np_fav_cb(lv_event_t*e){ (void)e;
    if(!(g_pln>0&&g_pli>=0)) return;
    fav_toggle(g_pl[g_pli],g_pltitle);
    if(np_fav) lv_obj_set_style_img_recolor(np_fav,lv_color_hex(fav_has(g_pl[g_pli])?C_ACCENT:C_DIM),0); }
static void np_add_cb(lv_event_t*e){ (void)e; show_add_to_playlist(); }
static void np_playpause_cb(lv_event_t*e){ (void)e;
    if(!audio_is_playing() && g_pln>0){ pl_resume(); return; }   /* resume-ready: continue where it stopped */
    audio_toggle_pause(); save_resume();
    if(np_play) lv_label_set_text(np_play, audio_is_paused()?LV_SYMBOL_PLAY:LV_SYMBOL_PAUSE); }
static void np_next_cb(lv_event_t*e){ (void)e;
    if(g_pln>0&&g_pli>=0 && audio_dur_ms()>0 && audio_pos_ms()<audio_dur_ms()/2) smartmix_note(g_pl[g_pli],0); /* early skip */
    pl_next(); }
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
    lv_obj_set_style_border_width(b,2,0); lv_obj_set_style_radius(b,R_BTN,0);
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
    /* top bar holds the back button so it never overlaps the cover art */
    lv_obj_t*tb=lv_obj_create(scr); lv_obj_set_size(tb,480,44); lv_obj_set_pos(tb,0,0);
    lv_obj_set_style_bg_color(tb,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(tb,0,0);
    lv_obj_set_style_border_width(tb,0,0); lv_obj_set_style_pad_all(tb,0,0);
    lv_obj_clear_flag(tb,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bk=lv_btn_create(tb); lv_obj_set_size(bk,46,36); lv_obj_align(bk,LV_ALIGN_LEFT_MID,4,0);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(bk,R_BTN,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_ACCENT),0);
    lv_obj_add_event_cb(bk,np_back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl,lv_color_hex(C_ACCENT),0); lv_obj_center(bl);
    /* favorite (✓) + add-to-playlist (＋) on the right of the top bar */
    { int isfav=(g_pln>0&&g_pli>=0)&&fav_has(g_pl[g_pli]);
      lv_obj_t*fb=lv_btn_create(tb); lv_obj_set_size(fb,46,36); lv_obj_align(fb,LV_ALIGN_RIGHT_MID,-4,0);
      lv_obj_set_style_bg_color(fb,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(fb,R_BTN,0);
      lv_obj_set_style_border_width(fb,2,0); lv_obj_set_style_border_color(fb,lv_color_hex(C_ACCENT),0);
      lv_obj_add_event_cb(fb,np_fav_cb,LV_EVENT_CLICKED,NULL);
      np_fav=lv_img_create(fb); lv_img_set_src(np_fav,&heart_img);
      lv_obj_set_style_img_recolor_opa(np_fav,LV_OPA_COVER,0);
      lv_obj_set_style_img_recolor(np_fav,lv_color_hex(isfav?C_ACCENT:C_DIM),0); lv_obj_center(np_fav);
      lv_obj_t*ab=lv_btn_create(tb); lv_obj_set_size(ab,46,36); lv_obj_align(ab,LV_ALIGN_RIGHT_MID,-56,0);
      lv_obj_set_style_bg_color(ab,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(ab,R_BTN,0);
      lv_obj_set_style_border_width(ab,2,0); lv_obj_set_style_border_color(ab,lv_color_hex(C_ACCENT),0);
      lv_obj_add_event_cb(ab,np_add_cb,LV_EVENT_CLICKED,NULL);
      lv_obj_t*al=lv_label_create(ab); lv_label_set_text(al,LV_SYMBOL_PLUS);
      lv_obj_set_style_text_color(al,lv_color_hex(C_ACCENT),0); lv_obj_center(al); }
    /* cover (full width, directly below the bar) */
    lv_obj_t*cov=lv_obj_create(scr); lv_obj_set_size(cov,COVER_W,COVER_H); lv_obj_set_pos(cov,0,44);
    lv_obj_set_style_radius(cov,0,0); lv_obj_set_style_border_width(cov,0,0);
    lv_obj_set_style_bg_color(cov,lv_color_hex(C_PANEL),0);
    lv_obj_clear_flag(cov,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cov,0,0);
    np_cover=cov; np_set_cover();
    /* title */
    np_title=lv_label_create(scr); lv_label_set_text(np_title,title);
    lv_label_set_long_mode(np_title,LV_LABEL_LONG_WRAP); lv_obj_set_width(np_title,448);
    lv_obj_set_style_text_align(np_title,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(np_title,lv_color_hex(C_FG),0);
    lv_obj_set_style_text_font(np_title,&lv_font_montserrat_20,0);
    lv_obj_align(np_title,LV_ALIGN_TOP_MID,0,492);
    /* compact progress row: elapsed · thin bar · total, on one line */
    lv_obj_t*pr=lv_obj_create(scr);
    lv_obj_set_size(pr,452,26); lv_obj_align(pr,LV_ALIGN_TOP_MID,0,584);
    lv_obj_set_style_bg_opa(pr,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(pr,0,0);
    lv_obj_set_style_pad_all(pr,0,0); lv_obj_set_style_pad_column(pr,10,0);
    lv_obj_clear_flag(pr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pr,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pr,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    np_pos=lv_label_create(pr); lv_label_set_text(np_pos,"0:00");
    lv_obj_set_style_text_color(np_pos,lv_color_hex(C_DIM),0);
    lv_obj_set_style_text_font(np_pos,&lv_font_montserrat_14,0);
    lv_obj_set_width(np_pos,42); lv_obj_set_style_text_align(np_pos,LV_TEXT_ALIGN_RIGHT,0);
    np_prog=lv_slider_create(pr); lv_obj_set_height(np_prog,6); lv_obj_set_flex_grow(np_prog,1);
    lv_slider_set_range(np_prog,0,1000); lv_slider_set_value(np_prog,0,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(np_prog,lv_color_hex(C_PANEL),LV_PART_MAIN);
    lv_obj_set_style_radius(np_prog,3,LV_PART_MAIN);
    lv_obj_set_style_bg_color(np_prog,lv_color_hex(C_ACCENT),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(np_prog,lv_color_hex(C_FG),LV_PART_KNOB);
    lv_obj_set_style_pad_all(np_prog,5,LV_PART_KNOB);   /* small knob, bigger hit area */
    lv_obj_set_ext_click_area(np_prog,14);
    lv_obj_add_event_cb(np_prog,np_prog_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(np_prog,np_prog_cb,LV_EVENT_PRESS_LOST,NULL);
    np_dur=lv_label_create(pr); lv_label_set_text(np_dur,"0:00");
    lv_obj_set_style_text_color(np_dur,lv_color_hex(C_DIM),0);
    lv_obj_set_style_text_font(np_dur,&lv_font_montserrat_14,0);
    lv_obj_set_width(np_dur,42); lv_obj_set_style_text_align(np_dur,LV_TEXT_ALIGN_LEFT,0);
    /* transport row: prev | play/pause | next  (np_play = the symbol inside play/pause) */
    mk_tbtn(scr, 90,76,-150,622, C_PANEL,  LV_SYMBOL_PREV, np_prev_cb);
    lv_obj_t*ppb=mk_tbtn(scr,120,86,0,617, C_ACCENT, LV_SYMBOL_PAUSE, np_playpause_cb);
    np_play=lv_obj_get_child(ppb,0);   /* the label mk_tbtn created inside it */
    if(!audio_is_playing()||audio_is_paused()) lv_label_set_text(np_play,LV_SYMBOL_PLAY);   /* resume-ready / paused */
    mk_tbtn(scr, 90,76, 150,622, C_PANEL,  LV_SYMBOL_NEXT, np_next_cb);
    /* (progress/seek bar is now the compact inline row under the title; volume is the top overlay) */
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

/* A-Z rail: slide a finger along it -> the list follows smoothly, a big bubble shows the
 * letter. Rail order is "#" (digits/symbols sort first) then A..Z. Inside a letter's slot
 * the list position is interpolated toward the next letter's first entry, so dragging
 * scrolls continuously instead of jumping. */
#define RAIL_N 27
static lv_obj_t *g_firstof[27];   /* 0..25 = A..Z, 26 = other/#  (buttons in the current list) */
static lv_obj_t *g_brlist, *g_bubble, *g_bubble_lbl, *g_rail_lbl[RAIL_N]; static int g_rail_hl=-1;
static int rail_letter(int pos){ return pos==0?26:pos-1; }
static const char RAIL_TXT[RAIL_N][2]={"#","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
static void rail_highlight(int pos){
    if(pos==g_rail_hl) return;
    if(g_rail_hl>=0){ lv_obj_t*o=g_rail_lbl[g_rail_hl];
        lv_obj_set_style_text_color(o,lv_color_hex(g_firstof[rail_letter(g_rail_hl)]?C_ACCENT:C_DIM),0);
        lv_obj_set_style_text_font(o,&lv_font_montserrat_12,0); }
    g_rail_hl=pos;
    if(pos>=0){ lv_obj_set_style_text_color(g_rail_lbl[pos],lv_color_hex(C_FG),0);
        lv_obj_set_style_text_font(g_rail_lbl[pos],&lv_font_montserrat_16,0); }
}
static void rail_cb(lv_event_t*e){
    lv_event_code_t code=lv_event_get_code(e); lv_obj_t*rail=lv_event_get_target(e);
    if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){
        if(g_bubble) lv_obj_add_flag(g_bubble,LV_OBJ_FLAG_HIDDEN); rail_highlight(-1); return; }
    if(code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING) return;
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t a; lv_obj_get_coords(rail,&a);
    float f=(p.y-a.y1)*(float)RAIL_N/lv_area_get_height(&a);
    if(f<0)f=0; if(f>RAIL_N-0.001f)f=RAIL_N-0.001f;
    int pos=(int)f; float frac=f-pos;
    int k=-1; for(int q=pos;q<RAIL_N;q++) if(g_firstof[rail_letter(q)]){ k=q; break; }   /* empty letter: next one */
    if(k<0) for(int q=pos;q>=0;q--) if(g_firstof[rail_letter(q)]){ k=q; break; }
    if(k<0) return;
    if(k!=pos) frac=0;
    /* scroll target: from this letter's first row toward the next letter's first row */
    lv_coord_t y0=lv_obj_get_y(g_firstof[rail_letter(k)]), y1=-1;
    for(int q=k+1;q<RAIL_N && y1<0;q++) if(g_firstof[rail_letter(q)]) y1=lv_obj_get_y(g_firstof[rail_letter(q)]);
    if(y1<y0){ uint32_t n=lv_obj_get_child_cnt(g_brlist); lv_obj_t*last=n?lv_obj_get_child(g_brlist,n-1):NULL;
        y1 = last? lv_obj_get_y(last)+lv_obj_get_height(last) : y0; }
    lv_coord_t cur=lv_obj_get_scroll_y(g_brlist), maxy=cur+lv_obj_get_scroll_bottom(g_brlist);
    lv_coord_t t=(lv_coord_t)(y0+frac*(y1-y0)); if(t>maxy)t=maxy; if(t<0)t=0;
    if(t!=cur) lv_obj_scroll_to_y(g_brlist,t,LV_ANIM_OFF);
    /* bubble beside the finger, with the letter we are on */
    lv_label_set_text(g_bubble_lbl,RAIL_TXT[k]);
    lv_coord_t by=p.y-48; if(by<56)by=56; if(by>720-100)by=720-100;
    lv_obj_set_pos(g_bubble,330,by); lv_obj_clear_flag(g_bubble,LV_OBJ_FLAG_HIDDEN);
    rail_highlight(k);
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
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(bk,R_BTN,0);
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
    lv_obj_set_style_pad_all(list,0,0); lv_obj_set_style_pad_hor(list,8,0); lv_obj_set_style_pad_row(list,6,0);
    lv_obj_set_style_pad_top(list,6,0);
    /* entries: served from the smart index (instant) when browsing Music and the index
     * is ready; otherwise a live directory read (podcasts, first boot, any non-Music path). */
    entry_t*es=malloc(sizeof(entry_t)*4096); int ne=0, from_index=0;
    if(!strncmp(cur,MUSIC_ROOT,strlen(MUSIC_ROOT)) && smartmix_ready()){
        char (*nm)[256]=malloc((size_t)4096*256); char*dd=malloc(4096);
        if(nm&&dd){ int n=smartmix_listdir(cur,nm,dd,4096);
            if(n>=0){ ne=n; for(int i=0;i<n;i++){ strncpy(es[i].name,nm[i],255); es[i].name[255]=0; es[i].dir=dd[i]; } from_index=1; } }
        free(nm); free(dd);
    }
    if(!from_index){
        DIR*dp=opendir(cur);
        if(dp){ struct dirent*de;
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
        }
    }
    {
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
            lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,R_BTN,0);
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
    g_rail_hl=-1; g_brlist=list;
    for(int k=0;k<RAIL_N;k++){
        lv_obj_t*ll=lv_label_create(rail); lv_label_set_text(ll,RAIL_TXT[k]); g_rail_lbl[k]=ll;
        lv_obj_set_style_text_color(ll,lv_color_hex(g_firstof[rail_letter(k)]?C_ACCENT:C_DIM),0);
        lv_obj_set_style_text_font(ll,&lv_font_montserrat_12,0);
    }
    lv_obj_add_flag(rail,LV_OBJ_FLAG_CLICKABLE); lv_obj_set_ext_click_area(rail,14);   /* the whole rail is the slider */
    lv_obj_clear_flag(rail,LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(rail,rail_cb,LV_EVENT_ALL,NULL);
    /* the zoom bubble (hidden until the rail is touched) */
    g_bubble=lv_obj_create(scr); lv_obj_set_size(g_bubble,96,96);
    lv_obj_set_style_radius(g_bubble,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(g_bubble,lv_color_hex(C_ACCENT),0);
    lv_obj_set_style_bg_opa(g_bubble,LV_OPA_90,0); lv_obj_set_style_border_width(g_bubble,0,0);
    lv_obj_set_style_shadow_width(g_bubble,18,0); lv_obj_set_style_shadow_opa(g_bubble,LV_OPA_50,0);
    lv_obj_clear_flag(g_bubble,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    g_bubble_lbl=lv_label_create(g_bubble); lv_label_set_text(g_bubble_lbl,"");
    lv_obj_set_style_text_font(g_bubble_lbl,&lv_font_montserrat_48,0);
    lv_obj_set_style_text_color(g_bubble_lbl,lv_color_hex(C_BG),0); lv_obj_center(g_bubble_lbl);
    lv_obj_add_flag(g_bubble,LV_OBJ_FLAG_HIDDEN);
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
static void set_back_cb(lv_event_t*e){ (void)e; show_setup(); }
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
    lv_obj_set_style_radius(b,R_BTN,0); lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_border_width(b,on?2:0,0);
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
/* ---- Controllers: live button test (shows raw codes + the action they map to) ---- */
static lv_obj_t *pt_lbl, *pt_act[PA_N]; static lv_timer_t *pt_tmr;
static const char*PA_NAMES[PA_N]={"Up","Down","Left","Right","A","B","X","Y","L","R","+","-","Home"};
static void pt_tick(lv_timer_t*t){ (void)t; if(!pt_lbl) return;
    int ty,co,va; const char*dv; pad_last_raw(&ty,&co,&va,&dv); char b[200];
    snprintf(b,sizeof b,"%d controller(s)%s%s\nlast: %s %d = %d\n%s",pad_count(),pad_count()?": ":"",pad_count()?pad_name(0):"",
             ty==1?"key":ty==3?"axis":"event",co,va,dv);
    lv_label_set_text(pt_lbl,b);
    for(int a=0;a<PA_N;a++) lv_obj_set_style_bg_color(pt_act[a],lv_color_hex(pad_held(a)?C_ACCENT:C_PANEL2),0); }
static void pt_del_cb(lv_event_t*e){ (void)e; if(pt_tmr){ lv_timer_del(pt_tmr); pt_tmr=NULL; } pt_lbl=NULL; }
static void show_settings(void);
static void pt_back_cb(lv_event_t*e){ (void)e; show_settings(); }
static lv_obj_t* page_new(const char*title,lv_event_cb_t back,lv_obj_t**scr_out);
static void padtest_open_cb(lv_event_t*e){ (void)e;
    lv_obj_t*scr,*l=page_new("Test buttons",pt_back_cb,&scr);
    lv_obj_add_event_cb(scr,pt_del_cb,LV_EVENT_DELETE,NULL);
    pt_lbl=lv_label_create(l); lv_obj_set_style_text_color(pt_lbl,lv_color_hex(C_FG),0);
    lv_obj_set_style_pad_all(pt_lbl,12,0); lv_label_set_text(pt_lbl,"");
    lv_obj_t*g=lv_obj_create(l); lv_obj_set_size(g,LV_PCT(100),LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(g,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(g,0,0);
    lv_obj_set_flex_flow(g,LV_FLEX_FLOW_ROW_WRAP); lv_obj_set_style_pad_gap(g,8,0);
    for(int a=0;a<PA_N;a++){ pt_act[a]=lv_obj_create(g); lv_obj_set_size(pt_act[a],100,56);
        lv_obj_set_style_radius(pt_act[a],R_BTN,0); lv_obj_set_style_border_width(pt_act[a],0,0);
        lv_obj_clear_flag(pt_act[a],LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t*n=lv_label_create(pt_act[a]); lv_label_set_text(n,PA_NAMES[a]); lv_obj_center(n);
        lv_obj_set_style_text_color(n,lv_color_hex(C_FG),0); }
    lv_obj_t*h=lv_label_create(l); lv_label_set_text(h,"Press buttons: the matching action lights up.\n"
        "Pair a Joy-Con: hold its small sync button\nuntil the lights run, then Scan.");
    lv_obj_set_style_text_color(h,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(h,12,0);
    pt_tmr=lv_timer_create(pt_tick,60,NULL); pt_tick(NULL);
    g_scr=SCR_SETTINGS; load_screen(scr);
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
    char sub[48];
    for(int i=0;i<nd;i++){ if(d[i].phone||d[i].pad) continue;
        snprintf(set_macs[k],18,"%s",d[i].mac);
        int sel=!strcmp(out,d[i].mac);
        r=set_row(set_list,LV_SYMBOL_BLUETOOTH,d[i].name,
                  d[i].connected?(d[i].battery>=0?(snprintf(sub,sizeof sub,"connected - battery %d%%",d[i].battery),sub):"connected")
                                :(sel?"selected - not connected":"hold to forget"),sel);
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

    set_section(set_list,"CONTROLLERS");
    for(int i=0;i<nd;i++){ if(!d[i].pad) continue;
        snprintf(set_macs[k],18,"%s",d[i].mac);
        { char ps[48]; int chg=0, pb=-1; for(int j=0;j<pad_count();j++) if(!strcmp(pad_name(j),d[i].name)) pb=pad_battery(j,&chg);
          if(pb>=0) snprintf(ps,sizeof ps,"connected - battery %d%%%s",pb,chg?" (charging)":"");
          r=set_row(set_list,LV_SYMBOL_KEYBOARD,d[i].name,pb>=0?ps:d[i].connected?"connected":"press a button on it to reconnect",d[i].connected); }
        lv_obj_add_event_cb(r,set_forget_cb,LV_EVENT_LONG_PRESSED,(void*)(long)k);
        k++; }
    r=set_row(set_list,LV_SYMBOL_EYE_OPEN,"Test buttons",pad_count()?pad_name(0):"no controller connected",0);
    lv_obj_add_event_cb(r,padtest_open_cb,LV_EVENT_CLICKED,NULL);

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
    if(set_status){ char st[96]; bt_status(st,sizeof st); fold(st); lv_label_set_text(set_status,st); }
}
static void show_settings(void){
    null_live();
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*hdr=lv_obj_create(scr); lv_obj_set_size(hdr,480,54); lv_obj_set_pos(hdr,0,0);
    lv_obj_set_style_bg_color(hdr,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(hdr,0,0);
    lv_obj_set_style_border_width(hdr,0,0); lv_obj_clear_flag(hdr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bk=lv_btn_create(hdr); lv_obj_set_size(bk,46,40); lv_obj_align(bk,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(bk,R_BTN,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_ACCENT),0);
    lv_obj_add_event_cb(bk,set_back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bkl=lv_label_create(bk); lv_label_set_text(bkl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bkl,lv_color_hex(C_ACCENT),0); lv_obj_center(bkl);
    lv_obj_t*ttl=lv_label_create(hdr); lv_label_set_text(ttl,"Bluetooth");
    lv_obj_set_style_text_color(ttl,lv_color_hex(C_ACCENT),0); lv_obj_align(ttl,LV_ALIGN_LEFT_MID,56,0);

    set_list=lv_obj_create(scr); lv_obj_set_size(set_list,480,720-54-36); lv_obj_set_pos(set_list,0,54);
    lv_obj_set_style_bg_color(set_list,lv_color_hex(C_BG),0); lv_obj_set_style_border_width(set_list,0,0);
    lv_obj_set_style_radius(set_list,0,0); lv_obj_set_style_pad_all(set_list,0,0); lv_obj_set_style_pad_hor(set_list,8,0);
    lv_obj_set_style_pad_row(set_list,6,0);
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
    if(g_scr==SCR_GAME && (game_running()||brick_running()||coil_running()||pet_shown())) return;   /* don't yank the player out of a game */
    if(pt_lbl) return;                               /* controller test page stays up */
    if(now-g_last_input < IDLE_MS) return;
    if(playing){ if(g_scr!=SCR_PLAYER) show_player(g_pltitle,g_pldir); }
    else if(!bt_busy()) show_clock();
}


/* ---- preferences (display / sound), persisted in /usr/data/r3_prefs ---- */
#define PREFS_FILE "/usr/data/r3_prefs"
#define BL_PATH    "/sys/class/backlight/backlight_pwm0/brightness"   /* 0..101 */
static int p_bright=60, p_timeout=60 /* s, 0 = never */, p_filter=0, p_nos=0;
static int g_scr_off=0;           /* backlight off (screen timeout) */
static volatile int g_wake_swallow=0;   /* the touch that woke the screen must not click */
static void prefs_save(void){
    FILE*f=fopen(PREFS_FILE,"w"); if(!f) return;
    fprintf(f,"bright=%d\ntimeout=%d\nfilter=%d\nnos=%d\n",p_bright,p_timeout,p_filter,p_nos); fclose(f);
}
static void prefs_load(void){
    FILE*f=fopen(PREFS_FILE,"r"); if(!f) return; char l[64];
    while(fgets(l,sizeof l,f)){ int v;
        if(sscanf(l,"bright=%d",&v)==1) p_bright = v<5?5:(v>101?101:v);
        else if(sscanf(l,"timeout=%d",&v)==1) p_timeout = v<0?0:v;
        else if(sscanf(l,"filter=%d",&v)==1) p_filter = v<0?0:(v>4?4:v);
        else if(sscanf(l,"nos=%d",&v)==1) p_nos = v?1:0; }
    fclose(f);
}
static void bl_set(int v){ FILE*f=fopen(BL_PATH,"w"); if(f){ fprintf(f,"%d\n",v); fclose(f); } }
static void screen_off(void){ if(g_scr_off) return; g_scr_off=1; bl_set(0); }
static void screen_wake(void){ if(!g_scr_off) return; g_scr_off=0; g_last_input=r3_tick_ms();
    lv_obj_invalidate(lv_scr_act()); bl_set(p_bright); }

/* ---- shared page scaffolding: header with back + a vertical, scrollable column ---- */
static lv_obj_t* page_new(const char*title,lv_event_cb_t back,lv_obj_t**scr_out){
    null_live();
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_BG),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*hdr=lv_obj_create(scr); lv_obj_set_size(hdr,480,54); lv_obj_set_pos(hdr,0,0);
    lv_obj_set_style_bg_color(hdr,lv_color_hex(C_BAR),0); lv_obj_set_style_radius(hdr,0,0);
    lv_obj_set_style_border_width(hdr,0,0); lv_obj_clear_flag(hdr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bk=lv_btn_create(hdr); lv_obj_set_size(bk,46,40); lv_obj_align(bk,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(bk,R_BTN,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_ACCENT),0);
    lv_obj_add_event_cb(bk,back,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bkl=lv_label_create(bk); lv_label_set_text(bkl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bkl,lv_color_hex(C_ACCENT),0); lv_obj_center(bkl);
    lv_obj_t*ttl=lv_label_create(hdr); lv_label_set_text(ttl,title);
    lv_obj_set_style_text_color(ttl,lv_color_hex(C_ACCENT),0); lv_obj_align(ttl,LV_ALIGN_LEFT_MID,56,0);
    lv_obj_t*l=lv_obj_create(scr); lv_obj_set_size(l,480,720-54); lv_obj_set_pos(l,0,54);
    lv_obj_set_style_bg_color(l,lv_color_hex(C_BG),0); lv_obj_set_style_border_width(l,0,0);
    lv_obj_set_style_radius(l,0,0); lv_obj_set_style_pad_all(l,0,0); lv_obj_set_style_pad_hor(l,8,0); lv_obj_set_style_pad_row(l,6,0);
    lv_obj_set_flex_flow(l,LV_FLEX_FLOW_COLUMN); lv_obj_set_scroll_dir(l,LV_DIR_VER);
    *scr_out=scr; return l;
}
static lv_obj_t* page_box(lv_obj_t*l,int h){   /* a plain panel row for custom widgets */
    lv_obj_t*b=lv_obj_create(l); lv_obj_set_size(b,LV_PCT(100),h);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(b,R_BTN,0);
    lv_obj_set_style_border_width(b,0,0); lv_obj_set_style_pad_all(b,12,0);
    lv_obj_clear_flag(b,LV_OBJ_FLAG_SCROLLABLE); return b;
}
static lv_obj_t* style_choice(lv_obj_t*bm){   /* button-matrix as a one-of-N selector */
    lv_obj_set_style_bg_opa(bm,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(bm,0,0);
    lv_obj_set_style_pad_all(bm,0,0); lv_obj_set_style_pad_gap(bm,8,0);
    lv_obj_set_style_bg_color(bm,lv_color_hex(C_PANEL2),LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm,lv_color_hex(C_FG),LV_PART_ITEMS);
    lv_obj_set_style_radius(bm,R_BTN,LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm,0,LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm,lv_color_hex(C_ACCENT),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm,lv_color_hex(C_BG),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_btnmatrix_set_btn_ctrl_all(bm,LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm,true);
    return bm;
}
static void show_setup(void);
static void setup_back_cb(lv_event_t*e){ (void)e; show_setup(); }

/* ---- Display ---- */
static const int TO_VALS[]={30,60,120,300,0};
static const char*TO_MAP[]={"30 s","1 min","2 min","5 min","Never",""};
static void disp_bright_cb(lv_event_t*e){ lv_obj_t*s=lv_event_get_target(e);
    p_bright=lv_slider_get_value(s); bl_set(p_bright);
    if(lv_event_get_code(e)==LV_EVENT_RELEASED) prefs_save(); }
static void disp_to_cb(lv_event_t*e){ lv_obj_t*bm=lv_event_get_target(e);
    uint16_t i=lv_btnmatrix_get_selected_btn(bm); if(i>4) return;
    p_timeout=TO_VALS[i]; prefs_save(); }
static void show_display(void){
    lv_obj_t*scr,*l=page_new("Display",setup_back_cb,&scr);
    set_section(l,"BRIGHTNESS");
    lv_obj_t*b=page_box(l,70);
    lv_obj_t*lo=lv_label_create(b); lv_label_set_text(lo,LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(lo,lv_color_hex(C_DIM),0); lv_obj_align(lo,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_t*s=lv_slider_create(b); lv_obj_set_size(s,370,14); lv_obj_align(s,LV_ALIGN_LEFT_MID,40,0);
    lv_slider_set_range(s,5,101); lv_slider_set_value(s,p_bright,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s,lv_color_hex(C_PANEL2),LV_PART_MAIN);
    lv_obj_set_style_bg_color(s,lv_color_hex(C_ACCENT),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s,lv_color_hex(C_FG),LV_PART_KNOB);
    lv_obj_set_style_pad_all(s,8,LV_PART_KNOB); lv_obj_set_ext_click_area(s,20);
    lv_obj_add_event_cb(s,disp_bright_cb,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(s,disp_bright_cb,LV_EVENT_RELEASED,NULL);
    set_section(l,"SCREEN OFF AFTER");
    b=page_box(l,84);
    lv_obj_t*bm=lv_btnmatrix_create(b); lv_btnmatrix_set_map(bm,TO_MAP);
    lv_obj_set_size(bm,LV_PCT(100),58); lv_obj_center(bm); style_choice(bm);
    for(int i=0;i<5;i++) if(TO_VALS[i]==p_timeout) lv_btnmatrix_set_btn_ctrl(bm,i,LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(bm,disp_to_cb,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_t*n=lv_label_create(l); lv_label_set_text(n,"Music keeps playing with the screen off.\nPower key or a touch wakes it.");
    lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0);
    g_scr=SCR_SETTINGS; load_screen(scr);
}

/* ---- Sounds (DAC digital filter + NOS) ---- */
static const char*FL_MAP[]={"1","2","3","4","5",""};
static void snd_filter_cb(lv_event_t*e){ lv_obj_t*bm=lv_event_get_target(e);
    uint16_t i=lv_btnmatrix_get_selected_btn(bm); if(i>4) return;
    p_filter=i; audio_set_dac_filter(p_filter,p_nos); prefs_save(); }
static void snd_nos_cb(lv_event_t*e){ lv_obj_t*sw=lv_event_get_target(e);
    p_nos=lv_obj_has_state(sw,LV_STATE_CHECKED)?1:0; audio_set_dac_filter(p_filter,p_nos); prefs_save(); }
static void show_sounds(void){
    lv_obj_t*scr,*l=page_new("Sounds",setup_back_cb,&scr);
    set_section(l,"DAC DIGITAL FILTER");
    lv_obj_t*b=page_box(l,84);
    lv_obj_t*bm=lv_btnmatrix_create(b); lv_btnmatrix_set_map(bm,FL_MAP);
    lv_obj_set_size(bm,LV_PCT(100),58); lv_obj_center(bm); style_choice(bm);
    lv_btnmatrix_set_btn_ctrl(bm,p_filter,LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(bm,snd_filter_cb,LV_EVENT_VALUE_CHANGED,NULL);
    set_section(l,"OVERSAMPLING");
    lv_obj_t*r=set_row(l,LV_SYMBOL_AUDIO,"NOS mode","non-oversampling",0);
    lv_obj_clear_flag(r,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t*sw=lv_switch_create(r); lv_obj_align(sw,LV_ALIGN_RIGHT_MID,-4,0);
    lv_obj_set_style_bg_color(sw,lv_color_hex(C_ACCENT),LV_PART_INDICATOR|LV_STATE_CHECKED);
    if(p_nos) lv_obj_add_state(sw,LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw,snd_nos_cb,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_t*n=lv_label_create(l); lv_label_set_text(n,"Applied live; kept across restarts.");
    lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0);
    g_scr=SCR_SETTINGS; load_screen(scr);
}

/* ---- Date & time ---- */
static struct tm dt_edit; static lv_obj_t*dt_val[5], *dt_msg;
static const char*DT_NAME[5]={"Hour","Minute","Day","Month","Year"};
static void dt_show(void){
    char b[16]; int v[5]={dt_edit.tm_hour,dt_edit.tm_min,dt_edit.tm_mday,dt_edit.tm_mon+1,dt_edit.tm_year+1900};
    for(int i=0;i<5;i++) if(dt_val[i]){ snprintf(b,sizeof b,i<4?"%02d":"%d",v[i]); lv_label_set_text(dt_val[i],b); }
}
static void dt_step_cb(lv_event_t*e){ long k=(long)lv_event_get_user_data(e); int f=(int)(k>>1), d=(k&1)?+1:-1;
    if(f==0) dt_edit.tm_hour=(dt_edit.tm_hour+d+24)%24;
    else if(f==1) dt_edit.tm_min=(dt_edit.tm_min+d+60)%60;
    else if(f==2){ dt_edit.tm_mday+=d; if(dt_edit.tm_mday<1) dt_edit.tm_mday=31; if(dt_edit.tm_mday>31) dt_edit.tm_mday=1; }
    else if(f==3) dt_edit.tm_mon=(dt_edit.tm_mon+d+12)%12;
    else { dt_edit.tm_year+=d; if(dt_edit.tm_year<120) dt_edit.tm_year=120; }
    dt_show(); if(dt_msg) lv_label_set_text(dt_msg,""); }
static void dt_set_cb(lv_event_t*e){ (void)e;
    struct tm t=dt_edit; t.tm_sec=0; t.tm_isdst=-1;
    time_t when=mktime(&t);                 /* local time (TZ from /usr/data/localtime) */
    struct timeval tv={when,0};
    if(when!=(time_t)-1 && settimeofday(&tv,0)==0){
        system("hwclock -w -u >/dev/null 2>&1");   /* RTC keeps UTC */
        localtime_r(&when,&dt_edit); dt_show();
        if(dt_msg) lv_label_set_text(dt_msg,"Clock set.");
    } else if(dt_msg) lv_label_set_text(dt_msg,"Could not set the clock.");
}
static void show_datetime(void){
    lv_obj_t*scr,*l=page_new("Date & time",setup_back_cb,&scr);
    time_t now=time(0); localtime_r(&now,&dt_edit);
    for(int i=0;i<5;i++){
        lv_obj_t*b=page_box(l,72);
        lv_obj_t*nm=lv_label_create(b); lv_label_set_text(nm,DT_NAME[i]);
        lv_obj_set_style_text_color(nm,lv_color_hex(C_DIM),0); lv_obj_align(nm,LV_ALIGN_LEFT_MID,0,0);
        for(int s=0;s<2;s++){
            lv_obj_t*bt=lv_btn_create(b); lv_obj_set_size(bt,64,48);
            lv_obj_align(bt,LV_ALIGN_RIGHT_MID,s?0:-180,0);
            lv_obj_set_style_bg_color(bt,lv_color_hex(C_PANEL2),0); lv_obj_set_style_radius(bt,R_BTN,0);
            lv_obj_set_style_border_width(bt,2,0); lv_obj_set_style_border_color(bt,lv_color_hex(C_ACCENT),0);
            lv_obj_add_event_cb(bt,dt_step_cb,LV_EVENT_CLICKED,(void*)(long)(i*2+s));
            lv_obj_add_event_cb(bt,dt_step_cb,LV_EVENT_LONG_PRESSED_REPEAT,(void*)(long)(i*2+s));
            lv_obj_t*bl=lv_label_create(bt); lv_label_set_text(bl,s?LV_SYMBOL_PLUS:LV_SYMBOL_MINUS);
            lv_obj_set_style_text_color(bl,lv_color_hex(C_ACCENT),0); lv_obj_center(bl); }
        dt_val[i]=lv_label_create(b); lv_obj_set_width(dt_val[i],100);
        lv_obj_set_style_text_align(dt_val[i],LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_font(dt_val[i],&lv_font_montserrat_28,0);
        lv_obj_set_style_text_color(dt_val[i],lv_color_hex(C_FG),0);
        lv_obj_align(dt_val[i],LV_ALIGN_RIGHT_MID,-72,0);
    }
    lv_obj_t*sb=lv_btn_create(l); lv_obj_set_size(sb,LV_PCT(100),64);
    lv_obj_set_style_bg_color(sb,lv_color_hex(C_ACCENT),0); lv_obj_set_style_radius(sb,R_BTN,0);
    lv_obj_add_event_cb(sb,dt_set_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*sl=lv_label_create(sb); lv_label_set_text(sl,"Set clock");
    lv_obj_set_style_text_color(sl,lv_color_hex(C_BG),0); lv_obj_set_style_text_font(sl,&lv_font_montserrat_20,0);
    lv_obj_center(sl);
    dt_msg=lv_label_create(l); lv_label_set_text(dt_msg,"");
    lv_obj_set_style_text_color(dt_msg,lv_color_hex(C_ACCENT),0); lv_obj_set_style_pad_all(dt_msg,12,0);
    dt_show();
    g_scr=SCR_SETTINGS; load_screen(scr);
}

/* ---- Settings menu ---- */
static void setup_home_cb(lv_event_t*e){ (void)e; show_home(); }
/* ---- Settings > Wi-Fi: radio, networks, music server ---- */
static lv_obj_t *wf_list; static int wf_ver=-1; static wifi_net_t wf_nets[WIFI_MAX]; static int wf_n;
static char wf_pw_ssid[33]; static lv_obj_t *wf_pw_ta;
static void show_wifi(void);
static void wf_back_cb(lv_event_t*e){ (void)e; show_setup(); }
static void wf_del_cb(lv_event_t*e){   /* screens are deleted after the next one loads: only drop our own list */
    if(wf_list && lv_obj_get_screen(wf_list)==lv_event_get_target(e)) wf_list=NULL; }
static void wf_onoff_cb(lv_event_t*e){ lv_obj_t*sw=lv_event_get_target(e); wifi_set_enabled(lv_obj_has_state(sw,LV_STATE_CHECKED)); }
static void wf_srv_cb(lv_event_t*e){ lv_obj_t*sw=lv_event_get_target(e); wifi_set_server(lv_obj_has_state(sw,LV_STATE_CHECKED)); }
static void wf_scan_cb(lv_event_t*e){ (void)e; wifi_scan(); }
static void wf_forget_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e);
    if(i>=0 && i<wf_n && wf_nets[i].saved) wifi_forget(wf_nets[i].ssid); }
/* password page */
static void wf_pw_back_cb(lv_event_t*e){ (void)e; wf_pw_ta=NULL; show_wifi(); }
static void wf_pw_go(void){ if(!wf_pw_ta) return; const char*pw=lv_textarea_get_text(wf_pw_ta);
    if(strlen(pw)<8) return;                                  /* WPA needs 8+ characters */
    wifi_connect(wf_pw_ssid,pw); lv_textarea_set_text(wf_pw_ta,""); wf_pw_ta=NULL; show_wifi(); }
static void wf_pw_kb_cb(lv_event_t*e){ if(lv_event_get_code(e)==LV_EVENT_READY) wf_pw_go();
    else if(lv_event_get_code(e)==LV_EVENT_CANCEL){ wf_pw_ta=NULL; show_wifi(); } }
static void wf_pw_show_cb(lv_event_t*e){ lv_obj_t*sw=lv_event_get_target(e);
    if(wf_pw_ta) lv_textarea_set_password_mode(wf_pw_ta,!lv_obj_has_state(sw,LV_STATE_CHECKED)); }
static void show_wifi_password(const char*ssid){
    snprintf(wf_pw_ssid,sizeof wf_pw_ssid,"%s",ssid);
    lv_obj_t*scr,*l=page_new("Password",wf_pw_back_cb,&scr);
    lv_obj_set_height(l,720-54-300);
    lv_obj_t*t=lv_label_create(l); char tt[64]; snprintf(tt,sizeof tt,"%s",ssid); fold(tt); lv_label_set_text(t,tt);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(t,lv_color_hex(C_FG),0);
    lv_obj_set_style_pad_top(t,14,0); lv_obj_set_style_pad_left(t,6,0);
    wf_pw_ta=lv_textarea_create(l); lv_obj_set_width(wf_pw_ta,LV_PCT(100)); lv_textarea_set_one_line(wf_pw_ta,true);
    lv_textarea_set_password_mode(wf_pw_ta,true); lv_textarea_set_max_length(wf_pw_ta,63);
    lv_textarea_set_placeholder_text(wf_pw_ta,"Wi-Fi password");
    lv_obj_set_style_text_font(wf_pw_ta,&lv_font_montserrat_20,0);
    lv_obj_set_style_bg_color(wf_pw_ta,lv_color_hex(C_PANEL),0); lv_obj_set_style_text_color(wf_pw_ta,lv_color_hex(C_FG),0);
    lv_obj_set_style_border_color(wf_pw_ta,lv_color_hex(C_ACCENT),0); lv_obj_set_style_radius(wf_pw_ta,R_BTN,0);
    lv_obj_add_state(wf_pw_ta,LV_STATE_FOCUSED);
    lv_obj_t*row=set_row(l,LV_SYMBOL_EYE_OPEN,"Show password",NULL,0); lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t*sw=lv_switch_create(row); lv_obj_align(sw,LV_ALIGN_RIGHT_MID,-4,0);
    lv_obj_set_style_bg_color(sw,lv_color_hex(C_ACCENT),LV_PART_INDICATOR|LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw,wf_pw_show_cb,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_t*h=lv_label_create(l); lv_label_set_text(h,"Tap " LV_SYMBOL_OK " on the keyboard to connect.");
    lv_obj_set_style_text_color(h,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_left(h,6,0);
    lv_obj_t*kb=lv_keyboard_create(scr); lv_obj_set_size(kb,480,300); lv_obj_align(kb,LV_ALIGN_BOTTOM_MID,0,0);
    lv_keyboard_set_textarea(kb,wf_pw_ta);
    lv_obj_set_style_bg_color(kb,lv_color_hex(C_BAR),0);
    lv_obj_set_style_bg_color(kb,lv_color_hex(C_PANEL),LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb,lv_color_hex(C_FG),LV_PART_ITEMS);
    lv_obj_set_style_radius(kb,8,LV_PART_ITEMS); lv_obj_set_style_border_width(kb,0,LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb,lv_color_hex(C_ACCENT),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_text_font(kb,&lv_font_montserrat_20,LV_PART_ITEMS);
    lv_obj_add_event_cb(kb,wf_pw_kb_cb,LV_EVENT_ALL,NULL);
    g_scr=SCR_SETTINGS; load_screen(scr);
}
static void wf_net_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e); if(i<0||i>=wf_n) return;
    wifi_net_t*n=&wf_nets[i]; if(n->current) return;
    if(n->saved || !n->secured) wifi_connect(n->ssid,NULL); else show_wifi_password(n->ssid); }
static lv_obj_t* wf_switch(lv_obj_t*row,int on,lv_event_cb_t cb){
    lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t*sw=lv_switch_create(row); lv_obj_align(sw,LV_ALIGN_RIGHT_MID,-4,0);
    lv_obj_set_style_bg_color(sw,lv_color_hex(C_ACCENT),LV_PART_INDICATOR|LV_STATE_CHECKED);
    if(on) lv_obj_add_state(sw,LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw,cb,LV_EVENT_VALUE_CHANGED,NULL); return sw; }
static void wifi_fill(void){
    if(!wf_list) return;
    wf_ver=wifi_version();
    lv_coord_t sy=lv_obj_get_scroll_y(wf_list); lv_obj_clean(wf_list);
    char st[80], ip[20], sub[96]; wifi_status(st,sizeof st); fold(st); wifi_ip(ip,sizeof ip);
    lv_obj_t*sp=lv_obj_create(wf_list); lv_obj_remove_style_all(sp); lv_obj_set_size(sp,10,8);
    lv_obj_t*r=set_row(wf_list,LV_SYMBOL_WIFI,"Wi-Fi",st,0); wf_switch(r,wifi_enabled(),wf_onoff_cb);
    if(!wifi_enabled()){ lv_obj_t*n=lv_label_create(wf_list);
        lv_label_set_text(n,"Turn Wi-Fi on to share your music with\nVLC and other players on this network.\nWi-Fi uses more battery.");
        lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0); return; }

    set_section(wf_list,"MUSIC SERVER");
    if(!wifi_server()) snprintf(sub,sizeof sub,"let VLC on this network play your library");
    else if(!ip[0]) snprintf(sub,sizeof sub,"starts once Wi-Fi is connected");
    else if(!wifi_server_running()) snprintf(sub,sizeof sub,"starting...");
    else { int s=wifi_server_streams(); if(s) snprintf(sub,sizeof sub,"%d playing now - http://%s:%d",s,ip,WIFI_SRV_PORT);
           else snprintf(sub,sizeof sub,"on - http://%s:%d",ip,WIFI_SRV_PORT); }
    r=set_row(wf_list,LV_SYMBOL_UPLOAD,"Share music",sub,0); wf_switch(r,wifi_server(),wf_srv_cb);
    if(wifi_server() && ip[0]){ lv_obj_t*n=lv_label_create(wf_list);
        lv_label_set_text(n,"VLC: View > Playlist > Local Network >\nUniversal Plug'n'Play > HiBy R3.\nOr open the address in a browser.");
        lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,8,0); }

    set_section(wf_list,"NETWORKS");
    r=set_row(wf_list,LV_SYMBOL_REFRESH,wifi_busy()?"Working...":"Scan",NULL,0);
    lv_obj_add_event_cb(r,wf_scan_cb,LV_EVENT_CLICKED,NULL);
    wf_n=wifi_nets(wf_nets,WIFI_MAX);
    for(long i=0;i<wf_n;i++){ wifi_net_t*n=&wf_nets[i];
        const char*bars = !n->signal?"not in range":n->signal>-60?"strong":n->signal>-72?"good":"weak";
        if(n->current) snprintf(sub,sizeof sub,"connected%s%s",ip[0]?" - ":"",ip);
        else if(n->saved) snprintf(sub,sizeof sub,"saved - %s - hold to forget",bars);
        else snprintf(sub,sizeof sub,"%s - %s",n->secured?"password":"open",bars);
        r=set_row(wf_list,n->secured?LV_SYMBOL_WIFI:LV_SYMBOL_WARNING,n->ssid,sub,n->current);
        lv_obj_add_event_cb(r,wf_net_cb,LV_EVENT_SHORT_CLICKED,(void*)i);
        if(n->saved) lv_obj_add_event_cb(r,wf_forget_cb,LV_EVENT_LONG_PRESSED,(void*)i); }
    lv_obj_t*pad=lv_obj_create(wf_list); lv_obj_remove_style_all(pad); lv_obj_set_size(pad,10,40);
    lv_obj_update_layout(wf_list); lv_obj_scroll_to_y(wf_list,sy,LV_ANIM_OFF);
}
static void show_wifi(void){
    lv_obj_t*scr; wf_list=page_new("Wi-Fi",wf_back_cb,&scr);
    lv_obj_add_event_cb(scr,wf_del_cb,LV_EVENT_DELETE,NULL);
    wifi_fill(); if(wifi_enabled()) wifi_scan();
    g_scr=SCR_SETTINGS; load_screen(scr);
}
static void setup_go_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e);
    if(i==0) show_settings(); else if(i==1) show_wifi(); else if(i==2) show_display(); else if(i==3) show_sounds(); else show_datetime(); }
static void rescan_cb(lv_event_t*e){ (void)e; smartmix_rescan(); show_setup(); }   /* rebuild the smart index + fast-browse cache */
static void show_setup(void){
    lv_obj_t*scr,*l=page_new("Settings",setup_home_cb,&scr);
    static const char*ic[5]={LV_SYMBOL_BLUETOOTH,LV_SYMBOL_WIFI,LV_SYMBOL_IMAGE,LV_SYMBOL_AUDIO,LV_SYMBOL_BELL};
    static const char*nm[5]={"Bluetooth","Wi-Fi","Display","Sounds","Date & time"};
    char sub[5][64];
    snprintf(sub[0],64,"%s",bt_output()[0]?"output: speaker":"output: headphone jack");
    { char st[64]; wifi_status(st,sizeof st); fold(st);
      snprintf(sub[1],64,"%s%s",st,wifi_enabled()&&wifi_server()?", sharing music":""); }
    if(p_timeout) snprintf(sub[2],64,"brightness %d%%, off after %d%s",p_bright*100/101,
                           p_timeout>=60?p_timeout/60:p_timeout,p_timeout>=60?" min":" s");
    else snprintf(sub[2],64,"brightness %d%%, always on",p_bright*100/101);
    snprintf(sub[3],64,"filter %d%s",p_filter+1,p_nos?", NOS":"");
    { time_t t=time(0); struct tm tm; localtime_r(&t,&tm); strftime(sub[4],64,"%d/%m/%Y %H:%M",&tm); }
    lv_obj_t*pad=lv_obj_create(l); lv_obj_set_size(pad,10,8);
    lv_obj_set_style_bg_opa(pad,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(pad,0,0);
    for(long i=0;i<5;i++){ lv_obj_t*r=set_row(l,ic[i],nm[i],sub[i],0);
        lv_obj_t*ch=lv_label_create(r); lv_label_set_text(ch,LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(ch,lv_color_hex(C_DIM),0); lv_obj_align(ch,LV_ALIGN_RIGHT_MID,-6,0);
        lv_obj_add_event_cb(r,setup_go_cb,LV_EVENT_CLICKED,(void*)i); }
    /* Rescan library: rebuild the smart index + fast-browse cache (after adding/removing music) */
    { char rsub[48];
      if(smartmix_ready()) snprintf(rsub,sizeof rsub,"%d tracks indexed",smartmix_count());
      else snprintf(rsub,sizeof rsub,"indexing library...");
      lv_obj_t*r=set_row(l,LV_SYMBOL_REFRESH,"Rescan library",rsub,0);
      lv_obj_add_event_cb(r,rescan_cb,LV_EVENT_CLICKED,NULL); }
    g_scr=SCR_SETTINGS; load_screen(scr);
}

/* ---- Home ---- */
/* ================= Playlists & Favorites UI ================= */
static int cmp_row(const void*a,const void*b){ return strcasecmp((const char*)a,(const char*)b); }
static void show_playlist_tracks(const char*m3u,const char*disp);
static void show_new_playlist(void);

static void pl_home_cb(lv_event_t*e){ (void)e; show_home(); }
static void pl_file_cb(lv_event_t*e){ char*m3u=lv_event_get_user_data(e);
    char d[128]; snprintf(d,sizeof d,"%s",pl_base(m3u)); char*dot=strrchr(d,'.'); if(dot)*dot=0;
    show_playlist_tracks(m3u,d); }
static void pl_new_cb(lv_event_t*e){ (void)e; g_add_track[0]=0; show_new_playlist(); }

/* fill a list `l` with the .m3u files (not Favorites), each row -> cb(user_data=strdup path) */
static void pl_add_file_rows(lv_obj_t*l, lv_event_cb_t cb){
    DIR*dp=opendir(PL_DIR); if(!dp) return;
    static char names[256][128]; int nn=0; struct dirent*de;
    while((de=readdir(dp))&&nn<256){ const char*e2=strrchr(de->d_name,'.');
        if(!e2||strcasecmp(e2,".m3u")||!strcasecmp(de->d_name,"Favorites.m3u")) continue;
        snprintf(names[nn++],128,"%s",de->d_name); }
    closedir(dp); qsort(names,nn,128,cmp_row);
    for(int i=0;i<nn;i++){ char d[128]; snprintf(d,sizeof d,"%s",names[i]); char*dot=strrchr(d,'.'); if(dot)*dot=0;
        char full[1024]; snprintf(full,sizeof full,"%s/%s",PL_DIR,names[i]);
        char sub[24]; int c=m3u_count(full); snprintf(sub,sizeof sub,"%d song%s",c,c==1?"":"s");
        lv_obj_t*b=set_row(l,LV_SYMBOL_LIST,d,sub,0); char*fm=strdup(full);
        lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,fm);
        lv_obj_add_event_cb(b,free_cb,LV_EVENT_DELETE,fm); }
}
/* ---- smart mix playback ---- */
static void play_mix(char mix[][1024],int n){
    if(n<=0) return;
    pl_free();
    char**tmp=malloc(sizeof(char*)*n); for(int i=0;i<n;i++) tmp[i]=strdup(mix[i]);
    g_pl=tmp; g_pln=n; g_pli=0;
    pl_play(0); show_player(g_pltitle,g_pldir);
}
static void mix_foryou_cb(lv_event_t*e){ (void)e; static char mix[60][1024];
    int n=smartmix_for_you(mix,60); if(n>0) play_mix(mix,n); }
static void mix_similar_cb(lv_event_t*e){ (void)e; if(!(g_pln>0&&g_pli>=0)) return;
    char seed[1024]; snprintf(seed,sizeof seed,"%s",g_pl[g_pli]);
    static char mix[60][1024]; int n=smartmix_build(seed,mix,60); if(n>0) play_mix(mix,n); }

/* ---- peer (LAN) playlist sharing ---- */
static peer_t g_peer; static char g_peer_pl[128];
static char g_dl_local_m3u[1024];
static pthread_t g_dlth; static volatile int g_dl_run=0,g_dl_total=0,g_dl_done=0;
static void show_peer_playlists(void);
static void show_peer_tracks(const char*plname);
static void show_import_confirm(int missing);

static void urlenc(const char*s,char*o,int cap){ const char*hx="0123456789ABCDEF"; int j=0;
    for(;*s&&j<cap-4;s++){ unsigned char c=*s;
        if(c=='/'||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_') o[j++]=c;
        else { o[j++]='%'; o[j++]=hx[c>>4]; o[j++]=hx[c&15]; } }
    o[j]=0; }
static int m3u_missing(const char*m3u){ FILE*f=fopen(m3u,"r"); if(!f)return 0; int miss=0; char l[1024]; struct stat st;
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\r\n")]=0; if(!l[0]||l[0]=='#')continue;
        char full[1600]; snprintf(full,sizeof full,"%s/%s",SD_ROOT,l); if(stat(full,&st)!=0)miss++; } fclose(f); return miss; }
static void* dl_thread(void*a){ (void)a;
    FILE*f=fopen(g_dl_local_m3u,"r"); if(!f){ g_dl_run=0; return 0; } char l[1024]; struct stat st;
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\r\n")]=0; if(!l[0]||l[0]=='#')continue;
        char full[1600]; snprintf(full,sizeof full,"%s/%s",SD_ROOT,l); if(stat(full,&st)==0)continue;
        char enc[1400],path[1500]; urlenc(l,enc,sizeof enc); snprintf(path,sizeof path,"/f/%s",enc);
        if(share_download(g_peer.ip,g_peer.port,path,full)==0) g_dl_done++; }
    fclose(f); sync(); g_dl_run=0; return 0; }
static void dl_start_cb(lv_event_t*e){ (void)e; if(g_dl_run) return; g_dl_done=0; g_dl_run=1;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,160*1024);
    pthread_create(&g_dlth,&at,dl_thread,0); pthread_attr_destroy(&at); pthread_detach(g_dlth); }

static void peers_back_cb(lv_event_t*e){ (void)e; show_playlists(); }
static void peer_row_cb(lv_event_t*e){ peer_t*p=lv_event_get_user_data(e); g_peer=*p; show_peer_playlists(); }
static void peer_pl_row_cb(lv_event_t*e){ char*nm=lv_event_get_user_data(e); snprintf(g_peer_pl,sizeof g_peer_pl,"%s",nm); show_peer_tracks(nm); }
static void peer_tracks_back_cb(lv_event_t*e){ (void)e; show_peer_playlists(); }
static void import_done_cb(lv_event_t*e){ (void)e; show_playlists(); }

static void show_peer_playlists(void){
    lv_obj_t*scr,*l=page_new(g_peer.name[0]?g_peer.name:g_peer.ip,peers_back_cb,&scr);
    static char resp[8192]; int n=share_get(g_peer.ip,g_peer.port,"/playlists",resp,sizeof resp-1);
    if(n<=0){ lv_obj_t*m=lv_label_create(l); lv_label_set_text(m,"Could not reach this device.");
        lv_obj_set_style_text_color(m,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(m,12,0); }
    else { char*p=resp; int any=0;
        while(p&&*p){ char*nl=strchr(p,'\n'); if(nl)*nl=0; int cnt=0; char*tab=strchr(p,'\t');
            if(tab){ *tab=0; cnt=atoi(tab+1); }
            if(p[0]){ char sub[24]; snprintf(sub,sizeof sub,"%d song%s",cnt,cnt==1?"":"s");
                lv_obj_t*b=set_row(l,LV_SYMBOL_LIST,p,sub,0); char*d=strdup(p);
                lv_obj_add_event_cb(b,peer_pl_row_cb,LV_EVENT_CLICKED,d);
                lv_obj_add_event_cb(b,free_cb,LV_EVENT_DELETE,d); any=1; }
            p=nl?nl+1:0; }
        if(!any){ lv_obj_t*m=lv_label_create(l); lv_label_set_text(m,"No playlists shared here.");
            lv_obj_set_style_text_color(m,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(m,12,0); } }
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
static void peer_import_cb(lv_event_t*e){ (void)e;
    char enc[160],path[200]; urlenc(g_peer_pl,enc,sizeof enc); snprintf(path,sizeof path,"/pl/%s",enc);
    static char m3u[16384]; int n=share_get(g_peer.ip,g_peer.port,path,m3u,sizeof m3u-1);
    if(n<=0){ show_peer_tracks(g_peer_pl); return; }
    mkdir(PL_DIR,0777);
    char nm[160]; snprintf(nm,sizeof nm,"%s - %s",g_peer.name[0]?g_peer.name:"shared",g_peer_pl);
    for(char*p=nm;*p;p++) if(strchr("/:*?\"<>|\\",*p))*p='_';
    char local[1024]; snprintf(local,sizeof local,"%s/%s.m3u",PL_DIR,nm);
    FILE*f=fopen(local,"w"); if(f){ fwrite(m3u,1,n,f); fclose(f); sync(); }
    snprintf(g_dl_local_m3u,sizeof g_dl_local_m3u,"%s",local);
    int miss=m3u_missing(local);
    if(miss>0) show_import_confirm(miss); else show_playlists();
}
static void show_peer_tracks(const char*plname){
    lv_obj_t*scr,*l=page_new(plname,peer_tracks_back_cb,&scr);
    char enc[160],path[200]; urlenc(plname,enc,sizeof enc); snprintf(path,sizeof path,"/pl/%s",enc);
    static char m3u[16384]; int n=share_get(g_peer.ip,g_peer.port,path,m3u,sizeof m3u-1);
    int have=0,missing=0;
    if(n>0){ char*p=m3u; char t2[256]="";
        while(p&&*p){ char*nl=strchr(p,'\n'); if(nl)*nl=0;
            if(!strncmp(p,"#EXTINF",7)){ char*c=strchr(p,','); if(c) snprintf(t2,sizeof t2,"%s",c+1); }
            else if(p[0]&&p[0]!='#'){ char full[1600]; snprintf(full,sizeof full,"%s/%s",SD_ROOT,p);
                struct stat st; int h=(stat(full,&st)==0); if(h)have++; else missing++;
                char d2[256]; if(t2[0]) snprintf(d2,sizeof d2,"%s",t2); else pretty(pl_base(p),d2,sizeof d2); t2[0]=0;
                lv_obj_t*b=set_row(l,h?LV_SYMBOL_OK:LV_SYMBOL_DOWNLOAD,d2,h?NULL:"not on this device",0);
                lv_obj_clear_flag(b,LV_OBJ_FLAG_CLICKABLE); }
            p=nl?nl+1:0; } }
    char sub[64]; snprintf(sub,sizeof sub,"%d here, %d to fetch",have,missing);
    lv_obj_t*ib=set_row(l,LV_SYMBOL_SAVE,"Import to my playlists",sub,1);
    lv_obj_add_event_cb(ib,peer_import_cb,LV_EVENT_CLICKED,NULL);
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
static void show_import_confirm(int missing){
    g_dl_total=missing; g_dl_done=0;
    lv_obj_t*scr,*l=page_new("Import",pl_home_cb,&scr);
    char msg[96]; snprintf(msg,sizeof msg,"Playlist saved. %d song%s not on this device.",missing,missing==1?"":"s");
    lv_obj_t*m=lv_label_create(l); lv_label_set_text(m,msg);
    lv_obj_set_style_text_color(m,lv_color_hex(C_FG),0); lv_obj_set_style_pad_all(m,12,0);
    lv_obj_t*b=set_row(l,LV_SYMBOL_DOWNLOAD,"Download them from this device",NULL,1);
    lv_obj_add_event_cb(b,dl_start_cb,LV_EVENT_CLICKED,NULL);
    g_dl_lbl=lv_label_create(l); lv_label_set_text(g_dl_lbl,"");
    lv_obj_set_style_text_color(g_dl_lbl,lv_color_hex(C_ACCENT),0); lv_obj_set_style_pad_all(g_dl_lbl,12,0);
    lv_obj_t*d=set_row(l,LV_SYMBOL_OK,"Done",NULL,0);
    lv_obj_add_event_cb(d,import_done_cb,LV_EVENT_CLICKED,NULL);
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}

static void show_playlists(void){
    lv_obj_t*scr,*l=page_new("Playlists",pl_home_cb,&scr);
    /* smart suggestions */
    { lv_obj_t*fy=set_row(l,LV_SYMBOL_AUDIO,"For You",
        smartmix_ready()?"from your favorites & plays":"indexing your library...",1);
      lv_obj_add_event_cb(fy,mix_foryou_cb,LV_EVENT_CLICKED,NULL); }
    if(g_pln>0){ lv_obj_t*sm=set_row(l,LV_SYMBOL_SHUFFLE,"Similar to current song",NULL,0);
      lv_obj_add_event_cb(sm,mix_similar_cb,LV_EVENT_CLICKED,NULL); }
    int fc=m3u_count(FAV_M3U); char fsub[24]; snprintf(fsub,sizeof fsub,"%d song%s",fc,fc==1?"":"s");
    lv_obj_t*fb=set_row(l,LV_SYMBOL_OK,"Favorites",fsub,1);
    char*fm=strdup(FAV_M3U); lv_obj_add_event_cb(fb,pl_file_cb,LV_EVENT_CLICKED,fm);
    lv_obj_add_event_cb(fb,free_cb,LV_EVENT_DELETE,fm);
    pl_add_file_rows(l,pl_file_cb);
    lv_obj_t*nb=set_row(l,LV_SYMBOL_PLUS,"New playlist",NULL,0);
    lv_obj_add_event_cb(nb,pl_new_cb,LV_EVENT_CLICKED,NULL);
    /* nearby devices sharing playlists (discovered on the LAN) */
    peer_t pr[8]; int npr=share_peers(pr,8); char myip[20]=""; wifi_ip(myip,sizeof myip); int shown=0;
    for(int i=0;i<npr;i++){ if(myip[0]&&!strcmp(pr[i].ip,myip)) continue;   /* skip ourselves */
        if(!shown){ lv_obj_t*h=lv_label_create(l); lv_label_set_text(h,"Nearby devices");
            lv_obj_set_style_text_color(h,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(h,12,0); shown=1; }
        lv_obj_t*b=set_row(l,LV_SYMBOL_WIFI,pr[i].name[0]?pr[i].name:pr[i].ip,pr[i].ip,0);
        peer_t*pp=malloc(sizeof(peer_t)); *pp=pr[i];
        lv_obj_add_event_cb(b,peer_row_cb,LV_EVENT_CLICKED,pp);
        lv_obj_add_event_cb(b,free_cb,LV_EVENT_DELETE,pp); }
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
static void pl_tracks_back_cb(lv_event_t*e){ (void)e; show_playlists(); }
static void pl_track_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e);
    pl_load_m3u(g_cur_m3u);
    if(i>=0 && i<g_pln){ pl_play((int)i); show_player(g_pltitle,g_pldir); } }
static int  is_fav_m3u(const char*m3u){ return !strcmp(m3u,FAV_M3U); }
static void pl_disp(const char*m3u,char*out,int cap){ snprintf(out,cap,"%s",pl_base(m3u));
    char*d=strrchr(out,'.'); if(d&&!strcasecmp(d,".m3u"))*d=0; }
static void pl_track_hold_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e);
    lv_indev_wait_release(lv_indev_get_act());   /* don't let the finger-release click the new screen */
    if(!m3u_nth(g_cur_m3u,(int)i,g_act_full,sizeof g_act_full,g_act_title,sizeof g_act_title)) return;
    g_act_idx=(int)i; snprintf(g_act_src,sizeof g_act_src,"%s",g_cur_m3u); show_track_actions(); }
static void show_playlist_tracks(const char*m3u,const char*disp){
    snprintf(g_cur_m3u,sizeof g_cur_m3u,"%s",m3u);
    lv_obj_t*scr,*l=page_new(disp,pl_tracks_back_cb,&scr);
    FILE*f=fopen(m3u,"r"); long idx=0;
    if(f){ char line[1024],title[256]="";
        while(fgets(line,sizeof line,f)){ line[strcspn(line,"\r\n")]=0;
            if(!line[0]) continue;
            if(!strncmp(line,"#EXTINF",7)){ char*c=strchr(line,','); if(c) snprintf(title,sizeof title,"%s",c+1); continue; }
            if(line[0]=='#') continue;
            char full[1600]; if(line[0]=='/') snprintf(full,sizeof full,"%s",line);
            else snprintf(full,sizeof full,"%s/%s",SD_ROOT,line);
            struct stat st; if(stat(full,&st)!=0){ title[0]=0; continue; }   /* skip missing (Phase 2: offer to fetch) */
            char d2[256]; if(title[0]) snprintf(d2,sizeof d2,"%s",title); else pretty(pl_base(full),d2,sizeof d2);
            title[0]=0;
            lv_obj_t*b=set_row(l,LV_SYMBOL_AUDIO,d2,"hold to move / remove",0);
            lv_obj_add_event_cb(b,pl_track_cb,LV_EVENT_CLICKED,(void*)idx);
            lv_obj_add_event_cb(b,pl_track_hold_cb,LV_EVENT_LONG_PRESSED,(void*)idx); idx++; }
        fclose(f); }
    if(idx==0){ lv_obj_t*n=lv_label_create(l); lv_label_set_text(n,"(empty - add songs from Now Playing)");
        lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0); }
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
/* --- track action sheet: long-press a track in a playlist/Favorites view --- */
static void act_reshow(void){ char d[128]; pl_disp(g_act_src,d,sizeof d); show_playlist_tracks(g_act_src,d); }
static void act_back_cb(lv_event_t*e){ (void)e; act_reshow(); }
static void act_play_cb(lv_event_t*e){ (void)e; pl_load_m3u(g_act_src);
    int pi=-1; for(int k=0;k<g_pln;k++) if(!strcmp(g_pl[k],g_act_full)){ pi=k; break; }
    if(pi<0) pi=(g_act_idx<g_pln)?g_act_idx:0;
    if(pi>=0&&pi<g_pln){ pl_play(pi); show_player(g_pltitle,g_pldir); } }
static void act_remove_cb(lv_event_t*e){ (void)e; m3u_remove(g_act_src,g_act_full); act_reshow(); }
static void act_move_cb(lv_event_t*e){ (void)e; g_act_move=1; show_track_pick(); }
static void act_add_cb (lv_event_t*e){ (void)e; g_act_move=0; show_track_pick(); }
static void show_track_actions(void){
    lv_obj_t*scr,*l=page_new("Song",act_back_cb,&scr);
    lv_obj_t*t=lv_label_create(l); lv_label_set_text(t,g_act_title[0]?g_act_title:pl_base(g_act_full));
    lv_obj_set_style_text_color(t,lv_color_hex(C_FG),0); lv_obj_set_style_pad_all(t,12,0);
    lv_label_set_long_mode(t,LV_LABEL_LONG_DOT); lv_obj_set_width(t,440);
    lv_obj_t*pb=set_row(l,LV_SYMBOL_PLAY,"Play",NULL,0);          lv_obj_add_event_cb(pb,act_play_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*mb=set_row(l,LV_SYMBOL_SHUFFLE,"Move to...",NULL,0); lv_obj_add_event_cb(mb,act_move_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*ab=set_row(l,LV_SYMBOL_PLUS,"Add to...",NULL,0);     lv_obj_add_event_cb(ab,act_add_cb,LV_EVENT_CLICKED,NULL);
    char rm[80]; { char d[64]; pl_disp(g_act_src,d,sizeof d); snprintf(rm,sizeof rm,"Remove from %s",d); }
    lv_obj_t*rb=set_row(l,LV_SYMBOL_TRASH,rm,NULL,0);            lv_obj_add_event_cb(rb,act_remove_cb,LV_EVENT_CLICKED,NULL);
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
/* pick a destination playlist for Move/Add (excludes Favorites-if-source and the source list itself) */
static void pick_back_cb(lv_event_t*e){ (void)e; show_track_actions(); }
static void act_apply(const char*target){
    if(g_act_full[0]) m3u_append(target,g_act_full,g_act_title);
    if(g_act_move && g_act_src[0] && strcmp(g_act_src,target)) m3u_remove(g_act_src,g_act_full);
    sync(); act_reshow();
}
static void pick_fav_cb(lv_event_t*e){ (void)e; act_apply(FAV_M3U); }
static void pick_file_cb(lv_event_t*e){ char*m3u=lv_event_get_user_data(e); act_apply(m3u); }
static void show_track_pick(void){
    char ttl[48]; snprintf(ttl,sizeof ttl,"%s to...",g_act_move?"Move":"Add");
    lv_obj_t*scr,*l=page_new(ttl,pick_back_cb,&scr);
    if(!is_fav_m3u(g_act_src)){ lv_obj_t*fb=set_row(l,LV_SYMBOL_OK,"Favorites",NULL,0);
        lv_obj_add_event_cb(fb,pick_fav_cb,LV_EVENT_CLICKED,NULL); }
    DIR*dp=opendir(PL_DIR);
    if(dp){ static char names[256][128]; int nn=0; struct dirent*de;
        while((de=readdir(dp))&&nn<256){ const char*e2=strrchr(de->d_name,'.');
            if(!e2||strcasecmp(e2,".m3u")||!strcasecmp(de->d_name,"Favorites.m3u")) continue;
            char full[1024]; snprintf(full,sizeof full,"%s/%s",PL_DIR,de->d_name);
            if(!strcmp(full,g_act_src)) continue;                 /* can't move to itself */
            snprintf(names[nn++],128,"%s",de->d_name); }
        closedir(dp); qsort(names,nn,128,cmp_row);
        for(int i=0;i<nn;i++){ char d[128]; snprintf(d,sizeof d,"%s",names[i]); char*dot=strrchr(d,'.'); if(dot)*dot=0;
            char full[1024]; snprintf(full,sizeof full,"%s/%s",PL_DIR,names[i]);
            char sub[24]; int c=m3u_count(full); snprintf(sub,sizeof sub,"%d song%s",c,c==1?"":"s");
            lv_obj_t*b=set_row(l,LV_SYMBOL_LIST,d,sub,0); char*fm=strdup(full);
            lv_obj_add_event_cb(b,pick_file_cb,LV_EVENT_CLICKED,fm);
            lv_obj_add_event_cb(b,free_cb,LV_EVENT_DELETE,fm); } }
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
/* add-to-playlist picker (current track) */
static void addpl_back_cb(lv_event_t*e){ (void)e; show_player(g_pltitle,g_pldir); }
static void addpl_fav_cb(lv_event_t*e){ (void)e; if(g_add_track[0]) m3u_append(FAV_M3U,g_add_track,g_add_title); show_player(g_pltitle,g_pldir); }
static void addpl_pick_cb(lv_event_t*e){ char*m3u=lv_event_get_user_data(e);
    if(g_add_track[0]) m3u_append(m3u,g_add_track,g_add_title); show_player(g_pltitle,g_pldir); }
static void addpl_new_cb(lv_event_t*e){ (void)e; show_new_playlist(); }   /* keeps g_add_track */
static void show_add_to_playlist(void){
    if(!(g_pln>0&&g_pli>=0)){ show_player(g_pltitle,g_pldir); return; }
    snprintf(g_add_track,sizeof g_add_track,"%s",g_pl[g_pli]);
    snprintf(g_add_title,sizeof g_add_title,"%s",g_pltitle);
    lv_obj_t*scr,*l=page_new("Add to playlist",addpl_back_cb,&scr);
    lv_obj_t*fb=set_row(l,LV_SYMBOL_OK,"Favorites",NULL,1);
    lv_obj_add_event_cb(fb,addpl_fav_cb,LV_EVENT_CLICKED,NULL);
    pl_add_file_rows(l,addpl_pick_cb);
    lv_obj_t*nb=set_row(l,LV_SYMBOL_PLUS,"New playlist...",NULL,0);
    lv_obj_add_event_cb(nb,addpl_new_cb,LV_EVENT_CLICKED,NULL);
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}
/* new playlist (on-screen keyboard) */
static void npl_commit(void){
    if(!npl_ta) return;
    char name[64]; snprintf(name,sizeof name,"%s",lv_textarea_get_text(npl_ta)); npl_ta=NULL;
    while(name[0]==' ') memmove(name,name+1,strlen(name));
    size_t ln=strlen(name); while(ln&&name[ln-1]==' ')name[--ln]=0;
    for(char*p=name;*p;p++) if(strchr("/:*?\"<>|\\",*p)) *p='_';
    if(!name[0]){ show_playlists(); return; }
    mkdir(PL_DIR,0777);
    char path[1024]; snprintf(path,sizeof path,"%s/%s.m3u",PL_DIR,name);
    if(access(path,F_OK)!=0){ FILE*f=fopen(path,"w"); if(f){ fprintf(f,"#EXTM3U\n"); fclose(f); } }
    if(g_add_track[0]){ m3u_append(path,g_add_track,g_add_title); g_add_track[0]=0; }
    sync(); show_playlists();
}
static void npl_kb_cb(lv_event_t*e){ int c=lv_event_get_code(e);
    if(c==LV_EVENT_READY) npl_commit();
    else if(c==LV_EVENT_CANCEL){ npl_ta=NULL; show_playlists(); } }
static void show_new_playlist(void){
    lv_obj_t*scr,*l=page_new("New playlist",pl_home_cb,&scr);
    lv_obj_set_height(l,720-54-300);
    npl_ta=lv_textarea_create(l); lv_obj_set_width(npl_ta,LV_PCT(100)); lv_textarea_set_one_line(npl_ta,true);
    lv_textarea_set_max_length(npl_ta,48); lv_textarea_set_placeholder_text(npl_ta,"Playlist name");
    lv_obj_set_style_text_font(npl_ta,&lv_font_montserrat_20,0);
    lv_obj_set_style_bg_color(npl_ta,lv_color_hex(C_PANEL),0); lv_obj_set_style_text_color(npl_ta,lv_color_hex(C_FG),0);
    lv_obj_set_style_border_color(npl_ta,lv_color_hex(C_ACCENT),0); lv_obj_set_style_radius(npl_ta,R_BTN,0);
    lv_obj_add_state(npl_ta,LV_STATE_FOCUSED);
    lv_obj_t*h=lv_label_create(l); lv_label_set_text(h,"Tap " LV_SYMBOL_OK " on the keyboard to create.");
    lv_obj_set_style_text_color(h,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_left(h,6,0);
    lv_obj_t*kb=lv_keyboard_create(scr); lv_obj_set_size(kb,480,300); lv_obj_align(kb,LV_ALIGN_BOTTOM_MID,0,0);
    lv_keyboard_set_textarea(kb,npl_ta);
    lv_obj_set_style_bg_color(kb,lv_color_hex(C_BAR),0);
    lv_obj_set_style_bg_color(kb,lv_color_hex(C_PANEL),LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb,lv_color_hex(C_FG),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb,lv_color_hex(C_ACCENT),LV_PART_ITEMS|LV_STATE_CHECKED);
    lv_obj_set_style_text_font(kb,&lv_font_montserrat_20,LV_PART_ITEMS);
    lv_obj_add_event_cb(kb,npl_kb_cb,LV_EVENT_ALL,NULL);
    g_scr=SCR_PLAYLISTS; load_screen(scr);
}

static void tile_cb(lv_event_t*e){ const char*name=lv_event_get_user_data(e);
    if(!strcmp(name,"Music")){ strcpy(g_root,MUSIC_ROOT); strcpy(g_root_label,"Music"); show_browser(MUSIC_ROOT); }
    else if(!strcmp(name,"Podcasts")){ strcpy(g_root,PODCAST_ROOT); strcpy(g_root_label,"Podcasts"); show_browser(PODCAST_ROOT); }
    else if(!strcmp(name,"Playlists")) show_playlists();
    else if(!strcmp(name,"Settings")) show_setup();
    else if(!strcmp(name,"Games")) show_games();
    else DBG("tile (todo): %s\n",name); }
static void make_row(lv_obj_t*c,const char*txt,const char*icon,uint32_t accent){
    lv_obj_t*b=lv_btn_create(c);
    lv_obj_set_width(b,LV_PCT(100)); lv_obj_set_flex_grow(b,1);   /* rows share the height evenly */
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(b,R_BTN,0);
    lv_obj_set_style_border_color(b,lv_color_hex(accent),0); lv_obj_set_style_border_width(b,3,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(accent),LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,tile_cb,LV_EVENT_CLICKED,(void*)txt);
    lv_obj_t*ic=lv_label_create(b); lv_label_set_text(ic,icon);
    lv_obj_set_style_text_color(ic,lv_color_hex(accent),0);
    lv_obj_set_style_text_color(ic,lv_color_hex(C_BG),LV_STATE_PRESSED);
    lv_obj_set_style_text_font(ic,&lv_font_montserrat_28,0); lv_obj_align(ic,LV_ALIGN_LEFT_MID,20,0);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,txt);
    lv_obj_set_style_text_color(l,lv_color_hex(accent),0);
    lv_obj_set_style_text_color(l,lv_color_hex(C_BG),LV_STATE_PRESSED);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0); lv_obj_align(l,LV_ALIGN_LEFT_MID,70,0);
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
    pet_bell=lv_label_create(bar); lv_label_set_text(pet_bell,LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(pet_bell,lv_color_hex(C_ACCENT),0); lv_obj_align(pet_bell,LV_ALIGN_LEFT_MID,60,0);
    if(!pet_needs_you()) lv_obj_add_flag(pet_bell,LV_OBJ_FLAG_HIDDEN);
    btbat_lbl=lv_label_create(bar); lv_label_set_text(btbat_lbl,"");
    lv_obj_set_style_text_color(btbat_lbl,lv_color_hex(C_DIM),0); lv_obj_align(btbat_lbl,LV_ALIGN_RIGHT_MID,-170,0);
    batt_lbl=lv_label_create(bar); lv_label_set_text(batt_lbl,"");
    lv_obj_set_style_text_color(batt_lbl,lv_color_hex(C_DIM),0); lv_obj_align(batt_lbl,LV_ALIGN_RIGHT_MID,-80,0);
    lv_obj_t*g=lv_obj_create(scr); lv_obj_set_style_bg_opa(g,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(g,0,0); lv_obj_set_style_pad_all(g,0,0);
    lv_obj_set_size(g,448,634); lv_obj_align(g,LV_ALIGN_TOP_MID,0,70);
    lv_obj_set_flex_flow(g,LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(g,14,0);
    lv_obj_clear_flag(g,LV_OBJ_FLAG_SCROLLABLE);
    make_row(g,"Music",   LV_SYMBOL_AUDIO,   C_ACCENT);
    make_row(g,"Podcasts",LV_SYMBOL_VIDEO,   C_ACCENT2);
    make_row(g,"Playlists",LV_SYMBOL_LIST,   C_ACCENT);
    make_row(g,"Games",   LV_SYMBOL_PLAY,    C_ACCENT2);
    make_row(g,"Settings",LV_SYMBOL_SETTINGS,C_ACCENT);
    g_scr=SCR_HOME;
    load_screen(scr);
}

/* ---- physical keys ---- */
static int kf_pwr=-1, kf_media=-1;      /* event0 (power/prev), event2 (vol/play/next) */
static uint32_t pwr_down=0, pwr_tap_t=0; static int pwr_held=0, pwr_tap=0;
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
    lv_obj_clear_flag(g_volov,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);   /* taps fall through to the header */
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
static void do_poweroff(void){ game_save(); brick_save(); coil_save(); save_resume(); smartmix_flush(); audio_stop(); sync(); system("poweroff"); }
static void keys_poll(void){
    struct input_event e;
    if(kf_pwr>=0) while(read(kf_pwr,&e,sizeof e)==sizeof e){
        if(e.type!=EV_KEY) continue;
        g_last_input=r3_tick_ms();
        if(e.code==116){ /* power */
            if(e.value==1){ pwr_down=r3_tick_ms(); pwr_held=1; }
            else if(e.value==0 && pwr_held){ pwr_held=0;
                if(r3_tick_ms()-pwr_down < 1200){
                    if(g_scr_off){ screen_wake(); pwr_tap=0; }           /* first press only wakes */
                    else if(pwr_tap){ pwr_tap=0; show_home(); }         /* double press -> Home */
                    else { pwr_tap=1; pwr_tap_t=r3_tick_ms(); } } }
        } else if(e.code==165 && e.value==1){ if(!(g_scr==SCR_GAME && (game_key(165)||brick_key(165)||coil_key(165)))) pl_prev(); }  /* previous */
    }
    if(kf_media>=0) while(read(kf_media,&e,sizeof e)==sizeof e){
        if(e.type!=EV_KEY) continue;
        g_last_input=r3_tick_ms();
        if(e.code==115 && e.value){ bump_vol(+12); }        /* vol up (incl. autorepeat) */
        else if(e.code==114 && e.value){ bump_vol(-12); }   /* vol down */
        else if(g_scr==SCR_GAME && e.value==1 && (e.code==163||e.code==164) && (game_key(e.code)||brick_key(e.code)||coil_key(e.code))){}   /* game controls */
        else if(e.code==163 && e.value==1){ pl_next(); }    /* next */
        else if(e.code==164 && e.value==1){                 /* play/pause */
            if(!audio_is_playing() && g_pln>0) pl_resume();
            else { audio_toggle_pause(); save_resume();
                   if(np_play) lv_label_set_text(np_play,audio_is_paused()?LV_SYMBOL_PLAY:LV_SYMBOL_PAUSE); }
        }
    }
    /* single press resolves once the double-press window (350 ms) has passed */
    if(pwr_tap && !pwr_held && r3_tick_ms()-pwr_tap_t>=350){ pwr_tap=0; power_short(); }
    /* long-press power -> off (fires while still held) */
    if(pwr_held && r3_tick_ms()-pwr_down>=1200){ pwr_held=0; do_poweroff(); }
}


/* ---- Games menu: Blockfall + Doom games found on the SD card ----
 * Doom runs as its own process (/usr/data/r3doom, doomgeneric) on the player's own WADs in
 * /usr/data/mnt/sd_0/Games/Doom. We hand it the screen and input, wait for it to exit
 * (music keeps playing: the audio thread isn't blocked), then drop queued input + redraw. */
#define DOOM_BIN "/usr/data/r3doom"
#define DOOM_DIR "/usr/data/mnt/sd_0/Games/Doom"
static const struct { const char*name,*iwad,*pwad,*sub; } DOOMS[]={
    {"Doom",                    "doom.wad",    0,                 "The Ultimate Doom"},
    {"Sigil",                   "doom.wad",    "sigil.wad",       "Doom + Sigil episode"},
    {"Doom II",                 "doom2.wad",   0,                 "Hell on Earth"},
    {"No Rest for the Living",  "doom2.wad",   "nerve.wad",       "Doom II expansion"},
    {"Master Levels",           "doom2.wad",   "masterlevels.wad","Doom II"},
    {"TNT: Evilution",          "tnt.wad",     0,                 "Final Doom"},
    {"The Plutonia Experiment", "plutonia.wad",0,                 "Final Doom"},
};
#define NDOOM (int)(sizeof DOOMS/sizeof DOOMS[0])
static int file_ok(const char*dir,const char*f){ char p[512]; struct stat st;
    snprintf(p,sizeof p,"%s/%s",dir,f); return stat(p,&st)==0 && st.st_size>0; }
static void games_back_cb(lv_event_t*e){ (void)e; show_home(); }
static void show_games(void);
static void show_gb_list(void);
static void drain_input(void){ struct input_event e;
    while(read(tsfd,&e,sizeof e)==sizeof e){} t_down=0;
    if(kf_pwr>=0) while(read(kf_pwr,&e,sizeof e)==sizeof e){}
    if(kf_media>=0) while(read(kf_media,&e,sizeof e)==sizeof e){}
    pwr_held=0; pwr_tap=0; }
static void run_doom(int i){
    char iwad[512],pwad[512]; snprintf(iwad,sizeof iwad,"%s/%s",DOOM_DIR,DOOMS[i].iwad);
    if(DOOMS[i].pwad) snprintf(pwad,sizeof pwad,"%s/%s",DOOM_DIR,DOOMS[i].pwad);
    char*argv[10]; int n=0;
    argv[n++]="r3doom"; argv[n++]="-iwad"; argv[n++]=iwad;
    if(DOOMS[i].pwad){ argv[n++]="-file"; argv[n++]=pwad; }
    argv[n++]="-mb"; argv[n++]="12"; argv[n]=0;
    mkdir("/usr/data/doom",0755);
    if(audio_is_playing()){ g_resume_ms=audio_pos_ms(); save_resume(); audio_stop();   /* Doom has sound now: free the DAC */
        if(np_play) lv_label_set_text(np_play,LV_SYMBOL_PLAY); }
    char*envp[]={"HOME=/usr/data/doom","PATH=/usr/bin:/bin",0};
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa,1,"/usr/data/doom/r3doom.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    posix_spawn_file_actions_adddup2(&fa,1,2);
    pid_t pid; int rc;
    if(chdir("/usr/data/doom")!=0){}
    rc=posix_spawn(&pid,DOOM_BIN,&fa,0,argv,envp);
    posix_spawn_file_actions_destroy(&fa);
    if(chdir("/")!=0){}
    if(rc!=0){ DBG("doom spawn failed %d\n",rc); return; }
    DBG("doom started pid %d (%s)\n",(int)pid,DOOMS[i].name);
    waitpid(pid,0,0);                 /* SIGCHLD=SIG_IGN: returns once it has exited */
    { FILE*vf=fopen(VOL_FILE,"r"); int v; if(vf){ if(fscanf(vf,"%d",&v)==1) audio_set_volume(v); fclose(vf); } }   /* volume set in the game */
    drain_input(); bl_set(p_bright); g_last_input=r3_tick_ms();
    show_games();                     /* fresh screen => full redraw over Doom's pixels */
}
/* ---- Game Boy: r3gb (Peanut-GB) on the player's own ROMs ---- */
#define GB_BIN "/usr/data/r3gb"
static const char*GB_DIRS[]={"/usr/data/mnt/sd_0/Games/GameBoy","/usr/data/Games/GameBoy"};
static char gb_roms[64][320]; static int gb_nroms;
static void gb_scan(void){
    gb_nroms=0;
    for(int d=0;d<2;d++){ DIR*dp=opendir(GB_DIRS[d]); if(!dp) continue; struct dirent*e;
        while((e=readdir(dp)) && gb_nroms<64){ const char*x=strrchr(e->d_name,'.');
            if(e->d_name[0]=='.' || !x || strcasecmp(x,".gb")) continue;
            /* same game in both places: one entry — the copy with a save, else the SD one */
            int dup=-1; for(int k=0;k<gb_nroms;k++){ const char*b=strrchr(gb_roms[k],'/'); if(b&&!strcmp(b+1,e->d_name)) dup=k; }
            char path[320]; snprintf(path,sizeof path,"%s/%s",GB_DIRS[d],e->d_name);
            if(dup>=0){ char sav[330]; snprintf(sav,sizeof sav,"%s",path); char*dt=strrchr(sav,'.'); if(dt) strcpy(dt,".sav");
                struct stat st; if(stat(sav,&st)==0) snprintf(gb_roms[dup],320,"%s",path);
                continue; }
            snprintf(gb_roms[gb_nroms++],320,"%s",path); }
        closedir(dp); }
    for(int i=0;i<gb_nroms;i++) for(int j=i+1;j<gb_nroms;j++){ const char*a=strrchr(gb_roms[i],'/'),*b=strrchr(gb_roms[j],'/');
        if(strcasecmp(a,b)>0){ char t[320]; memcpy(t,gb_roms[i],320); memcpy(gb_roms[i],gb_roms[j],320); memcpy(gb_roms[j],t,320); } }
}
/* "Legend of Zelda, The - Link's Awakening (U) (V1.2) [!].gb" -> "Legend of Zelda, The - Link's Awakening" */
static void gb_title(const char*path,char*out,int cap){
    const char*b=strrchr(path,'/'); snprintf(out,cap,"%s",b?b+1:path);
    char*cut=out; for(char*p=out;*p;p++){ if(*p=='('||*p=='['||*p=='.'){ cut=p; break; } cut=p+1; }
    *cut=0; while(cut>out && cut[-1]==' ') *--cut=0; fold(out); }
static void run_gb(int i){
    if(i<0||i>=gb_nroms) return;
    if(audio_is_playing()){ g_resume_ms=audio_pos_ms(); save_resume(); audio_stop();   /* free the DAC */
        if(np_play) lv_label_set_text(np_play,LV_SYMBOL_PLAY); }
    char*argv[]={"r3gb",gb_roms[i],0}; char*envp[]={"PATH=/usr/bin:/bin",0};
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa,1,"/tmp/r3gb.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    posix_spawn_file_actions_adddup2(&fa,1,2);
    pid_t pid; int rc=posix_spawn(&pid,GB_BIN,&fa,0,argv,envp);
    posix_spawn_file_actions_destroy(&fa);
    if(rc!=0){ DBG("r3gb spawn failed %d\n",rc); return; }
    waitpid(pid,0,0);
    { FILE*vf=fopen(VOL_FILE,"r"); int v; if(vf){ if(fscanf(vf,"%d",&v)==1) audio_set_volume(v); fclose(vf); } }   /* volume set in the game */
    drain_input(); bl_set(p_bright); g_last_input=r3_tick_ms();
    show_gb_list();
}
static void gb_go_cb(lv_event_t*e){ run_gb((int)(long)lv_event_get_user_data(e)); }
static void gb_back_cb(lv_event_t*e){ (void)e; show_games(); }
static void show_gb_list(void){
    lv_obj_t*scr,*l=page_new("Game Boy",gb_back_cb,&scr); gb_scan();
    lv_obj_t*pad=lv_obj_create(l); lv_obj_set_size(pad,10,8);
    lv_obj_set_style_bg_opa(pad,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(pad,0,0);
    for(long i=0;i<gb_nroms;i++){ char t[160],sav[340],sub[40]; gb_title(gb_roms[i],t,sizeof t);
        snprintf(sav,sizeof sav,"%s",gb_roms[i]); char*d=strrchr(sav,'.'); if(d) strcpy(d,".sav");
        struct stat st; snprintf(sub,sizeof sub,"%s",stat(sav,&st)==0?"saved game":"");
        lv_obj_t*r=set_row(l,LV_SYMBOL_PLAY,t,sub[0]?sub:NULL,0);
        lv_obj_add_event_cb(r,gb_go_cb,LV_EVENT_CLICKED,(void*)i); }
    lv_obj_t*n=lv_label_create(l);
    lv_label_set_text(n,gb_nroms?"Hold power to save and quit a game.\nPower: pause. Controllers work too."
                               :"Copy your .gb ROMs to Games/GameBoy\non the SD card.");
    lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0);
    g_scr=SCR_SETTINGS; load_screen(scr);
}
/* built-in games: -1 Blockfall, -2 Brickstorm, -5 Coil. A saved unfinished game offers Resume / New. */
static void start_builtin(long id,int resume){
    null_live();
    lv_obj_t*gs = id==-1 ? game_create(show_games,resume) : id==-5 ? coil_create(show_games,resume) : brick_create(show_games,resume);
    g_scr=SCR_GAME; load_screen(gs); }
static lv_obj_t*g_choice; static long g_choice_id;
static void choice_close(void){ if(g_choice){ lv_obj_del_async(g_choice); g_choice=NULL; } }
static void choice_cb(lv_event_t*e){ long act=(long)lv_event_get_user_data(e); long id=g_choice_id;
    choice_close(); if(act>=0) start_builtin(id,act==1); }
static lv_obj_t* choice_btn(lv_obj_t*p,const char*txt,int primary,long act){
    lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,LV_PCT(100),64);
    lv_obj_set_style_radius(b,R_BTN,0); lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(primary?C_ACCENT:C_PANEL2),0);
    lv_obj_add_event_cb(b,choice_cb,LV_EVENT_CLICKED,(void*)act);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,txt); lv_obj_center(l);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0);
    lv_obj_set_style_text_color(l,lv_color_hex(primary?C_BG:C_FG),0); return b; }
static void ask_resume(long id,const char*name,int score){
    g_choice_id=id;
    g_choice=lv_obj_create(lv_scr_act()); lv_obj_set_size(g_choice,480,720); lv_obj_set_pos(g_choice,0,0);
    lv_obj_set_style_bg_color(g_choice,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(g_choice,LV_OPA_70,0);
    lv_obj_set_style_border_width(g_choice,0,0); lv_obj_set_style_radius(g_choice,0,0);
    lv_obj_clear_flag(g_choice,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_choice,choice_cb,LV_EVENT_CLICKED,(void*)-1L);    /* tap outside = cancel */
    lv_obj_t*p=lv_obj_create(g_choice); lv_obj_set_size(p,400,LV_SIZE_CONTENT); lv_obj_center(p);
    lv_obj_set_style_bg_color(p,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(p,R_BTN,0);
    lv_obj_set_style_border_color(p,lv_color_hex(C_ACCENT),0); lv_obj_set_style_border_width(p,2,0);
    lv_obj_set_style_pad_all(p,16,0); lv_obj_set_style_pad_row(p,12,0);
    lv_obj_set_flex_flow(p,LV_FLEX_FLOW_COLUMN); lv_obj_clear_flag(p,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*t=lv_label_create(p); lv_label_set_text(t,name);
    lv_obj_set_style_text_color(t,lv_color_hex(C_ACCENT),0); lv_obj_set_style_text_font(t,&lv_font_montserrat_20,0);
    char b[48]; snprintf(b,sizeof b,"Resume  (score %d)",score);
    choice_btn(p,b,1,1); choice_btn(p,"New game",0,0); }
static void games_go_cb(lv_event_t*e){ long i=(long)lv_event_get_user_data(e); int sc=0;
    if(i==-1){ if(game_has_save(&sc)) ask_resume(i,"BLOCKFALL",sc); else start_builtin(i,0); }
    else if(i==-3){ null_live(); lv_obj_t*ps=pet_create(show_games); g_scr=SCR_GAME; load_screen(ps); }
    else if(i==-4) show_gb_list();
    else if(i==-2){ if(brick_has_save(&sc)) ask_resume(i,"BRICKSTORM",sc); else start_builtin(i,0); }
    else if(i==-5){ if(coil_has_save(&sc)) ask_resume(i,"COIL",sc); else start_builtin(i,0); }
    else run_doom((int)i); }
/* ---- high scores: best in each Games row + a page with every top 5 ---- */
static const struct { const char*name; const char*file; } SCORES[]={
    {"Blockfall","/usr/data/r3_blockfall"}, {"Brickstorm","/usr/data/r3_brickstorm"}, {"Coil","/usr/data/r3_coil"} };
#define NSCORES ((int)(sizeof SCORES/sizeof SCORES[0]))
static const char* game_sub(int k,int in_progress,const char*idle){   /* row subtitle with the record */
    static char b[NSCORES][80]; hiscore_t h={SCORES[k].file}; hs_load(&h);
    const char*base=in_progress?"game in progress":idle;
    if(h.n) snprintf(b[k],sizeof b[k],"%s - best %d",base,h.s[0]); else snprintf(b[k],sizeof b[k],"%s",base);
    return b[k]; }
static void scores_back_cb(lv_event_t*e){ (void)e; show_games(); }
static void show_scores(void);
static void scores_reset_cb(lv_event_t*e){        /* first tap arms, second tap erases */
    lv_obj_t*b=lv_event_get_target(e); lv_obj_t*t=lv_obj_get_child(b,0);
    if(!lv_obj_has_state(b,LV_STATE_CHECKED)){ lv_obj_add_state(b,LV_STATE_CHECKED);
        lv_label_set_text(t,"Tap again to erase all scores"); return; }
    for(int k=0;k<NSCORES;k++) unlink(SCORES[k].file);
    show_scores(); }
static void show_scores(void){
    lv_obj_t*scr,*l=page_new("High scores",scores_back_cb,&scr);
    for(int k=0;k<NSCORES;k++){
        hiscore_t h={SCORES[k].file}; hs_load(&h);
        char up[24]; snprintf(up,sizeof up,"%s",SCORES[k].name); for(char*c=up;*c;c++) *c=(char)toupper((unsigned char)*c);
        set_section(l,up);
        lv_obj_t*p=lv_obj_create(l); lv_obj_set_size(p,LV_PCT(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(p,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(p,R_BTN,0);
        lv_obj_set_style_border_width(p,0,0); lv_obj_set_style_pad_all(p,12,0); lv_obj_set_style_pad_row(p,6,0);
        lv_obj_set_flex_flow(p,LV_FLEX_FLOW_COLUMN); lv_obj_clear_flag(p,LV_OBJ_FLAG_SCROLLABLE);
        if(!h.n){ lv_obj_t*n=lv_label_create(p); lv_label_set_text(n,"no score yet");
            lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); continue; }
        for(int i=0;i<h.n;i++){
            lv_obj_t*row=lv_obj_create(p); lv_obj_set_size(row,LV_PCT(100),28);
            lv_obj_set_style_bg_opa(row,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(row,0,0);
            lv_obj_set_style_pad_all(row,0,0); lv_obj_clear_flag(row,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
            uint32_t col=i==0?C_ACCENT:C_FG; char b[16];
            lv_obj_t*a=lv_label_create(row); snprintf(b,sizeof b,"%d.",i+1); lv_label_set_text(a,b);
            lv_obj_set_style_text_color(a,lv_color_hex(C_DIM),0); lv_obj_align(a,LV_ALIGN_LEFT_MID,4,0);
            lv_obj_t*s=lv_label_create(row); snprintf(b,sizeof b,"%d",h.s[i]); lv_label_set_text(s,b);
            lv_obj_set_style_text_font(s,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(s,lv_color_hex(col),0);
            lv_obj_align(s,LV_ALIGN_RIGHT_MID,-150,0);
            lv_obj_t*d=lv_label_create(row); lv_label_set_text(d,h.d[i]);
            lv_obj_set_style_text_color(d,lv_color_hex(C_DIM),0); lv_obj_align(d,LV_ALIGN_RIGHT_MID,-4,0); }
    }
    lv_obj_t*sp=lv_obj_create(l); lv_obj_remove_style_all(sp); lv_obj_set_size(sp,10,16);   /* gap, draws nothing */
    lv_obj_t*rb=lv_btn_create(l); lv_obj_set_size(rb,LV_PCT(100),56);
    lv_obj_set_style_radius(rb,R_BTN,0); lv_obj_set_style_shadow_width(rb,0,0);
    lv_obj_set_style_bg_color(rb,lv_color_hex(C_PANEL),0);
    lv_obj_set_style_bg_color(rb,lv_color_hex(0x8A2A1E),LV_STATE_CHECKED);
    lv_obj_add_event_cb(rb,scores_reset_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*rl=lv_label_create(rb); lv_label_set_text(rl,"Reset all scores"); lv_obj_center(rl);
    lv_obj_set_style_text_color(rl,lv_color_hex(C_DIM),0);
    g_scr=SCR_SETTINGS; load_screen(scr);
}
static void scores_go_cb(lv_event_t*e){ (void)e; show_scores(); }
static void show_games(void){
    lv_obj_t*scr,*l=page_new("Games",games_back_cb,&scr);
    lv_obj_t*pad=lv_obj_create(l); lv_obj_set_size(pad,10,8);
    lv_obj_set_style_bg_opa(pad,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(pad,0,0);
    int sc;
    lv_obj_t*r=set_row(l,LV_SYMBOL_SHUFFLE,"Blockfall",game_sub(0,game_has_save(&sc),"falling-block puzzle"),0);
    lv_obj_add_event_cb(r,games_go_cb,LV_EVENT_CLICKED,(void*)-1L);
    r=set_row(l,LV_SYMBOL_EYE_OPEN,"Moko",pet_summary(),pet_needs_you());
    lv_obj_add_event_cb(r,games_go_cb,LV_EVENT_CLICKED,(void*)-3L);
    r=set_row(l,LV_SYMBOL_STOP,"Brickstorm",game_sub(1,brick_has_save(&sc),"break the bricks, catch the balls"),0);
    lv_obj_add_event_cb(r,games_go_cb,LV_EVENT_CLICKED,(void*)-2L);
    r=set_row(l,LV_SYMBOL_LOOP,"Coil",game_sub(2,coil_has_save(&sc),"eat, grow, don't bite yourself"),0);
    lv_obj_add_event_cb(r,games_go_cb,LV_EVENT_CLICKED,(void*)-5L);
    r=set_row(l,LV_SYMBOL_LIST,"High scores","top 5 of every game",0);
    lv_obj_add_event_cb(r,scores_go_cb,LV_EVENT_CLICKED,NULL);
    { struct stat gst; gb_scan(); char sub[48];
      if(stat(GB_BIN,&gst)==0){ snprintf(sub,sizeof sub,gb_nroms?"%d games":"copy your ROMs to Games/GameBoy",gb_nroms);
        r=set_row(l,LV_SYMBOL_IMAGE,"Game Boy",sub,0);
        lv_obj_add_event_cb(r,games_go_cb,LV_EVENT_CLICKED,(void*)-4L); } }
    struct stat st; int have_bin = stat(DOOM_BIN,&st)==0, any=0;
    for(long i=0;i<NDOOM;i++){
        if(!file_ok(DOOM_DIR,DOOMS[i].iwad) || (DOOMS[i].pwad && !file_ok(DOOM_DIR,DOOMS[i].pwad))) continue;
        if(!any){ set_section(l,"DOOM"); any=1; }
        r=set_row(l,LV_SYMBOL_PLAY,DOOMS[i].name,DOOMS[i].sub,0);
        if(have_bin) lv_obj_add_event_cb(r,games_go_cb,LV_EVENT_CLICKED,(void*)i);
    }
    if(!any){ lv_obj_t*n=lv_label_create(l);
        lv_label_set_text(n,"Doom: copy your doom.wad / doom2.wad\n(tnt, plutonia, sigil, nerve...) to\nGames/Doom on the SD card.");
        lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0); }
    else { lv_obj_t*n=lv_label_create(l);
        lv_label_set_text(n,"In game: power = menu, hold power = quit,\nplay = use, volume = weapons, next = map.");
        lv_obj_set_style_text_color(n,lv_color_hex(C_DIM),0); lv_obj_set_style_pad_all(n,12,0); }
    g_scr=SCR_SETTINGS; load_screen(scr);
}

/* game controller / remote: games get it first, otherwise it is a music remote */
static void scroll_current(int dy){ lv_obj_t*l = set_list?set_list : (g_scr==SCR_BROWSER?g_brlist:NULL);
    if(l) lv_obj_scroll_by_bounded(l,0,dy,LV_ANIM_ON); }
static void pad_action(int a){
    g_last_input=r3_tick_ms();
    if(g_scr_off){ screen_wake(); return; }                          /* first press only wakes */
    if(pt_lbl) return;                               /* Test buttons page: only light the boxes */
    if(g_scr==SCR_CLOCK){ show_home(); return; }
    if(g_scr==SCR_GAME && (game_pad(a)||brick_pad(a)||coil_pad(a))) return;
    switch(a){
    case PA_A: if(!audio_is_playing() && g_pln>0) pl_resume();
               else { audio_toggle_pause(); save_resume();
                      if(np_play) lv_label_set_text(np_play,audio_is_paused()?LV_SYMBOL_PLAY:LV_SYMBOL_PAUSE); } break;
    case PA_R: pl_next(); break;
    case PA_L: pl_prev(); break;
    case PA_PLUS: case PA_X: bump_vol(+12); break;
    case PA_MINUS: case PA_Y: bump_vol(-12); break;
    case PA_HOME: show_home(); break;
    case PA_B: power_short(); break;
    case PA_UP: scroll_current(+120); break;
    case PA_DOWN: scroll_current(-120); break;
    }
}
static void heartbeat(void){ static uint32_t lh=0; uint32_t n=r3_tick_ms();
    if(n-lh>=2000){ lh=n; printf("hb %lu\n",(unsigned long)n); } }
static void fmt_ms(long ms,char*o,int c){ long s=ms/1000; snprintf(o,c,"%ld:%02ld",s/60,s%60); }
/* SD card hot-plug: mount when the card appears, lazily unmount when it is pulled */
static int sd_mounted(void){ FILE*f=fopen("/proc/mounts","r"); if(!f) return 0; char l[256]; int m=0;
    while(fgets(l,sizeof l,f)) if(strstr(l," /usr/data/mnt/sd_0 ")) m=1; fclose(f); return m; }
static void sd_hotplug(void){
    int present = access("/dev/mmcblk0p1",F_OK)==0, mounted=sd_mounted();
    if(present && !mounted){ mkdir("/usr/data/mnt",0755); mkdir("/usr/data/mnt/sd_0",0755);
        if(mount("/dev/mmcblk0p1","/usr/data/mnt/sd_0","vfat",0,"codepage=936,iocharset=utf8")==0) DBG("sd mounted\n"); }
    else if(!present && mounted){ umount2("/usr/data/mnt/sd_0",MNT_DETACH); DBG("sd removed\n"); }
}
static void tick_ui(void){
    { static uint32_t lsd; uint32_t t=r3_tick_ms(); if(t-lsd>=3000){ lsd=t; sd_hotplug(); } }
    static uint32_t lc=0,lp=0; uint32_t now=r3_tick_ms();
    static uint32_t ls=0;   /* keep the resume point fresh: survives restarts / flat battery, not just power-off */
    if(audio_is_playing() && !audio_is_paused() && now-ls>=30000){ ls=now; save_resume(); smartmix_flush(); }
    if(now-lc>=1000){ lc=now; if(clock_lbl){ time_t t=time(0); char b[8];
        strftime(b,sizeof b,"%H:%M",localtime(&t)); lv_label_set_text(clock_lbl,b);} }
    if(now-lp>=300 && (np_pos||np_prog)){ lp=now;
        long pos=audio_pos_ms(), dur=audio_dur_ms();
        char p[16],d[16]; fmt_ms(pos,p,sizeof p); fmt_ms(dur,d,sizeof d);
        if(np_pos) lv_label_set_text(np_pos,p);
        if(np_dur) lv_label_set_text(np_dur,d);
        if(np_prog && dur>0 && !lv_obj_has_state(np_prog,LV_STATE_PRESSED) && now>g_prog_hold)
            lv_slider_set_value(np_prog,(int32_t)((long long)pos*1000/dur),LV_ANIM_OFF);
    }
    if(g_dl_lbl){ char t[48];
        if(g_dl_run) snprintf(t,sizeof t,"Downloading %d / %d...",g_dl_done,g_dl_total);
        else if(g_dl_total) snprintf(t,sizeof t,LV_SYMBOL_OK " %d downloaded",g_dl_done);
        else t[0]=0; lv_label_set_text(g_dl_lbl,t); }
    if(set_list && bt_version()!=set_ver) settings_fill();
    if(wf_list && wifi_version()!=wf_ver) wifi_fill();
    static uint32_t lpet=0;
    if(now-lpet>=30000 || !lpet){ lpet=now|1; pet_tick();
        if(pet_bell){ if(pet_needs_you()) lv_obj_clear_flag(pet_bell,LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pet_bell,LV_OBJ_FLAG_HIDDEN); } }
    static uint32_t lbat=0;
    if(batt_lbl && (now-lbat>=10000 || !lbat)){ lbat=now|1;
        int cap=-1; char st[24]="";
        FILE*f=fopen("/sys/class/power_supply/battery/capacity","r"); if(f){ if(fscanf(f,"%d",&cap)!=1)cap=-1; fclose(f); }
        f=fopen("/sys/class/power_supply/battery/status","r"); if(f){ if(!fgets(st,sizeof st,f)) st[0]=0; fclose(f); }
        if(cap>=0){ const char*sym= !strncmp(st,"Charging",8)?LV_SYMBOL_CHARGE: cap>80?LV_SYMBOL_BATTERY_FULL:
                cap>55?LV_SYMBOL_BATTERY_3: cap>30?LV_SYMBOL_BATTERY_2: cap>10?LV_SYMBOL_BATTERY_1:LV_SYMBOL_BATTERY_EMPTY;
            char b[32]; snprintf(b,sizeof b,"%s %d%%",sym,cap); lv_label_set_text(batt_lbl,b);
            if(btbat_lbl){ int hb=bt_audio_battery(NULL,0), chg=0, pb=pad_battery(0,&chg); char t[48]="";
                /* headset (HFP / BLE) and game controller batteries, when they report one */
                if(hb>=0) snprintf(t,sizeof t,LV_SYMBOL_BLUETOOTH " %d%%",hb);
                if(pb>=0) snprintf(t+strlen(t),sizeof t-strlen(t),"%s" LV_SYMBOL_KEYBOARD " %d%%%s",t[0]?"  ":"",pb,chg?"+":"");
                lv_label_set_text(btbat_lbl,t);
                lv_obj_set_style_text_color(btbat_lbl,lv_color_hex((hb>=0&&hb<=15)||(pb>=0&&pb<=25&&!chg)?C_ACCENT:C_DIM),0); }
            lv_obj_set_style_text_color(batt_lbl,lv_color_hex(cap<=15?C_ACCENT:C_DIM),0); } }
    static uint32_t lr=0;
    if(bt_receiver() && now-lr>=120){ lr=now; audio_dac_follow(); }   /* cheap: cached mixer ctls */
    if(audio_bt_failed()){ g_resume_ms=audio_pos_ms(); if(np_play) lv_label_set_text(np_play,LV_SYMBOL_PLAY); }
    if(g_volov && !lv_obj_has_flag(g_volov,LV_OBJ_FLAG_HIDDEN) && now>g_vol_hide)
        { lv_obj_add_flag(g_volov,LV_OBJ_FLAG_HIDDEN);   /* auto-hide the volume overlay */
          FILE*vf=fopen(VOL_FILE,"w"); if(vf){ fprintf(vf,"%d\n",audio_get_volume()); fclose(vf); } }   /* remember it */
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
    char pos[32]={0}; long ms=0; FILE*f2=fopen(RESUME_FILE,"r");
    if(f2){ char tmp[1024]; if(fgets(tmp,sizeof tmp,f2)&&fgets(tmp,sizeof tmp,f2)&&fgets(pos,sizeof pos,f2)) ms=atol(pos); fclose(f2); }
    pl_build(dir,name);
    g_resume_ms=ms;
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
    sd_hotplug();                  /* mount the SD card (and keep watching for insert / removal) */
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
    { FILE*vf=fopen(VOL_FILE,"r"); int v; if(vf){ if(fscanf(vf,"%d",&v)==1) audio_set_volume(v); fclose(vf); } }   /* last volume, else default 160 */
    prefs_load(); bl_set(p_bright); audio_set_dac_filter(p_filter,p_nos);
    make_vol_overlay();
    show_home();
    load_resume();                 /* if a track was playing at power-off, open it ready */
    if(argc>2 && (!strcmp(argv[1],"-t")||!strcmp(argv[1],"-a")||!strcmp(argv[1],"-u"))){
        const char*b=strrchr(argv[2],'/');
        if(strcmp(argv[1],"-u")) audio_play(argv[2]);
        if(strcmp(argv[1],"-a")) show_player(b?b+1:argv[2],"/usr/data/mnt/sd_0/Music");
    }
    g_last_input=r3_tick_ms();
    wifi_start();                  /* Wi-Fi worker: radio + music server, if switched on */
    share_start();                 /* LAN peer discovery for playlist sharing */
    smartmix_start(MUSIC_ROOT);     /* background index for smart suggestions */
    bt_start();                    /* BT worker: daemons, auto-reconnect remembered speaker */
    printf("r3player UI up\n");
    for(;;){
        if(g_scr_off){ struct input_event e;
            while(read(tsfd,&e,sizeof e)==sizeof e)
                if(e.type==EV_KEY && e.code==BTN_TOUCH && e.value){ g_wake_swallow=1; t_down=1; screen_wake(); }
        }
        if(!g_scr_off) lv_timer_handler();
        if(!g_scr_off && p_timeout && r3_tick_ms()-g_last_input >= (uint32_t)p_timeout*1000) screen_off();
        if(g_to_del){ lv_obj_del(g_to_del); g_to_del=NULL; }
        keys_poll();
        pad_poll(); { int a; while(pad_next(&a)) pad_action(a); }
        if(audio_finished()){ if(g_pln>0&&g_pli>=0) smartmix_note(g_pl[g_pli],1); pl_next(); }  /* finished = a play; auto-advance */
        tick_ui(); idle_poll(); heartbeat(); usleep(15000); }
    return 0;
}
