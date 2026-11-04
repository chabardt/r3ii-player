/* r3doom — doomgeneric backend for the HiBy R3 Pro II (480x720 portrait, goodix MT touch).
 *
 * Launched by r3player (Games) as its own process, which owns the screen and input until
 * it exits. Uses the player's own classic IWAD/PWADs; no game data is shipped.
 *
 *   +-----------------------+  y 0..299   game: 320x200 scaled x1.5 (tap = Enter)
 *   |        DOOM           |
 *   +-----------+-----------+  y 300..
 *   |           | TURN pad  |  right-top: swipe sideways = turn (mouse), tap = Use
 *   |   STICK   +-----------+
 *   | move/strafe|  FIRE   |  right-bottom: hold = fire (also Enter in menus)
 *   +-----------+-----------+
 * Keys: power = menu (Esc), long power = quit, play = Use, vol+/- = next/prev weapon,
 * next = automap. Always-run while the stick is pushed far. */
#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_video.h"
#include "pad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

#define SW 480
#define SH 720
#define GH 300                 /* game area height (200 * 1.5) */
#define DECK_Y GH
#define MID_X 240
#define FIRE_Y 540             /* right column split: turn pad above, fire below */

static uint16_t *fb; static int fbstride;   /* in pixels */
static int ts_fd=-1, kp_fd=-1, km_fd=-1;
static uint32_t t0;

static uint32_t now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint32_t)(ts.tv_sec*1000u+ts.tv_nsec/1000000u); }
uint32_t r3_tick_ms(void){ return now_ms(); }       /* for pad.c */

/* ---------------- key queue -> doomgeneric ---------------- */
#define KQ 64
static struct { unsigned char key; int pressed; } kq[KQ]; static int kq_r,kq_w;
static void key(unsigned char k,int down){ int n=(kq_w+1)%KQ; if(n==kq_r) return;
    kq[kq_w].key=k; kq[kq_w].pressed=down; kq_w=n; }
static void tapkey(unsigned char k){ key(k,1); key(k,0); }
int DG_GetKey(int*pressed,unsigned char*k){
    if(kq_r==kq_w) return 0;
    *pressed=kq[kq_r].pressed; *k=kq[kq_r].key; kq_r=(kq_r+1)%KQ; return 1; }
static void mouse_turn(int dx){ event_t ev; memset(&ev,0,sizeof ev);
    ev.type=ev_mouse; ev.data1=0; ev.data2=dx; ev.data3=0; D_PostEvent(&ev); }

/* ---------------- drawing helpers (deck) ---------------- */
static inline uint16_t rgb(int r,int g,int b){ return (uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)); }
static void fill(int x0,int y0,int w,int h,uint16_t c){
    for(int y=y0;y<y0+h && y<SH;y++){ uint16_t*p=fb+y*fbstride+x0; for(int x=0;x<w && x0+x<SW;x++) p[x]=c; } }
static void circle(int cx,int cy,int r,int th,uint16_t c,int filled){
    for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++){ int d=x*x+y*y;
        if(d<=r*r && (filled || d>=(r-th)*(r-th))){ int px=cx+x,py=cy+y;
            if(px>=0&&px<SW&&py>=DECK_Y&&py<SH) fb[py*fbstride+px]=c; } } }
#define C_BG   rgb(10,10,10)
#define C_PAN  rgb(26,21,16)
#define C_DIM  rgb(138,106,74)
#define C_ACC  rgb(232,115,28)
static int stick_cx=MID_X/2, stick_cy=DECK_Y+(SH-DECK_Y)/2;
static void draw_deck(int kx,int ky,int fire_on){
    fill(0,DECK_Y,SW,SH-DECK_Y,C_BG);
    fill(MID_X+2,DECK_Y+6,SW-MID_X-8,FIRE_Y-DECK_Y-10,C_PAN);          /* turn pad */
    for(int i=0;i<3;i++){ int y=DECK_Y+(FIRE_Y-DECK_Y)/2;               /* <-> hint */
        fill(MID_X+60,y-1+i,SW-MID_X-120,1,C_DIM); }
    circle(stick_cx,stick_cy,110,3,C_DIM,0);                           /* stick ring */
    circle(kx,ky,42,42,C_ACC,1);                                       /* knob */
    circle(MID_X+(SW-MID_X)/2,FIRE_Y+(SH-FIRE_Y)/2,70,fire_on?70:5,C_ACC,fire_on); /* FIRE */
}

/* ---------------- touch: multitouch protocol B (slots) ---------------- */
#define NS 10
static struct { int id,x,y,down, role, ox,oy, lx; uint32_t t; } sl[NS];
enum { R_NONE, R_STICK, R_TURN, R_FIRE, R_GAME };
static int cur_slot;
static int st_up,st_dn,st_l,st_r,st_run, fire_held;
static void stick_apply(int dx,int dy){
    const int dz=22, far=80;
    int up=dy<-dz, dn=dy>dz, l=dx<-dz, r=dx>dz, run=(dx*dx+dy*dy)>far*far;
    if(up!=st_up){ key(KEY_UPARROW,up); st_up=up; }
    if(dn!=st_dn){ key(KEY_DOWNARROW,dn); st_dn=dn; }
    if(l!=st_l){ key(KEY_STRAFE_L,l); st_l=l; }
    if(r!=st_r){ key(KEY_STRAFE_R,r); st_r=r; }
    if(run!=st_run){ key(KEY_RSHIFT,run); st_run=run; }
}
static int deck_dirty=1, knob_x, knob_y;
static void touch_down(int i){
    int x=sl[i].x,y=sl[i].y; sl[i].ox=x; sl[i].oy=y; sl[i].lx=x; sl[i].t=now_ms();
    if(y<DECK_Y) sl[i].role=R_GAME;
    else if(x<MID_X){ sl[i].role=R_STICK; }
    else if(y>=FIRE_Y){ sl[i].role=R_FIRE; key(KEY_FIRE,1); key(KEY_ENTER,1); fire_held=1; deck_dirty=1; }
    else sl[i].role=R_TURN;
}
static void touch_move(int i){
    int x=sl[i].x,y=sl[i].y;
    if(sl[i].role==R_STICK){ int dx=x-stick_cx, dy=y-stick_cy;   /* fixed-centre stick */
        stick_apply(dx,dy);
        int d2=dx*dx+dy*dy; if(d2>110*110){ float s=110.0f/__builtin_sqrtf((float)d2); dx=(int)(dx*s); dy=(int)(dy*s); }
        knob_x=stick_cx+dx; knob_y=stick_cy+dy; deck_dirty=1; }
    else if(sl[i].role==R_TURN){ int d=x-sl[i].lx; sl[i].lx=x; if(d) mouse_turn(d*6); }
}
static void touch_up(int i){
    int r=sl[i].role; sl[i].role=R_NONE;
    if(r==R_STICK){ stick_apply(0,0); knob_x=stick_cx; knob_y=stick_cy; deck_dirty=1; }
    else if(r==R_FIRE){ key(KEY_FIRE,0); key(KEY_ENTER,0); fire_held=0; deck_dirty=1; }
    else if(r==R_TURN){ int moved=abs(sl[i].x-sl[i].ox)>14||abs(sl[i].y-sl[i].oy)>14;
        if(!moved && now_ms()-sl[i].t<300) tapkey(KEY_USE); }
    else if(r==R_GAME){ if(now_ms()-sl[i].t<400) tapkey(KEY_ENTER); }
}
static int mt_b_seen;
static void poll_touch(void){
    struct input_event e; static int pending_down[NS], st_x, st_y, st_down;
    while(read(ts_fd,&e,sizeof e)==sizeof e){
        if(e.type==EV_ABS){
            switch(e.code){
            case ABS_MT_SLOT: cur_slot = e.value<0||e.value>=NS?0:e.value; mt_b_seen=1; break;
            case ABS_MT_TRACKING_ID: mt_b_seen=1;
                if(e.value<0){ if(sl[cur_slot].down){ sl[cur_slot].down=0; touch_up(cur_slot); } }
                else { sl[cur_slot].id=e.value; sl[cur_slot].down=1; pending_down[cur_slot]=1; } break;
            case ABS_MT_POSITION_X: sl[cur_slot].x=e.value; st_x=e.value; break;
            case ABS_MT_POSITION_Y: sl[cur_slot].y=e.value; st_y=e.value; break;
            case ABS_X: st_x=e.value; break;
            case ABS_Y: st_y=e.value; break; }
        } else if(e.type==EV_KEY && e.code==BTN_TOUCH && !mt_b_seen){   /* single-touch fallback */
            if(e.value){ sl[0].x=st_x; sl[0].y=st_y; sl[0].down=1; pending_down[0]=1; st_down=1; }
            else if(st_down){ st_down=0; sl[0].down=0; touch_up(0); }
        } else if(e.type==EV_SYN && e.code==SYN_REPORT){
            if(!mt_b_seen && st_down){ sl[0].x=st_x; sl[0].y=st_y; }
            for(int i=0;i<NS;i++){
                if(pending_down[i]){ pending_down[i]=0; touch_down(i); }
                if(sl[i].down) touch_move(i); }
        }
    }
}

/* ---------------- physical keys ---------------- */
static int landscape_mode(void);
static uint32_t pwr_t; static int pwr_down, weapon=1;
static void poll_keys(void){
    struct input_event e;
    while(kp_fd>=0 && read(kp_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY) continue;
        if(e.code==116){ if(e.value==1){ pwr_down=1; pwr_t=now_ms(); }
            else if(e.value==0 && pwr_down){ pwr_down=0; if(now_ms()-pwr_t<800) tapkey(KEY_ESCAPE); } }
        else if(e.code==165 && e.value!=2) key(KEY_TAB,e.value);          /* prev = automap too */
    }
    if(pwr_down && now_ms()-pwr_t>=1200){ fprintf(stderr,"[r3doom] long power: quit\n"); exit(0); }
    while(km_fd>=0 && read(km_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY || e.value==2) continue;
        if(e.code==164) key(KEY_USE,e.value);                              /* play = use */
        else if(e.code==163) key(KEY_TAB,e.value);                         /* next = automap */
        else if(e.value && (e.code==115||e.code==114) && landscape_mode()){       /* controller: keys = volume */
            int v=160; FILE*f=fopen("/usr/data/r3_vol","r"); if(f){ if(fscanf(f,"%d",&v)!=1) v=160; fclose(f); }
            v+= e.code==115?12:-12; if(v<0)v=0; if(v>255)v=255;
            f=fopen("/usr/data/r3_vol","w"); if(f){ fprintf(f,"%d\n",v); fclose(f); } }   /* r3snd applies it */
        else if(e.value==1 && (e.code==115||e.code==114)){                 /* touch mode: vol = weapons */
            weapon += e.code==115?1:-1; if(weapon>7)weapon=1; if(weapon<1)weapon=7;
            tapkey((unsigned char)('0'+weapon)); }
    }
}

/* ---------------- doomgeneric hooks ---------------- */
void DG_Init(){
    t0=now_ms();
    int fd=open("/dev/fb0",O_RDWR); struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fd,FBIOGET_VSCREENINFO,&v); ioctl(fd,FBIOGET_FSCREENINFO,&f);
    v.xoffset=0; v.yoffset=0; ioctl(fd,FBIOPAN_DISPLAY,&v);
    fb=mmap(0,f.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); fbstride=f.line_length/2;
    ts_fd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
    kp_fd=open("/dev/input/event0",O_RDONLY|O_NONBLOCK);
    km_fd=open("/dev/input/event2",O_RDONLY|O_NONBLOCK);
    knob_x=stick_cx; knob_y=stick_cy;
    draw_deck(knob_x,knob_y,0); deck_dirty=0;
}
/* ---------------- game controller (Joy-Con...): landscape, no touch deck ----------------
 * Held sideways: stick = forward/back/turn, A fire (+Enter), B use, X next weapon,
 * Y run on/off, SL/SR strafe, - menu, Capture automap. Run is on by default. */
static int landscape, run_on=1, pad_weapon=1;
static int pst[PA_N];                         /* last state sent per action */
static void pad_to_keys(void){
    static const int K[PA_N]={KEY_UPARROW,KEY_DOWNARROW,KEY_LEFTARROW,KEY_RIGHTARROW,
        KEY_FIRE,KEY_USE,0,0, KEY_STRAFE_L,KEY_STRAFE_R, KEY_ESCAPE,KEY_ESCAPE,KEY_TAB};
    for(int a=0;a<PA_N;a++){ int h=pad_held(a); if(h==pst[a]) continue; pst[a]=h;
        if(a==PA_A){ key(KEY_FIRE,h); key(KEY_ENTER,h); continue; }
        if(a==PA_X){ if(h){ pad_weapon=pad_weapon%7+1; tapkey((unsigned char)('0'+pad_weapon)); } continue; }
        if(a==PA_Y){ if(h){ run_on=!run_on; key(KEY_RSHIFT,run_on); } continue; }
        if(K[a]) key((unsigned char)K[a],h); }
    int a; while(pad_next(&a)){}             /* presses are handled as held states above */
}
static void set_landscape(int on){
    if(on==landscape) return; landscape=on;
    for(int y=0;y<SH;y++) memset(fb+y*fbstride,0,SW*2);
    for(int a=0;a<PA_N;a++){ if(pst[a]){ pst[a]=0; } }
    if(on) key(KEY_RSHIFT,run_on);
    else { key(KEY_RSHIFT,0); knob_x=stick_cx; knob_y=stick_cy; deck_dirty=1; }
}
/* portrait: 320x200 -> 480x300 at the top (x1.5). landscape (R3 turned clockwise, top
 * to the right): game top along the physical left edge, 320x200 -> 720x450 (x2.25). */
void DG_DrawFrame(){
    static uint16_t lut[256]; static short xm[SW], ym[GH], lx[SH], ly[SW]; static int init;
    if(!init){ for(int x=0;x<SW;x++) xm[x]=(short)(x*2/3); for(int y=0;y<GH;y++) ym[y]=(short)(y*2/3);
        for(int py=0;py<SH;py++) lx[py]=(short)((SH-1-py)*320/SH);              /* game x per physical row */
        for(int px=0;px<SW;px++){ int g=(px-15)*200/450; ly[px]=(short)(px<15||px>=465?-1:(g>199?199:g)); }
        init=1; }
    pad_poll(); set_landscape(pad_count()>0);
    for(int i=0;i<256;i++) lut[i]=rgb(colors[i].r,colors[i].g,colors[i].b);
    const unsigned char*src=(const unsigned char*)DG_ScreenBuffer;
    if(landscape){
        for(int py=0;py<SH;py++){ const unsigned char*s=src+lx[py]; uint16_t*d=fb+py*fbstride;
            for(int px=15;px<465;px++) d[px]=lut[s[ly[px]*320]]; }
        pad_to_keys(); poll_keys();
        { struct input_event e; while(read(ts_fd,&e,sizeof e)==sizeof e){} }   /* touch off */
        return; }
    for(int y=0;y<GH;y++){ const unsigned char*s=src+ym[y]*320; uint16_t*d=fb+y*fbstride;
        for(int x=0;x<SW;x++) d[x]=lut[s[xm[x]]]; }
    poll_touch(); poll_keys();
    if(deck_dirty){ draw_deck(knob_x,knob_y,fire_held); deck_dirty=0; }
}
void DG_SleepMs(uint32_t ms){ usleep(ms*1000); }
uint32_t DG_GetTicksMs(){ return now_ms()-t0; }
void DG_SetWindowTitle(const char*t){ (void)t; }

int main(int argc,char**argv){
    doomgeneric_Create(argc,argv);
    for(;;){ doomgeneric_Tick(); if(!landscape) poll_touch(); poll_keys(); }
    return 0;
}
static int landscape_mode(void){ return landscape; }
