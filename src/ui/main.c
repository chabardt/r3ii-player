/* r3ii-player — LVGL UI for HiBy R3 Pro II (480x720 RGB565, goodix touch, X1600).
 * Home menu -> Music directory browser -> Now Playing with audio playback. */
#include "lvgl.h"
#include "r3_tick.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#define MUSIC_ROOT "/usr/data/mnt/sd_0/Music"

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
        memcpy(fbmem+off,cp,w); if(FBPAGE2)memcpy(fbmem+FBPAGE2+off,cp,w);
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

/* ---- palette ---- */
#define C_SCR 0xF2E9D8
#define C_INK 0x241F1C
#define C_PANEL 0xFBF6EA
#define C_TEAL 0x2F8F8F
#define C_ORANGE 0xE07A3F
#define C_MAROON 0x9C3B4E

static lv_obj_t *clock_lbl, *np_pos, *np_play; /* live-updated widgets (NULL when off-screen) */
static char np_dur[16];

/* switch screens now, but defer deleting the old one to the main loop (never mid-event) */
static lv_obj_t *g_to_del;
static void load_screen(lv_obj_t*ns){ lv_obj_t*old=lv_scr_act(); lv_scr_load(ns);
    if(old&&old!=ns) g_to_del=old; }
static void show_home(void);
static void show_browser(const char*path);

/* ---- helpers ---- */
static int is_mp3(const char*n){ const char*d=strrchr(n,'.'); return d&&!strcasecmp(d,".mp3"); }
static int is_audio(const char*n){ const char*d=strrchr(n,'.'); if(!d)return 0;
    return !strcasecmp(d,".mp3")||!strcasecmp(d,".flac")||!strcasecmp(d,".wav")
         ||!strcasecmp(d,".ogg")||!strcasecmp(d,".m4a")||!strcasecmp(d,".ape"); }
static void pretty(const char*in,char*out,int cap){ /* strip leading "NN. " and ".mp3" */
    const char*s=in; while(*s>='0'&&*s<='9')s++;
    if(*s=='.'||*s=='-'||*s==' '){ while(*s=='.'||*s=='-'||*s==' ')s++; } else s=in;
    strncpy(out,s,cap-1); out[cap-1]=0;
    char*d=strrchr(out,'.'); if(d&&!strcasecmp(d,".mp3"))*d=0;
}

/* ---- Now Playing ---- */
static void np_back_cb(lv_event_t*e){ const char*dir=lv_event_get_user_data(e); show_browser(dir); }
static void np_playpause_cb(lv_event_t*e){ (void)e; audio_toggle_pause();
    if(np_play) lv_label_set_text(np_play, audio_is_paused()?LV_SYMBOL_PLAY:LV_SYMBOL_PAUSE); }
static void np_vol_cb(lv_event_t*e){ lv_obj_t*s=lv_event_get_target(e); audio_set_volume(lv_slider_get_value(s)); }

static void show_player(const char*title,const char*dir){
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_SCR),0);
    lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    /* cover placeholder (full width square) */
    lv_obj_t*cov=lv_obj_create(scr); lv_obj_set_size(cov,480,380); lv_obj_set_pos(cov,0,0);
    lv_obj_set_style_radius(cov,0,0); lv_obj_set_style_border_width(cov,0,0);
    lv_obj_set_style_bg_color(cov,lv_color_hex(C_TEAL),0);
    lv_obj_clear_flag(cov,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*cl=lv_label_create(cov); lv_label_set_text(cl,LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cl,lv_color_hex(C_SCR),0);
    lv_obj_set_style_text_font(cl,&lv_font_montserrat_28,0); lv_obj_center(cl);
    /* title */
    lv_obj_t*t=lv_label_create(scr); lv_label_set_text(t,title);
    lv_label_set_long_mode(t,LV_LABEL_LONG_SCROLL_CIRCULAR); lv_obj_set_width(t,440);
    lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(t,lv_color_hex(C_INK),0);
    lv_obj_set_style_text_font(t,&lv_font_montserrat_20,0);
    lv_obj_align(t,LV_ALIGN_TOP_MID,0,395);
    /* position */
    np_pos=lv_label_create(scr); lv_label_set_text(np_pos,"0:00");
    lv_obj_set_style_text_color(np_pos,lv_color_hex(C_TEAL),0);
    lv_obj_align(np_pos,LV_ALIGN_TOP_MID,0,435);
    /* transport */
    lv_obj_t*back=lv_btn_create(scr); lv_obj_set_size(back,90,70); lv_obj_align(back,LV_ALIGN_TOP_LEFT,20,490);
    lv_obj_set_style_bg_color(back,lv_color_hex(C_PANEL),0); lv_obj_set_style_border_color(back,lv_color_hex(C_INK),0);
    lv_obj_set_style_border_width(back,3,0); lv_obj_set_style_radius(back,0,0);
    lv_obj_add_event_cb(back,np_back_cb,LV_EVENT_CLICKED,(void*)dir);
    lv_obj_t*bl=lv_label_create(back); lv_label_set_text(bl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl,lv_color_hex(C_INK),0); lv_obj_center(bl);
    lv_obj_t*pp=lv_btn_create(scr); lv_obj_set_size(pp,120,80); lv_obj_align(pp,LV_ALIGN_TOP_MID,0,485);
    lv_obj_set_style_bg_color(pp,lv_color_hex(C_ORANGE),0); lv_obj_set_style_border_color(pp,lv_color_hex(C_INK),0);
    lv_obj_set_style_border_width(pp,3,0); lv_obj_set_style_radius(pp,0,0);
    lv_obj_add_event_cb(pp,np_playpause_cb,LV_EVENT_CLICKED,NULL);
    np_play=lv_label_create(pp); lv_label_set_text(np_play,LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(np_play,lv_color_hex(C_INK),0);
    lv_obj_set_style_text_font(np_play,&lv_font_montserrat_28,0); lv_obj_center(np_play);
    /* volume */
    lv_obj_t*vol=lv_slider_create(scr); lv_obj_set_size(vol,400,16); lv_obj_align(vol,LV_ALIGN_TOP_MID,0,610);
    lv_slider_set_range(vol,0,255); lv_slider_set_value(vol,audio_get_volume(),LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol,lv_color_hex(C_MAROON),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol,lv_color_hex(C_INK),LV_PART_KNOB);
    lv_obj_add_event_cb(vol,np_vol_cb,LV_EVENT_VALUE_CHANGED,NULL);
    load_screen(scr);
}

/* ---- Browser ---- */
typedef struct { char name[256]; int dir; } entry_t;
static int ecmp(const void*a,const void*b){ const entry_t*x=a,*y=b;
    if(x->dir!=y->dir)return y->dir-x->dir; return strcasecmp(x->name,y->name); }

static void entry_cb(lv_event_t*e){
    char*full=lv_event_get_user_data(e); /* strdup'd full path */
    DBG("entry_cb: %s\n",full);
    struct stat st;
    if(stat(full,&st)==0 && S_ISDIR(st.st_mode)){ show_browser(full); }
    else { char ttl[256]; const char*base=strrchr(full,'/'); pretty(base?base+1:full,ttl,sizeof ttl);
        DBG("audio_play start\n"); audio_play(full); DBG("audio_play returned\n");
        char dir[1024]; strncpy(dir,full,sizeof dir-1); dir[sizeof dir-1]=0;
        char*sl=strrchr(dir,'/'); if(sl)*sl=0;
        static char pdir[1024]; strncpy(pdir,dir,sizeof pdir-1); pdir[sizeof pdir-1]=0;
        DBG("show_player start\n"); show_player(ttl,pdir); DBG("show_player done\n"); }
}
static void br_back_cb(lv_event_t*e){ char*parent=lv_event_get_user_data(e);
    if(!parent||!parent[0]||!strcmp(parent,"/")) show_home(); else show_browser(parent); }
/* free malloc'd path user_data when a widget is deleted (prevents OOM on 57MB device) */
static void free_cb(lv_event_t*e){ void*p=lv_event_get_user_data(e); if(p)free(p); }

static void show_browser(const char*path){
    DBG("show_browser START %s\n",path);
    char cur[1024]; strncpy(cur,path,sizeof cur-1); cur[sizeof cur-1]=0; /* own copy: path may alias a reused buffer */
    np_pos=np_play=NULL;
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_SCR),0);
    lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    /* header */
    lv_obj_t*hdr=lv_obj_create(scr); lv_obj_set_size(hdr,480,54); lv_obj_set_pos(hdr,0,0);
    lv_obj_set_style_bg_color(hdr,lv_color_hex(C_TEAL),0); lv_obj_set_style_radius(hdr,0,0);
    lv_obj_set_style_border_width(hdr,0,0); lv_obj_clear_flag(hdr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bk=lv_btn_create(hdr); lv_obj_set_size(bk,46,40); lv_obj_align(bk,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_set_style_bg_color(bk,lv_color_hex(C_SCR),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(C_INK),0);
    char*parent=malloc(1024);   /* own buffer per screen (no aliasing with path) */
    if(!strcmp(cur,MUSIC_ROOT)) parent[0]=0;
    else { strncpy(parent,cur,1023); parent[1023]=0;
        char*sl=strrchr(parent,'/'); if(sl)*sl=0; }
    lv_obj_add_event_cb(bk,br_back_cb,LV_EVENT_CLICKED,parent);
    lv_obj_add_event_cb(bk,free_cb,LV_EVENT_DELETE,parent);
    lv_obj_t*bkl=lv_label_create(bk); lv_label_set_text(bkl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bkl,lv_color_hex(C_INK),0); lv_obj_center(bkl);
    lv_obj_t*ttl=lv_label_create(hdr); const char*base=strrchr(cur,'/');
    lv_label_set_text(ttl,!strcmp(cur,MUSIC_ROOT)?"Music":(base?base+1:cur));
    lv_label_set_long_mode(ttl,LV_LABEL_LONG_DOT); lv_obj_set_width(ttl,380);
    lv_obj_set_style_text_color(ttl,lv_color_hex(0xFFFFFF),0); lv_obj_align(ttl,LV_ALIGN_LEFT_MID,56,0);
    /* list */
    lv_obj_t*list=lv_list_create(scr); lv_obj_set_size(list,480,720-54); lv_obj_set_pos(list,0,54);
    lv_obj_set_style_bg_color(list,lv_color_hex(C_SCR),0); lv_obj_set_style_border_width(list,0,0);
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
            else { char full[1024]; snprintf(full,sizeof full,"%s/%s",cur,de->d_name);
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
            lv_obj_set_style_text_color(b,lv_color_hex(C_INK),0);
            lv_obj_set_style_border_color(b,lv_color_hex(C_INK),0);
            lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,0,0);
            char*full=malloc(1024); snprintf(full,1024,"%s/%s",cur,es[i].name);
            lv_obj_add_event_cb(b,entry_cb,LV_EVENT_CLICKED,full);
            lv_obj_add_event_cb(b,free_cb,LV_EVENT_DELETE,full);
        }
        DBG("show_browser built %d entries\n",ne);
        free(es);
    } else DBG("opendir FAILED %s\n",cur);
    load_screen(scr);
    DBG("show_browser DONE (loaded)\n");
}

/* ---- Home ---- */
static void tile_cb(lv_event_t*e){ const char*name=lv_event_get_user_data(e);
    if(!strcmp(name,"Music")) show_browser(MUSIC_ROOT);
    else DBG("tile (todo): %s\n",name); }
static void make_tile(lv_obj_t*g,const char*txt,uint32_t accent,int col,int row){
    lv_obj_t*b=lv_btn_create(g);
    lv_obj_set_grid_cell(b,LV_GRID_ALIGN_STRETCH,col,1,LV_GRID_ALIGN_STRETCH,row,1);
    lv_obj_set_style_bg_color(b,lv_color_hex(C_PANEL),0); lv_obj_set_style_radius(b,0,0);
    lv_obj_set_style_border_color(b,lv_color_hex(C_INK),0); lv_obj_set_style_border_width(b,3,0);
    lv_obj_set_style_shadow_color(b,lv_color_hex(C_INK),0); lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_shadow_ofs_x(b,4,0); lv_obj_set_style_shadow_ofs_y(b,4,0); lv_obj_set_style_shadow_opa(b,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(accent),LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(b,4,LV_STATE_PRESSED); lv_obj_set_style_translate_y(b,4,LV_STATE_PRESSED);
    lv_obj_set_style_shadow_ofs_x(b,0,LV_STATE_PRESSED); lv_obj_set_style_shadow_ofs_y(b,0,LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,tile_cb,LV_EVENT_CLICKED,(void*)txt);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,txt);
    lv_obj_set_style_text_color(l,lv_color_hex(accent),0);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0); lv_obj_center(l);
}
static void show_home(void){
    np_pos=np_play=NULL;
    lv_obj_t*scr=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr,lv_color_hex(C_SCR),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*bar=lv_obj_create(scr); lv_obj_set_size(bar,480,50); lv_obj_set_pos(bar,0,0);
    lv_obj_set_style_bg_color(bar,lv_color_hex(C_INK),0); lv_obj_set_style_radius(bar,0,0);
    lv_obj_set_style_border_width(bar,0,0); lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*nm=lv_label_create(bar); lv_label_set_text(nm,"r3ii");
    lv_obj_set_style_text_color(nm,lv_color_hex(C_SCR),0); lv_obj_align(nm,LV_ALIGN_LEFT_MID,0,0);
    clock_lbl=lv_label_create(bar); lv_label_set_text(clock_lbl,"--:--");
    lv_obj_set_style_text_color(clock_lbl,lv_color_hex(C_SCR),0); lv_obj_align(clock_lbl,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_t*h=lv_label_create(scr); lv_label_set_text(h,"HELLO");
    lv_obj_set_style_text_color(h,lv_color_hex(C_INK),0);
    lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0); lv_obj_align(h,LV_ALIGN_TOP_MID,0,70);
    static lv_coord_t cols[]={LV_GRID_FR(1),LV_GRID_FR(1),LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[]={LV_GRID_FR(1),LV_GRID_FR(1),LV_GRID_TEMPLATE_LAST};
    lv_obj_t*g=lv_obj_create(scr); lv_obj_set_style_bg_opa(g,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(g,0,0); lv_obj_set_style_pad_all(g,0,0);
    lv_obj_set_size(g,440,440); lv_obj_align(g,LV_ALIGN_TOP_MID,0,130);
    lv_obj_set_style_pad_row(g,20,0); lv_obj_set_style_pad_column(g,20,0);
    lv_obj_set_grid_dsc_array(g,cols,rows); lv_obj_clear_flag(g,LV_OBJ_FLAG_SCROLLABLE);
    make_tile(g,"Music",C_TEAL,0,0); make_tile(g,"Podcasts",C_ORANGE,1,0);
    make_tile(g,"Games",C_INK,0,1); make_tile(g,"Settings",C_MAROON,1,1);
    load_screen(scr);
}

static void heartbeat(void){ static uint32_t lh=0; uint32_t n=r3_tick_ms();
    if(n-lh>=2000){ lh=n; printf("hb %lu\n",(unsigned long)n); } } /* keep adb task alive */
static void fmt_ms(long ms,char*o,int c){ long s=ms/1000; snprintf(o,c,"%ld:%02ld",s/60,s%60); }
static void tick_ui(void){
    static uint32_t lc=0,lp=0; uint32_t now=r3_tick_ms();
    if(now-lc>=1000){ lc=now; if(clock_lbl){ time_t t=time(0); char b[8];
        strftime(b,sizeof b,"%H:%M",localtime(&t)); lv_label_set_text(clock_lbl,b);} }
    if(now-lp>=500 && np_pos){ lp=now; char p[16],d[16]; fmt_ms(audio_pos_ms(),p,sizeof p);
        fmt_ms(audio_dur_ms(),d,sizeof d); char line[40]; snprintf(line,sizeof line,"%s / %s",p,d);
        lv_label_set_text(np_pos,line); }
}

int main(int argc,char**argv){
    if(argc>1 && !strcmp(argv[1],"-d")){   /* daemonize: detach from the adb session so it persists */
        if(fork()>0)_exit(0); setsid();
        if(fork()>0)_exit(0);
        int fd=open("/usr/data/r3out.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd>=0){ dup2(fd,1); dup2(fd,2); if(fd>2)close(fd); }
        int nf=open("/dev/null",O_RDONLY); if(nf>=0){ dup2(nf,0); if(nf>0)close(nf); }
    }
    setvbuf(stdout,NULL,_IONBF,0);
    signal(SIGHUP,SIG_IGN); signal(SIGPIPE,SIG_IGN);
    dbgf=fopen("/usr/data/r3dbg.log","w");
    fb_init(); lv_init();
    static lv_disp_draw_buf_t db; static lv_color_t buf[480*120];
    lv_disp_draw_buf_init(&db,buf,NULL,480*120);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.draw_buf=&db; dd.flush_cb=disp_flush; dd.hor_res=480; dd.ver_res=720; lv_disp_drv_register(&dd);
    tsfd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type=LV_INDEV_TYPE_POINTER; id.read_cb=ts_read; lv_indev_drv_register(&id);
    show_home();
    printf("r3player UI up\n");
    for(;;){ lv_timer_handler();
        if(g_to_del){ DBG("del old screen\n");
                      lv_obj_del(g_to_del); g_to_del=NULL; DBG("del done\n"); }
        tick_ui(); heartbeat(); usleep(5000); }
    return 0;
}
