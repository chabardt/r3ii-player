/* r3ii-player — LVGL UI for HiBy R3 Pro II (480x720 RGB565, goodix touch, X1600).
 * Home menu -> Music directory browser -> Now Playing with audio playback.
 * Dark theme (black + orange), playlist w/ auto-advance + prev/next, physical keys. */
#include "lvgl.h"
#include "r3_tick.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/mount.h>

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
static void null_live(void){ clock_lbl=np_pos=np_play=np_title=np_prog=NULL; }
/* persistent volume overlay (lives on the top layer, survives screen changes) */
static lv_obj_t *g_volov, *g_volbar; static uint32_t g_vol_hide;
static uint32_t g_prog_hold;   /* don't auto-update the seek bar until this time (after a user seek) */

/* ---- screen state (for the physical power button) ---- */
enum { SCR_HOME, SCR_BROWSER, SCR_PLAYER };
static int g_scr=SCR_HOME;

/* switch screens now, but defer deleting the old one to the main loop (never mid-event) */
static lv_obj_t *g_to_del;
static void load_screen(lv_obj_t*ns){ lv_obj_t*old=lv_scr_act(); lv_scr_load(ns);
    if(old&&old!=ns) g_to_del=old; }
static void show_home(void);
static void show_browser(const char*path);
static void show_player(const char*title,const char*dir);

/* ---- helpers ---- */
static int is_audio(const char*n){ const char*d=strrchr(n,'.'); if(!d)return 0;
    return !strcasecmp(d,".mp3")||!strcasecmp(d,".mpga")||!strcasecmp(d,".flac")
         ||!strcasecmp(d,".wav")||!strcasecmp(d,".m4a"); }
static void pretty(const char*in,char*out,int cap){ /* strip leading "NN. " and audio ext */
    const char*s=in; while(*s>='0'&&*s<='9')s++;
    if(*s=='.'||*s=='-'||*s==' '){ while(*s=='.'||*s=='-'||*s==' ')s++; } else s=in;
    strncpy(out,s,cap-1); out[cap-1]=0;
    char*d=strrchr(out,'.'); if(d&&is_audio(out))*d=0;
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
    audio_play(full);
    if(np_title) lv_label_set_text(np_title,g_pltitle);
    if(np_play)  lv_label_set_text(np_play,LV_SYMBOL_PAUSE);
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

/* ---- album art ---- */
#define COVER_TMP "/usr/data/.cover.jpg"
static int find_cover(const char*dir,char*out,int cap){
    const char*names[]={"cover.jpg","Cover.jpg","folder.jpg","Folder.jpg","front.jpg","Front.jpg","cover.jpeg",0};
    struct stat st; char p[1600];
    for(int i=0;names[i];i++){ snprintf(p,sizeof p,"%s/%s",dir,names[i]);
        if(stat(p,&st)==0){ strncpy(out,p,cap-1); out[cap-1]=0; return 1; } }
    DIR*dp=opendir(dir); if(dp){ struct dirent*de;
        while((de=readdir(dp))){ const char*e=strrchr(de->d_name,'.');
            if(e&&(!strcasecmp(e,".jpg")||!strcasecmp(e,".jpeg"))){
                snprintf(out,cap,"%s/%s",dir,de->d_name); closedir(dp); return 1; } }
        closedir(dp); }
    return 0;
}
/* copy the JPEG found in buf (SOI..EOI) to outpath */
static int write_jpeg_from(const unsigned char*buf,long n,const char*outpath){
    long s=-1; for(long i=0;i+2<n;i++) if(buf[i]==0xFF&&buf[i+1]==0xD8&&buf[i+2]==0xFF){ s=i; break; }
    if(s<0) return 0;
    long e=-1; for(long i=n-2;i>s;i--) if(buf[i]==0xFF&&buf[i+1]==0xD9){ e=i+2; break; }
    if(e<0) e=n;
    FILE*o=fopen(outpath,"wb"); if(!o)return 0; fwrite(buf+s,1,e-s,o); fclose(o); return 1;
}
/* extract an embedded JPEG cover (ID3v2 APIC / FLAC PICTURE) to outpath */
static int extract_cover(const char*path,const char*outpath){
    FILE*f=fopen(path,"rb"); if(!f)return 0;
    unsigned char hd[10]; if(fread(hd,1,10,f)!=10){ fclose(f); return 0; }
    if(hd[0]=='I'&&hd[1]=='D'&&hd[2]=='3'){                 /* MP3 ID3v2: scan the tag body */
        long sz=((long)(hd[6]&0x7f)<<21)|((long)(hd[7]&0x7f)<<14)|((long)(hd[8]&0x7f)<<7)|(hd[9]&0x7f);
        if(sz<=0||sz>8*1024*1024){ fclose(f); return 0; }
        unsigned char*buf=malloc(sz); if(!buf){fclose(f);return 0;}
        long got=fread(buf,1,sz,f); fclose(f);
        int r=write_jpeg_from(buf,got,outpath); free(buf); return r;
    }
    if(hd[0]=='f'&&hd[1]=='L'&&hd[2]=='a'&&hd[3]=='C'){     /* FLAC: scan the metadata region */
        fseek(f,4,SEEK_SET); unsigned char bh[4]; long off=4; int last=0;
        while(!last && fread(bh,1,4,f)==4){ last=bh[0]&0x80;
            long bl=((long)bh[1]<<16)|((long)bh[2]<<8)|bh[3]; off+=4+bl; fseek(f,bl,SEEK_CUR); }
        long cap=off; if(cap<=0||cap>8*1024*1024){ fclose(f); return 0; }
        fseek(f,0,SEEK_SET); unsigned char*buf=malloc(cap); if(!buf){fclose(f);return 0;}
        long got=fread(buf,1,cap,f); fclose(f);
        int r=write_jpeg_from(buf,got,outpath); free(buf); return r;
    }
    fclose(f); return 0;
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
    DBG("prog_cb ev=%d val=%ld dur=%ld -> seek=%ld\n",lv_event_get_code(e),v,dur,dur>0?v*dur/1000:-1);
    if(dur>0 && np_prog){ audio_seek(v*dur/1000);
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
    lv_obj_t*cov=lv_obj_create(scr); lv_obj_set_size(cov,480,360); lv_obj_set_pos(cov,0,0);
    lv_obj_set_style_radius(cov,0,0); lv_obj_set_style_border_width(cov,0,0);
    lv_obj_set_style_bg_color(cov,lv_color_hex(C_PANEL),0);
    lv_obj_clear_flag(cov,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cov,0,0);
    /* album art spanning full width (vertical overflow cropped by the container).
     * Prefer a separate cover file; else pull the embedded JPEG from the track tags. */
    char cover[1700]; int have=0; cover[0]='S'; cover[1]=':';
    if(g_pldir[0] && find_cover(g_pldir,cover+2,sizeof cover-2)) have=1;
    else if(g_pln>0 && g_pli>=0){
        char trk[1700]; snprintf(trk,sizeof trk,"%s/%s",g_pldir,g_pl[g_pli]);
        if(extract_cover(trk,COVER_TMP)){ strcpy(cover,"S:" COVER_TMP); have=1; }
    }
    if(have){
        /* The JPEG decoder is a line-reader (no random access), so LVGL can't zoom it.
         * Draw at native size, centered; the container clips the overflow. Covers are
         * ~500px ≈ the 480px screen width, so this reads as full-width. */
        lv_obj_t*img=lv_img_create(cov); lv_img_set_src(img,cover);
        lv_obj_center(img);
    } else {
        lv_obj_t*cl=lv_label_create(cov); lv_label_set_text(cl,LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(cl,lv_color_hex(C_ACCENT),0);
        lv_obj_set_style_text_font(cl,&lv_font_montserrat_28,0); lv_obj_center(cl);
    }
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
    lv_obj_align(np_title,LV_ALIGN_TOP_MID,0,376);
    /* position */
    np_pos=lv_label_create(scr); lv_label_set_text(np_pos,"0:00");
    lv_obj_set_style_text_color(np_pos,lv_color_hex(C_DIM),0);
    lv_obj_align(np_pos,LV_ALIGN_TOP_MID,0,416);
    /* transport row: prev | play/pause | next  (np_play = the symbol inside play/pause) */
    mk_tbtn(scr, 90,76,-150,470, C_PANEL,  LV_SYMBOL_PREV, np_prev_cb);
    lv_obj_t*ppb=mk_tbtn(scr,120,86,0,465, C_ACCENT, LV_SYMBOL_PAUSE, np_playpause_cb);
    np_play=lv_obj_get_child(ppb,0);   /* the label mk_tbtn created inside it */
    mk_tbtn(scr, 90,76, 150,470, C_PANEL,  LV_SYMBOL_NEXT, np_next_cb);
    /* progress / seek bar (volume lives in the top overlay now) */
    np_prog=lv_slider_create(scr); lv_obj_set_size(np_prog,440,14); lv_obj_align(np_prog,LV_ALIGN_TOP_MID,0,600);
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
    lv_label_set_text(ttl,!strcmp(cur,g_root)?g_root_label:(base?base+1:cur));
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
            if(es[i].dir){ strncpy(disp,es[i].name,sizeof disp-1); disp[sizeof disp-1]=0; }
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

/* ---- Home ---- */
static void tile_cb(lv_event_t*e){ const char*name=lv_event_get_user_data(e);
    if(!strcmp(name,"Music")){ strcpy(g_root,MUSIC_ROOT); strcpy(g_root_label,"Music"); show_browser(MUSIC_ROOT); }
    else if(!strcmp(name,"Podcasts")){ strcpy(g_root,PODCAST_ROOT); strcpy(g_root_label,"Podcasts"); show_browser(PODCAST_ROOT); }
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
        if(e.code==116){ /* power */
            if(e.value==1){ pwr_down=r3_tick_ms(); pwr_held=1; }
            else if(e.value==0 && pwr_held){ pwr_held=0;
                if(r3_tick_ms()-pwr_down < 1200) power_short(); }
        } else if(e.code==165 && e.value==1){ pl_prev(); }  /* previous */
    }
    if(kf_media>=0) while(read(kf_media,&e,sizeof e)==sizeof e){
        if(e.type!=EV_KEY) continue;
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
            lv_slider_set_value(np_prog,pos*1000/dur,LV_ANIM_OFF);
    }
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
    printf("r3player UI up\n");
    for(;;){ lv_timer_handler();
        if(g_to_del){ lv_obj_del(g_to_del); g_to_del=NULL; }
        keys_poll();
        if(audio_finished()) pl_next();     /* album auto-advance */
        tick_ui(); heartbeat(); usleep(15000); }
    return 0;
}
