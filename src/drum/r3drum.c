/* r3drum — Drumfolk, a rhythm-marching battle game for the HiBy R3 Pro II.
 *
 * Drum four-beat commands (DUM KA TOK CHI) in time with the beat; your squad of hooded drum-folk
 * performs them on the next four beats. Keep the streak for FEVER. Pick the squad (5 classes,
 * weapons and shields unlocked by winning) at the camp to suit each mission's foes and bosses.
 *
 * Rules and data: drum_core.c (rhythm) and drum_game.c (battle), both host-tested.
 * This file: screen (framebuffer, drawn in RAM then copied), input (touch pads, R3 keys,
 * Joy-Con via pad.c), audio (own thread, synthesized, the beat comes from the mixer itself so it
 * is sample-exact; hits are timed with evdev monotonic timestamps mapped onto the DAC clock),
 * camp UI, saves (/usr/data/r3drum.sav).
 * Keys: prev = DUM, next = KA, play = TOK, volume +/-; power = pause, hold power = back to camp
 * (at the camp: quit). Joy-Con: A DUM, B KA, X TOK, Y CHI. */
#include "drum_core.h"
#include "drum_game.h"
#include "drum_music.h"
#include "drum_decode.h"
#include <dirent.h>
#include "pad.h"
#include "volbar.h"
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>

#define SW 480
#define SH 720
#define TOP 24                         /* the bezel hides the first rows of the panel */
#define SAVE "/usr/data/r3drum.sav"
static uint16_t cvbuf[SW*SH];          /* the frame, drawn in RAM: CW x CH (portrait 480x720, landscape 720x480) */
static int CW=SW, CH=SH, land;         /* landscape: R3 turned clockwise, a controller in hand */
#define CV(x,y) cvbuf[(y)*CW+(x)]
static int PF_Y=TOP, PLAY_BOT=TOP+420, UW=SW;   /* playfield top / bottom, usable width (landscape: the bezel hides the right edge) */
static int GROUND=392;                  /* ground line on screen */
static uint16_t *fb; static int fbstride;
static int64_t now_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
uint32_t r3_tick_ms(void){ return (uint32_t)(now_us()/1000); }   /* for pad.c */

/* ======================================================================= drawing */
static inline uint16_t rgb(uint32_t c){ return (uint16_t)(((c>>8)&0xF800)|((c>>5)&0x07E0)|((c>>3)&0x1F)); }
static uint32_t lerpc(uint32_t a,uint32_t b,float t){ if(t<0)t=0; if(t>1)t=1;
    int r=(int)(((a>>16)&255)*(1-t)+((b>>16)&255)*t), g=(int)(((a>>8)&255)*(1-t)+((b>>8)&255)*t), bl=(int)((a&255)*(1-t)+(b&255)*t);
    return (uint32_t)(r<<16|g<<8|bl); }
static uint32_t shade(uint32_t c,float k){ return k<1 ? lerpc(0,c,k) : lerpc(c,0xFFFFFF,k-1); }
static inline void px(int x,int y,uint16_t c){ if((unsigned)x<CW && (unsigned)y<CH) CV(x,y)=c; }
static void rect(int x,int y,int w,int h,uint32_t c){ uint16_t k=rgb(c);
    if(x<0){ w+=x; x=0; } if(y<0){ h+=y; y=0; } if(x+w>CW) w=CW-x; if(y+h>CH) h=CH-y;
    for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) CV(i,j)=k; }
static void dim(int x,int y,int w,int h){ if(x<0){ w+=x; x=0; } if(y<0){ h+=y; y=0; } if(x+w>CW) w=CW-x; if(y+h>CH) h=CH-y;
    for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) CV(i,j)=(uint16_t)((CV(i,j)>>1)&0x7BEF); }
static int isqrt(int64_t v){ if(v<=0) return 0; int64_t r=(int64_t)1<<30; while(r>v) r>>=2; int64_t x=0;   /* no FPU here */
    while(r){ if(v>=x+r){ v-=x+r; x=(x>>1)+r; } else x>>=1; r>>=2; } return (int)x; }
static void ellipse(int cx,int cy,int rx,int ry,uint32_t c){ if(rx<1)rx=1; if(ry<1)ry=1; uint16_t k=rgb(c);
    for(int y=-ry;y<=ry;y++){ int w=isqrt((int64_t)rx*rx*((int64_t)ry*ry-(int64_t)y*y)/((int64_t)ry*ry));
        int yy=cy+y; if((unsigned)yy>=CH) continue; int x0=cx-w<0?0:cx-w, x1=cx+w>=CW?CW-1:cx+w;
        for(int x=x0;x<=x1;x++) CV(x,yy)=k; } }
static void circle(int cx,int cy,int r,uint32_t c){ ellipse(cx,cy,r,r,c); }
static void ring(int cx,int cy,int r,int t,uint32_t c){ uint16_t k=rgb(c);
    for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++){ int d=x*x+y*y; if(d<=r*r && d>=(r-t)*(r-t)) px(cx+x,cy+y,k); } }
static void line(int x0,int y0,int x1,int y1,int w,uint32_t c){ uint16_t k=rgb(c);
    int dx=abs(x1-x0), dy=abs(y1-y0), n=dx>dy?dx:dy; if(n==0) n=1; int h=w/2;
    for(int i=0;i<=n;i++){ int x=x0+(x1-x0)*i/n, y=y0+(y1-y0)*i/n;
        for(int a=-h;a<=w-1-h;a++) for(int b=-h;b<=w-1-h;b++) px(x+a,y+b,k); } }
static void tri(int x0,int y0,int x1,int y1,int x2,int y2,uint32_t c){ uint16_t k=rgb(c);
    int miny=y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2), maxy=y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2);
    if(miny<0)miny=0; if(maxy>=CH)maxy=CH-1;
    int X[3]={x0,x1,x2},Y[3]={y0,y1,y2};
    for(int y=miny;y<=maxy;y++){ int xs[3]; int n=0;
        for(int e=0;e<3;e++){ int a=e,b=(e+1)%3; if((Y[a]<=y&&Y[b]>y)||(Y[b]<=y&&Y[a]>y)) xs[n++]=X[a]+(y-Y[a])*(X[b]-X[a])/(Y[b]-Y[a]); }
        if(n<2) continue; int l=xs[0]<xs[1]?xs[0]:xs[1], r=xs[0]<xs[1]?xs[1]:xs[0]; if(l<0) l=0; if(r>=CW) r=CW-1;
        for(int x=l;x<=r;x++) CV(x,y)=k; } }
static void frame(int x,int y,int w,int h,int t,uint32_t c){ rect(x,y,w,t,c); rect(x,y+h-t,w,t,c); rect(x,y,t,h,c); rect(x+w-t,y,t,h,c); }
static void rbox(int x,int y,int w,int h,int r,uint32_t c){
    rect(x+r,y,w-2*r,h,c); rect(x,y+r,r,h-2*r,c); rect(x+w-r,y+r,r,h-2*r,c);
    circle(x+r,y+r,r,c); circle(x+w-r-1,y+r,r,c); circle(x+r,y+h-r-1,r,c); circle(x+w-r-1,y+h-r-1,r,c); }

/* 5x7 font, ASCII 32..95 (lower case drawn as upper case) */
static const uint8_t FONT[64][7]={
 {0,0,0,0,0,0,0},{4,4,4,4,4,0,4},{10,10,0,0,0,0,0},{10,31,10,10,31,10,0},{4,15,20,14,5,30,4},{24,25,2,4,8,19,3},{12,18,20,8,21,18,13},{4,4,0,0,0,0,0},
 {2,4,8,8,8,4,2},{8,4,2,2,2,4,8},{0,4,21,14,21,4,0},{0,4,4,31,4,4,0},{0,0,0,0,0,4,8},{0,0,0,31,0,0,0},{0,0,0,0,0,0,4},{1,1,2,4,8,16,16},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
 {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},{0,4,0,0,0,4,0},{0,4,0,0,0,4,8},{2,4,8,16,8,4,2},{0,0,31,0,31,0,0},{8,4,2,1,2,4,8},{14,17,1,2,4,0,4},
 {14,17,1,13,21,21,14},{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{28,18,17,17,17,18,28},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},
 {17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
 {17,17,10,4,10,17,17},{17,17,17,10,4,4,4},{31,1,2,4,8,16,31},{14,8,8,8,8,8,14},{16,16,8,4,2,1,1},{14,2,2,2,2,2,14},{4,10,17,0,0,0,0},{0,0,0,0,0,0,31}};
static int text_w(const char*s,int sc){ return (int)strlen(s)*6*sc-sc; }
static void text(int x,int y,const char*s,int sc,uint32_t c){ uint16_t k=rgb(c);
    for(;*s;s++,x+=6*sc){ int ch=(unsigned char)*s; if(ch>='a'&&ch<='z') ch-=32; if(ch<32||ch>95) ch='?';
        const uint8_t*g=FONT[ch-32];
        for(int r=0;r<7;r++) for(int b=0;b<5;b++) if(g[r]&(16>>b))
            for(int i=0;i<sc;i++) for(int j=0;j<sc;j++) px(x+b*sc+j,y+r*sc+i,k); } }
static void textc(int cx,int y,const char*s,int sc,uint32_t c){ text(cx-text_w(s,sc)/2,y,s,sc,c); }
static void texts(int x,int y,const char*s,int sc,uint32_t c){ text(x+sc,y+sc,s,sc,0x000000); text(x,y,s,sc,c); }   /* shadowed */
static void textcs(int cx,int y,const char*s,int sc,uint32_t c){ texts(cx-text_w(s,sc)/2,y,s,sc,c); }
static void set_mode(int l){
    land=l; if(l){ CW=720; CH=480; PF_Y=0; GROUND=372; PLAY_BOT=420; UW=CW-28; } else { CW=SW; CH=SH; PF_Y=TOP; GROUND=392; PLAY_BOT=TOP+420; UW=SW; }
    memset(cvbuf,0,sizeof cvbuf); if(fb) for(int y=0;y<SH;y++) memset(fb+y*fbstride,0,SW*2);
    fprintf(stderr,"[r3drum] %s\n",l?"landscape (controller)":"portrait"); }
static void present(void){
    if(!land){ for(int y=TOP;y<SH;y++) memcpy(fb+y*fbstride,&cvbuf[y*SW],SW*2); return; }
    /* landscape, R3 turned clockwise: viewer (vx,vy) -> panel (x=vy, y=719-vx); 8 panel rows at a time
     * so the canvas is read along its rows (the transpose stays in cache) */
    static uint16_t rows[8][SW];
    for(int py0=0;py0<SH;py0+=8){
        for(int px=0;px<SW;px++){ const uint16_t*src=&cvbuf[px*720+(SH-1-py0-7)];
            for(int k=0;k<8;k++) rows[7-k][px]=src[k]; }
        for(int k=0;k<8;k++) memcpy(fb+(py0+k)*fbstride,rows[k],SW*2); }
}

/* ======================================================================= audio */
enum { S_DUM, S_KA, S_TOK, S_CHI, S_KICK, S_HAT, S_CLAP, S_BASS, S_CHANT, S_HIT, S_KILL, S_HURT, S_WARN,
       S_ROAR, S_QUAKE, S_HEAL, S_TONE, S_MISS };
typedef struct { int type; int64_t start; float vol, pitch; } sev_t;
/* The X1600 has no FPU (the kernel emulates every float instruction): the synth is fixed-point.
 * Phases are 32-bit accumulators into a 1024-entry sine table, envelopes Q15 with a per-sample
 * multiplier; floats are only used once per note to set coefficients. */
typedef struct { int on, type; int64_t start; int n, att, dur;
                 uint32_t ph, ph2, step, sweep; int32_t env, edec, senv, sdec, cenv, cdec, vol, lp, lp2; uint32_t ns; } voice_t;
#define NV 32
static voice_t V[NV];
static sev_t Q[512]; static volatile unsigned qh, qt;          /* main thread -> mixer */
static pthread_mutex_t clk_mu=PTHREAD_MUTEX_INITIALIZER;
static int64_t written, anchor_played, anchor_us;              /* sample clock */
static volatile int bk_on, bk_fever; static volatile int64_t bk_t0, bk_beat;   /* backing beat */
static volatile int audio_run=1; static struct pcm*pcm; static struct mixer*mx; static int vol=160;
static void snd_at(int type,int64_t start,float v,float pitch){ unsigned h=qh; if(h-qt>=512) return;
    Q[h&511]=(sev_t){type,start,v,pitch}; __sync_synchronize(); qh=h+1; }
static int64_t now_sample(void){ pthread_mutex_lock(&clk_mu);
    int64_t s=anchor_played+(now_us()-anchor_us)*DR_RATE/1000000, w=written; pthread_mutex_unlock(&clk_mu); return s<w?s:w; }
static void snd(int type,float v,float pitch){ snd_at(type,0,v,pitch); }   /* 0 = as soon as possible */
static int16_t SINT[1024];
static void synth_init(void){ for(int i=0;i<1024;i++) SINT[i]=(int16_t)(32767*sinf(i*6.2831853f/1024)); }
static uint32_t hz(float f){ return (uint32_t)(f*(4294967296.0f/DR_RATE)); }
static int32_t dec(float k){ return (int32_t)(32768.0f*expf(-k/DR_RATE)); }            /* per-sample decay, Q15 */
static inline int32_t SINP(uint32_t ph){ return SINT[ph>>22]; }
static inline int32_t noise(voice_t*v){ v->ns=v->ns*1664525u+1013904223u; return (int32_t)(v->ns>>16)-32768; }
static void start_voice(int type,int64_t at,float vol,float p){
    int k=0; for(int i=0;i<NV;i++){ if(!V[i].on){ k=i; break; } if(V[i].start<V[k].start) k=i; }
    voice_t*v=&V[k]; memset(v,0,sizeof *v);
    v->on=1; v->type=type; v->start=at; v->env=32767; v->senv=32767; v->cenv=32767; v->ns=0x1234567u+(uint32_t)at;
    float vv=vol, ek=10, sk=0, ck=0;
    switch(type){
    case S_DUM:  v->step=hz(58);  v->sweep=hz(95);  sk=18; ek=8;  ck=150; break;
    case S_KA:   v->step=hz(200); v->sweep=hz(170); sk=28; ek=13; ck=200; vv*=0.9f; break;
    case S_TOK:  v->step=hz(880); ek=42; break;
    case S_CHI:  ek=12; vv*=0.7f; break;
    case S_KICK: v->step=hz(44);  v->sweep=hz(70);  sk=30; ek=11; vv*=0.8f; break;
    case S_HAT:  ek=55; vv*=0.22f; break;
    case S_CLAP: ek=22; vv*=0.45f; break;
    case S_BASS: v->step=hz(p); ek=4; v->att=DR_RATE/200; v->dur=DR_RATE*42/100; vv*=0.55f; break;
    case S_CHANT:v->step=hz(p); sk=3; ek=11; v->att=DR_RATE*12/1000; vv*=0.5f; break;
    case S_HIT:  ek=30; vv*=0.8f; break;
    case S_KILL: v->step=hz(120); v->sweep=hz(620); sk=5; ek=6; vv*=0.45f; break;
    case S_HURT: v->step=hz(95); ek=12; vv*=0.5f; break;
    case S_WARN: v->step=hz(440); ek=0; v->dur=DR_RATE*6/10; vv*=0.35f; break;
    case S_ROAR: v->step=hz(60); ek=2.2f; v->att=DR_RATE*15/100; vv*=0.9f; break;
    case S_QUAKE:ek=2.8f; break;
    case S_HEAL: ek=5; vv*=0.3f; break;
    case S_TONE: v->step=hz(p); ek=3.5f; v->att=DR_RATE/100; vv*=0.4f; break;
    case S_MISS: v->step=hz(30); v->sweep=hz(150); sk=4; ek=14; vv*=0.35f; break;
    }
    v->edec=ek>0?dec(ek):32768; v->sdec=sk>0?dec(sk):32768; v->cdec=ck>0?dec(ck):0;
    v->vol=(int32_t)(vv*32767);
}
/* one sample of a voice, Q15-ish int; advances its state */
static int32_t voice_sample(voice_t*v){
    int32_t s=0; uint32_t st;
    switch(v->type){
    case S_DUM: case S_KA: case S_KICK:
        st=v->step+(uint32_t)(((int64_t)v->sweep*v->senv)>>15); v->ph+=st; s=SINP(v->ph);
        if(v->cdec) s+=(int32_t)(((int64_t)noise(v)*v->cenv)>>16);
        break;
    case S_TOK:  v->ph+=v->step; v->ph2+=v->step/10*27; s=(SINP(v->ph)*2+SINP(v->ph2))/3; break;
    case S_CHI: case S_HAT: { int32_t n=noise(v); s=n-v->lp; v->lp=n; s/=2; } break;
    case S_CLAP: { int32_t n=noise(v); v->lp+=(n-v->lp)*2/5; s=n-v->lp; if(v->n>DR_RATE/100 && v->n<DR_RATE/50) s=s*2/5; } break;
    case S_BASS: { v->ph+=v->step; int32_t x=(int32_t)(v->ph>>16)-32768; s=(x<0?-x:x)*2-32768; } break;
    case S_CHANT: st=(uint32_t)(((int64_t)v->step*(26870+(((int64_t)5898*v->senv)>>15)))>>15); v->ph+=st;
        s=(SINP(v->ph)*10+SINP(v->ph*3)*5+SINP(v->ph*5)*2)/17; break;
    case S_HIT:  { int32_t n=noise(v); v->lp+=(n-v->lp)*3/20; s=v->lp*2; } break;
    case S_KILL: case S_MISS:
        st=v->step+(uint32_t)(((int64_t)v->sweep*v->senv)>>15); v->ph+=st;
        s = v->type==S_KILL ? SINP(v->ph) : (int32_t)(v->ph>>16)-32768; break;
    case S_HURT: v->ph+=v->step; s=(v->ph>>31)?-20000:20000; break;
    case S_WARN: { int frac=(int)((int64_t)(v->n*3)%DR_RATE*1024/DR_RATE); v->ph+=v->step+(uint32_t)(((uint64_t)v->step*frac)>>10); s=SINP(v->ph); } break;
    case S_ROAR: { int32_t n=noise(v); v->lp+=(n-v->lp)/20; v->ph+=v->step; s=v->lp*3+((int32_t)(v->ph>>16)-32768)/2; } break;
    case S_QUAKE:{ int32_t n=noise(v); v->lp+=(n-v->lp)/50; s=v->lp*7; } break;
    case S_HEAL: { uint32_t f= v->n<DR_RATE*8/100?hz(660): v->n<DR_RATE*16/100?hz(880):hz(1100); v->ph+=f; s=SINP(v->ph); } break;
    case S_TONE: v->ph+=v->step; v->ph2+=v->step*2; s=(SINP(v->ph)*10+SINP(v->ph2)*3)/13; break;
    }
    int32_t a=v->env; if(v->att && v->n<v->att) a=(int32_t)((int64_t)a*v->n/v->att);
    s=(int32_t)(((int64_t)s*a)>>15);
    if(!v->att || v->n>=v->att) v->env=(int32_t)(((int64_t)v->env*v->edec)>>15);
    v->senv=(int32_t)(((int64_t)v->senv*v->sdec)>>15); if(v->cdec) v->cenv=(int32_t)(((int64_t)v->cenv*v->cdec)>>15);
    v->n++;
    if((v->dur && v->n>=v->dur) || v->n>DR_RATE*5/2 || (v->n>DR_RATE/20 && v->env<8)) v->on=0;
    return (int32_t)(((int64_t)s*v->vol)>>15);
}
static const float BASSLINE[8]={55,55,82.4f,73.4f,55,65.4f,73.4f,82.4f};
static volatile int32_t soft_gain=32768;   /* Bluetooth: software volume, like r3player (0.25 dB a step) */
/* MY MUSIC: a decoder thread fills this ring at 48 kHz; the mixer reads frame (s - song_w0) for
 * mixer sample s, so a song frame always plays at a known sample: the beat grid stays exact */
#define RING (48000*2)
static int16_t mring[RING*2]; static volatile int64_t ring_w; static volatile int64_t song_w0=-1; static volatile int music_live;
static void mix_block(int16_t*out,int n){
    int64_t w0=written;
    while(qt!=qh){ sev_t e=Q[qt&511]; start_voice(e.type,e.start?e.start:w0,e.vol,e.pitch); __sync_synchronize(); qt++; }
    if(bk_on){ int64_t beat=bk_beat, t0=bk_t0;    /* the beat itself: generated here, sample-exact */
        int64_t h=beat/2;
        for(int64_t b=(w0-t0)/h-1; t0+b*h<w0+n; b++){ int64_t at=t0+b*h; if(at<w0 || b<0) continue;
            int beatno=(int)(b/2);
            if(b%2==0){ start_voice(S_KICK,at,beatno%4==0?1.0f:0.75f,0);
                if(bk_fever){ start_voice(S_BASS,at,1,BASSLINE[beatno%8]); if(beatno%2==1) start_voice(S_CLAP,at,1,0); } }
            else start_voice(S_HAT,at,bk_fever?1.3f:1,0); } }
    int act[NV], na=0; for(int k=0;k<NV;k++) if(V[k].on) act[na++]=k;
    int64_t sw0=song_w0; int ml=music_live;
    for(int i=0;i<n;i++){ int32_t l=0; int64_t s=w0+i;
        if(ml && sw0>=0 && s>=sw0){ int64_t f=s-sw0; if(f<ring_w && f>ring_w-RING){ int k=(int)(f%RING); l+=(mring[2*k]+mring[2*k+1])*11/32; } }
        for(int j=0;j<na;j++){ voice_t*v=&V[act[j]]; if(!v->on || v->start>s) continue; l+=voice_sample(v); }
        l=l*13/32; if(soft_gain<32768) l=(int32_t)(((int64_t)l*soft_gain)>>15);
        if(l>30000) l=30000; if(l<-30000) l=-30000; out[2*i]=out[2*i+1]=(int16_t)l; }
}
static int PERIOD=1024, NPER=4;         /* DAC buffer (see open_dac) */
#define BLK 512                            /* mixed and written per step */
#define QMAX 1536                          /* frames allowed in the DAC queue before writing: ~32-43 ms */
/* Output follows r3player: the wired DAC, or the Bluetooth device it uses (aplay -D r3bt through a
 * small pipe, like audio.c). A DAC that takes nothing for 0.6 s (it stops clocking while a BT
 * output is active) is dropped: the game then runs on the clock alone instead of hanging. */
static int bt_fd=-1, bt_lat; static pid_t bt_pid=-1; static char out_name[48]="";
#define BT_CONF "/tmp/r3bt_alsa.conf"
static int bt_open(void){
    char mac[24]=""; FILE*f=fopen("/usr/data/r3_bt","r"); char l[64];
    if(f){ while(fgets(l,sizeof l,f)) if(!strncmp(l,"out=",4)){ snprintf(mac,sizeof mac,"%s",l+4); mac[strcspn(mac,"\r\n")]=0; } fclose(f); }
    if(!mac[0] || access(BT_CONF,R_OK)) return -1;
    int fds[2]; if(pipe(fds)) return -1;
    fcntl(fds[1],1031 /*F_SETPIPE_SZ*/,4096);                    /* ~21 ms of pipe, not 340 */
    char*argv[]={"aplay","-q","-D","r3bt","-t","raw","-f","S16_LE","-r","48000","-c","2","--buffer-time=100000",0};
    char*envp[]={"ALSA_CONFIG_PATH=" BT_CONF,"PATH=/usr/bin:/bin:/usr/sbin:/sbin",0};
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa,fds[0],0); posix_spawn_file_actions_addclose(&fa,fds[1]);
    posix_spawn_file_actions_addopen(&fa,1,"/tmp/r3drum_aplay.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    posix_spawn_file_actions_adddup2(&fa,1,2);
    int e=posix_spawn(&bt_pid,"/usr/bin/aplay",&fa,0,argv,envp);
    posix_spawn_file_actions_destroy(&fa); close(fds[0]);
    if(e){ close(fds[1]); bt_pid=-1; return -1; }
    bt_lat=DR_RATE*230/1000;                                       /* pipe + aplay + A2DP, estimated */
    snprintf(out_name,sizeof out_name,"BLUETOOTH %s",mac); fprintf(stderr,"[r3drum] audio -> %s (aplay %d)\n",out_name,(int)bt_pid);
    return fds[1];
}
static void dac_route(void); static void hw_volume(void);
static void open_dac(void){                 /* lowest latency first; the codec driver decides what it takes */
    /* never small periods: 256-frame periods are accepted but wedge the codec's DMA until a power
     * cycle (then nothing plays, r3player included). 1024x4 is what r3gb / r3gba use. */
    static const int TRY[][2]={{1024,4},{2048,8}};
    /* r3player stops its song when we start, but closes the DAC from its audio thread a moment
     * later: "busy" for a while is normal, so retry for up to 3 s */
    for(int attempt=0;attempt<30 && !pcm;attempt++){
        for(unsigned i=0;i<sizeof TRY/sizeof TRY[0] && !pcm;i++){
            struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
            cfg.channels=2; cfg.rate=DR_RATE; cfg.period_size=TRY[i][0]; cfg.period_count=TRY[i][1]; cfg.format=PCM_FORMAT_S16_LE;
            cfg.start_threshold=TRY[i][0]; cfg.stop_threshold=TRY[i][0]*TRY[i][1]; cfg.avail_min=TRY[i][0];
            pcm=pcm_open(0,0,PCM_OUT,&cfg);
            if(pcm && !pcm_is_ready(pcm)){ if(attempt%10==0) fprintf(stderr,"[r3drum] DAC %dx%d: %s\n",TRY[i][0],TRY[i][1],pcm_get_error(pcm));
                pcm_close(pcm); pcm=NULL; }
            else if(pcm){ PERIOD=TRY[i][0]; NPER=TRY[i][1]; fprintf(stderr,"[r3drum] audio %dx%d (%d ms buffer, ~%d ms queued)\n",PERIOD,NPER,PERIOD*NPER*1000/DR_RATE,QMAX*1000/DR_RATE); } }
        if(!pcm) usleep(100000); }
    if(pcm){ snprintf(out_name,sizeof out_name,"HEADPHONES"); dac_route(); hw_volume(); }
}
static void bt_close(void){ if(bt_fd>=0) close(bt_fd); bt_fd=-1; if(bt_pid>0){ kill(bt_pid,SIGTERM); waitpid(bt_pid,0,0); } bt_pid=-1; }
static void*audio_thread(void*arg){ (void)arg;
    static int16_t buf[1024*2];
    while(audio_run){
        mix_block(buf,BLK);
        int buffered=0;
        if(bt_fd>=0){ const char*q=(const char*)buf; size_t left=(size_t)PERIOD*4;
            left=(size_t)BLK*4;
            while(left){ ssize_t w=write(bt_fd,q,left); if(w<0){ if(errno==EINTR) continue;
                    fprintf(stderr,"[r3drum] bluetooth gone: wired output\n"); bt_close(); open_dac(); break; } q+=w; left-=(size_t)w; }
            buffered=bt_lat; }
        else if(pcm){
            /* low latency on a big buffer: write only when <= QMAX frames are still queued */
            int64_t wait0=now_us(); unsigned last_av=0; int64_t moved=now_us();
            for(;;){ unsigned avail=0; struct timespec ts;
                if(pcm_get_htimestamp(pcm,&avail,&ts)!=0) break;              /* not started yet */
                buffered=PERIOD*NPER-(int)avail; if(buffered<=QMAX) break;
                if(avail!=last_av){ last_av=avail; moved=now_us(); }
                if(now_us()-moved>600000){ fprintf(stderr,"[r3drum] DAC not consuming: sound off\n"); pcm_close(pcm); pcm=NULL; break; }
                if(!audio_run) break; usleep(1000); }
            (void)wait0;
            if(pcm){ if(pcm_write(pcm,buf,BLK*4)) pcm_prepare(pcm);
                unsigned avail=0; struct timespec ts;
                if(pcm_get_htimestamp(pcm,&avail,&ts)==0){ buffered=PERIOD*NPER-(int)avail; if(buffered<0) buffered=0; } } }
        else usleep(BLK*1000000/DR_RATE);
        pthread_mutex_lock(&clk_mu); written+=BLK; anchor_played=written-buffered; anchor_us=now_us(); pthread_mutex_unlock(&clk_mu);
    }
    return 0;
}
static void hw_volume(void){
    { int v=vol<0?0:vol>255?255:vol; soft_gain = bt_fd<0||v>=255 ? 32768 : v==0 ? 0 : (int32_t)(32768.0*pow(10.0,-(255-v)*0.25/20.0)); }
    if(!mx) return; int hw=255-(vol<0?0:vol>255?255:vol);
    struct mixer_ctl*l=mixer_get_ctl_by_name(mx,"Left Playback Volume"),*r=mixer_get_ctl_by_name(mx,"Right Playback Volume");
    if(l) mixer_ctl_set_value(l,0,hw); if(r) mixer_ctl_set_value(r,0,hw); }
/* route like r3player (audio.c mx_fixed): the 4.4 mm balanced jack is port 3 + "Balance Lineout En",
 * 3.5 mm headphones port 1. Forcing port 1 left the codec without a path on the balanced jack:
 * it never clocked ("DAC not consuming"). */
static int read_switch(const char*p){ FILE*f=fopen(p,"r"); int v=-1; if(f){ if(fscanf(f,"%d",&v)!=1) v=-1; fclose(f); } return v; }
static void dac_route(void){ if(!mx) return;
    struct mixer_ctl*mo=mixer_get_ctl_by_name(mx,"Mute Output"),*sm=mixer_get_ctl_by_name(mx,"Soft Mute"),*op=mixer_get_ctl_by_name(mx,"Output Port Switch"),
                    *be=mixer_get_ctl_by_name(mx,"Balance Lineout En");
    int bal=read_switch("/sys/class/switch/balance/state"), hs=read_switch("/sys/class/switch/headset/state");
    int port = bal==1 ? 3 : (hs==1 ? 1 : 3);
    if(mo) mixer_ctl_set_value(mo,0,0); if(sm) mixer_ctl_set_value(sm,0,0); if(op) mixer_ctl_set_value(op,0,port);
    if(be) mixer_ctl_set_value(be,0,bal==1?1:0);
    fprintf(stderr,"[r3drum] DAC route: port %d%s\n",port,bal==1?" (balanced 4.4 mm)":hs==1?" (3.5 mm)":""); }

/* ======================================================================= state */
enum { ST_CAMP, ST_EDIT, ST_BATTLE, ST_RESULT, ST_LISTEN, ST_TEST };
static int st=ST_CAMP, sel=0, slot=0, paused, quit;
enum { SRC_TOUCH, SRC_PAD, SRC_KEYS, SRC_N };
static int off_src[SRC_N]={30,30,30};      /* timing offset per input, ms (measured on the rhythm test) */
static const char*const SRC_NAME[SRC_N]={"TOUCH","JOY-CON","R3 KEYS"};
static armory_t A;
static uint32_t vb_until;
typedef struct { uint32_t magic, ver; armory_t a; int offset, sel, music, off3[3]; } save_t;   /* v1: no music, v2: one offset */
static int music_opt;
#define MAGIC 0x44524D46u
static void save_game(void){ save_t s={MAGIC,3,A,off_src[0],sel,music_opt,{off_src[0],off_src[1],off_src[2]}}; FILE*f=fopen(SAVE ".tmp","wb"); if(!f) return;
    int ok=fwrite(&s,sizeof s,1,f)==1; fflush(f); fsync(fileno(f)); fclose(f); if(ok) rename(SAVE ".tmp",SAVE); }
static void load_game(void){ dg_armory_default(&A); save_t s; FILE*f=fopen(SAVE,"rb");
    if(f){ memset(&s,0,sizeof s); size_t n=fread(&s,1,sizeof s,f);
        if(n>=offsetof(save_t,music) && s.magic==MAGIC && s.ver>=1 && s.ver<=3){ A=s.a; sel=s.sel; music_opt=s.ver>=2?s.music:0;
            for(int i=0;i<SRC_N;i++) off_src[i]= s.ver>=3 ? s.off3[i] : s.offset; }
        fclose(f); }
    if(A.nsquad<1||A.nsquad>DG_MAXU) dg_armory_default(&A);
    if(sel<0||sel>=DG_NMISSION||sel>A.cleared) sel=0; }
static int owned(int code){ return code>=100 ? (A.shields>>(code-100))&1 : (A.weapons>>code)&1; }
static const char*code_name(int code){ return code>=100 ? DG_SHIELD[code-100].name : DG_WEAPON[code].name; }

/* battle */
static rc_t rc; static dg_t G; static int64_t last_beat; static float shake, judge_t, cmd_t, end_t;
static int judge, shown_cmd; static float pad_t[4];
static int got[16], ngot, newly[24], nnew, victory;
typedef struct { float x,y,vx,vy,life,size; uint32_t c; int world; } part_t;
static part_t P[160];
typedef struct { int unit; const char*s; float t; } bubble_t;
static bubble_t BB[3]; static float say_cool;
static const char*banner; static float banner_t; static const char*banner2;
static void burst(float x,float y,int n,uint32_t c,float spd,float size,int world){
    for(int i=0;i<n;i++){ part_t*p=0; for(int k=0;k<160;k++) if(P[k].life<=0){ p=&P[k]; break; } if(!p) return;
        float a=(float)(rand()%628)/100, v=spd*(0.4f+(rand()%60)/100.0f);
        *p=(part_t){x,y,cosf(a)*v,sinf(a)*v-spd*0.5f,0.5f+(rand()%50)/100.0f,size,c,world}; } }

static int music_start(void); static void music_stop(void); static int listen_fallback; static float listen_msg_t;
static void start_battle(void){
    if(music_opt && st!=ST_LISTEN && !listen_fallback){ if(music_start()){ st=ST_LISTEN; listen_msg_t=0; return; } }   /* listen first, then fight */
    dg_start(&G,sel,&A,(uint32_t)now_us());
    int64_t t=now_sample()+DR_RATE;                /* first beat in a second */
    rc_init(&rc,120,t); last_beat=-1;
    bk_t0=t; bk_beat=rc.beat; bk_fever=0; bk_on=!music_live;
    ngot=0; nnew=0; shake=0; judge_t=0; cmd_t=0; end_t=0; shown_cmd=CMD_NONE; victory=0;
    memset(P,0,sizeof P); memset(BB,0,sizeof BB); say_cool=3; st=ST_BATTLE; paused=0;
}
static void unlock(int code){ if(owned(code)) return;
    if(code>=100) A.shields|=1u<<(code-100); else A.weapons|=1u<<code;
    if(nnew<24) newly[nnew++]=code; }
static void end_battle(int won){
    victory=won; bk_on=0; music_stop();
    for(int i=0;i<ngot;i++) unlock(got[i]);                       /* finds are kept either way */
    if(won){ const mission_t*m=&DG_MISSION[sel]; for(int i=0;i<m->nreward;i++) unlock(m->reward[i]);
        if(sel==A.cleared && A.cleared<DG_NMISSION) A.cleared++; }
    save_game();
    if(won){ const float mel[6]={523,659,784,1047,784,1047}; for(int i=0;i<6;i++) snd_at(S_TONE,now_sample()+DR_RATE/8*i+800,1,mel[i]); }
    else { const float mel[4]={392,330,262,196}; for(int i=0;i<4;i++) snd_at(S_TONE,now_sample()+DR_RATE/5*i+800,1,mel[i]); }
    st=ST_RESULT;
}

/* ======================================================================= MY MUSIC */
static char song_path[640], song_title[64];
static dm_t DM; static dm_beat_t BEAT; static volatile int beat_ver, an_frames, an_rate, an_end, pb_rate;
static volatile int an_quit, pb_quit; static volatile int64_t pass_frame;   /* ring frame where the song (re)started */
static pthread_t an_th, pb_th; static int an_run, pb_run, music_failed;
static pthread_mutex_t beat_mu=PTHREAD_MUTEX_INITIALIZER;
#define SDR "/usr/data/mnt/sd_0"
static int pick_from_dir(const char*dir,int depth){       /* random walk: artist -> album -> track */
    static char names[512][256]; static char songs_[64][256]; int nd=0, ns=0;
    DIR*d=opendir(dir); if(!d) return 0; struct dirent*e;
    while((e=readdir(d))){ if(e->d_name[0]=='.') continue;
        if(e->d_type==DT_DIR && nd<512) snprintf(names[nd++],256,"%s",e->d_name);
        else if(dec_is_song(e->d_name) && ns<64) snprintf(songs_[ns++],256,"%s",e->d_name); }
    closedir(d);
    if(ns){ snprintf(song_path,sizeof song_path,"%s/%s",dir,songs_[rand()%ns]); return 1; }
    if(!nd || depth>4) return 0;
    char sub[640]; snprintf(sub,sizeof sub,"%s/%s",dir,names[rand()%nd]); return pick_from_dir(sub,depth+1);
}
static int pick_song(void){
    FILE*f=fopen(SDR "/Playlists/Favorites.m3u","r"); static char c[200][640]; int n=0; char l[700];
    if(f){ while(fgets(l,sizeof l,f) && n<200){ l[strcspn(l,"\r\n")]=0; if(!l[0]||l[0]=='#'||!dec_is_song(l)) continue;
            if(l[0]=='/') snprintf(c[n],640,"%s",l); else snprintf(c[n],640,SDR "/Playlists/%s",l);
            if(access(c[n],R_OK)){ char*m=strstr(l,"Music/"); if(m) snprintf(c[n],640,SDR "/%s",m); }
            if(!access(c[n],R_OK)) n++; }
        fclose(f); }
    if(getenv("R3DRUM_SONG")){ snprintf(song_path,sizeof song_path,"%s",getenv("R3DRUM_SONG")); n=1; }   /* testing */
    else if(n) snprintf(song_path,sizeof song_path,"%s",c[rand()%n]);
    else for(int t=0;t<6;t++) if(pick_from_dir(SDR "/Music",0)){ n=1; break; }
    if(!n) return 0;
    /* "ARTIST - TITLE" in the 5x7 font's alphabet */
    char a[200]="", t[200]; const char*b=strrchr(song_path,'/'); snprintf(t,sizeof t,"%s",b?b+1:song_path);
    char*x=strrchr(t,'.'); if(x) *x=0;
    { char tmp[640]; snprintf(tmp,sizeof tmp,"%s",song_path); char*s1=strrchr(tmp,'/');
      if(s1){ *s1=0; char*s2=strrchr(tmp,'/'); if(s2){ *s2=0; char*s3=strrchr(tmp,'/'); snprintf(a,sizeof a,"%s",s3?s3+1:tmp); } } }
    char raw[420]; snprintf(raw,sizeof raw,"%s - %s",a,t); int j=0;
    for(int i=0;raw[i]&&j<(int)sizeof song_title-1;i++){ unsigned char ch=(unsigned char)raw[i];
        if(ch>=0x80){ if((ch&0xC0)!=0x80) song_title[j++]='?'; } else song_title[j++]=(char)ch; }
    song_title[j]=0; return 1;
}
static void*an_thread(void*arg){ (void)arg;                /* listens to the whole song, ahead of play */
    dec_t d; if(dec_open(&d,song_path)){ an_end=1; return 0; }
    dm_init(&DM,d.rate); an_rate=d.rate; static int16_t st[4096*2], mono[4096]; int last=0;
    while(!an_quit){ int n=dec_read(&d,st,4096); if(n<=0) break;
        for(int i=0;i<n;i++) mono[i]=(int16_t)((st[2*i]+st[2*i+1])/2);
        dm_feed(&DM,mono,n); an_frames+=n;
        int secs=an_frames/d.rate;
        if((secs>=20 && last==0) || secs>=last+20){ last=secs; dm_beat_t b=dm_estimate(&DM);
            pthread_mutex_lock(&beat_mu); BEAT=b; beat_ver++; pthread_mutex_unlock(&beat_mu); }
        if(secs>=20) usleep(15000);                          /* the first 20 s fast, then gently */
    }
    if(!an_quit){ dm_beat_t b=dm_estimate(&DM); pthread_mutex_lock(&beat_mu); if(b.ok||!BEAT.ok) BEAT=b; beat_ver++; pthread_mutex_unlock(&beat_mu); }
    dec_close(&d); an_end=1; return 0;
}
static void*pb_thread(void*arg){ (void)arg;                /* plays it into the ring, resampled to 48 kHz */
    dec_t d; static int16_t st[4096*2]; int n=0, pos=0; uint32_t frac=0; int16_t pl=0,pr=0;
    while(!pb_quit){
        if(dec_open(&d,song_path)) break;
        pb_rate=d.rate; uint32_t step=(uint32_t)(((uint64_t)d.rate<<16)/DR_RATE); n=pos=0; frac=0;
        pass_frame=ring_w; __sync_synchronize(); beat_ver++;   /* the song (re)starts here: re-anchor */
        for(;;){ if(pb_quit) break;
            if(pos>=n){ n=dec_read(&d,st,4096); pos=0; if(n<=0) break; }
            for(;;){ int64_t played = song_w0>=0 && written>song_w0 ? written-song_w0 : 0;
                if(ring_w-played<RING-2048 || pb_quit) break; usleep(5000); }
            int k=(int)(ring_w%RING); int l=st[2*pos], r=st[2*pos+1];
            mring[2*k]=(int16_t)(pl+(((l-pl)*(int32_t)(frac>>4))>>12)); mring[2*k+1]=(int16_t)(pr+(((r-pr)*(int32_t)(frac>>4))>>12));
            __sync_synchronize(); ring_w++;
            frac+=step;
            while(frac>=65536){ frac-=65536; pl=st[2*pos]; pr=st[2*pos+1]; pos++;
                if(pos>=n){ n=dec_read(&d,st,4096); pos=0; if(n<=0) break; } }
            if(n<=0) break; }
        dec_close(&d);
    }
    return 0;
}
static void music_stop(void){
    music_live=0; an_quit=pb_quit=1;
    if(an_run){ pthread_join(an_th,0); an_run=0; } if(pb_run){ pthread_join(pb_th,0); pb_run=0; }
    song_w0=-1; ring_w=0;
}
static int music_start(void){                              /* -> listening; 0 if no song */
    music_stop(); music_failed=0;
    if(!pick_song()){ music_failed=1; return 0; }
    an_quit=pb_quit=0; an_frames=0; an_end=0; beat_ver=0; memset(&BEAT,0,sizeof BEAT);
    pthread_create(&an_th,0,an_thread,0); an_run=1;
    fprintf(stderr,"[r3drum] my music: %s\n",song_path);
    return 1;
}
/* the song's beat grid in mixer samples, around time T: beat length and the time of a beat near T */
static int song_grid(int64_t T,int64_t*beat,int64_t*tk){
    dm_beat_t b; pthread_mutex_lock(&beat_mu); b=BEAT; pthread_mutex_unlock(&beat_mu);
    if(!b.ok || an_rate<=0 || song_w0<0) return 0;
    int64_t pass_w0=song_w0+pass_frame;                    /* mixer sample where this pass began */
    double per48=b.period_x100/100.0*DR_RATE/an_rate, ph48=(double)b.phase*DR_RATE/an_rate;
    double k=floor(((double)(T-pass_w0)-ph48)/per48+0.5);
    *beat=(int64_t)(per48+0.5); *tk=pass_w0+(int64_t)(ph48+k*per48); return 1;
}

static void listen_tick(float dt){
    if(listen_msg_t>0){ listen_msg_t-=dt; if(listen_msg_t<=0){ listen_fallback=1; st=ST_CAMP; start_battle(); listen_fallback=0; } return; }
    int ver=beat_ver; dm_beat_t b; pthread_mutex_lock(&beat_mu); b=BEAT; pthread_mutex_unlock(&beat_mu);
    if(!ver && !an_end) return;
    if(!b.ok){ fprintf(stderr,"[r3drum] no steady beat (conf %d): drums\n",b.confidence); music_stop(); listen_msg_t=2.5f; return; }
    fprintf(stderr,"[r3drum] beat %d.%d BPM, conf %d\n",b.bpm_x10/10,b.bpm_x10%10,b.confidence);
    ring_w=0; pass_frame=0; song_w0=now_sample()+DR_RATE*6/10;   /* the song starts in 0.6 s */
    pb_quit=0; pthread_create(&pb_th,0,pb_thread,0); pb_run=1; music_live=1;
    start_battle();                                       /* st is ST_LISTEN: no new listening */
    int64_t bl,tk; if(song_grid(now_sample()+DR_RATE,&bl,&tk)){ while(tk<now_sample()+DR_RATE/2) tk+=bl;
        rc.beat=bl; rc.t0=tk; G.beat_s=(float)bl/DR_RATE; }
}
static void draw_listen(void){
    rect(0,TOP,CW,CH-TOP,0x120E0C);
    textcs(CW/2,TOP+150,listen_msg_t>0?"NO STEADY BEAT":"LISTENING...",4,0xE8731C);
    textc(CW/2,TOP+210,song_title,1,0xF4E4D0);
    if(listen_msg_t>0) textc(CW/2,TOP+250,"MARCHING TO THE DRUMS INSTEAD",2,0xC0A080);
    else { int w=an_rate>0? (int)((int64_t)an_frames*360/((int64_t)an_rate*20)) : 0; if(w>360) w=360;
        rect(60,TOP+250,360,10,0x2A221A); rect(60,TOP+250,w,10,0xE8731C);
        textc(CW/2,TOP+280,"FINDING THE BEAT OF YOUR SONG",1,0xC0A080); }
}
static void music_reanchor(void){                          /* lock the rhythm on the refined grid, keeping beat numbers */
    int64_t bl,tk; if(!song_grid(now_sample(),&bl,&tk)) return;
    int64_t bk=rc_nearest_beat(&rc,tk); rc.beat=bl; rc.t0=tk-bk*bl; G.beat_s=(float)bl/DR_RATE;
}

/* ======================================================================= world drawing */
static float cam;
static int sxw(float wx){ return (int)(wx-cam); }
/* ---- biomes: forest + jungle, desert, snow mountain, volcano ---- */
static const uint32_t SKY[4][2]={{0xB8E0C0,0x3A6A5A},{0xF8D890,0xE08850},{0xD8ECF8,0x6A90B8},{0xE0602A,0x2A0E0E}};
static float hsh(float x){ float s=sinf(x*12.9898f)*43758.5453f; return s-floorf(s); }   /* stable noise per world spot */
/* integer helpers for per-column work (no FPU): isin over 1024 steps -> Q15, ih -> 0..255 */
static inline int isin(int a){ return SINT[a&1023]; }
static inline int ih(int x){ uint32_t h=(uint32_t)x*2654435761u; h^=h>>15; return (int)(h&255); }
static uint16_t SKYROW[2][4][SH]; static int sky_ok[2][4];
static void pine(int x,int base,int h,uint32_t c,int snow){
    rect(x-2,base-h/5,4,h/5,0x3A2A1A);
    for(int k=0;k<3;k++){ int top=base-h+k*h/4, w=h/5+k*h/8; tri(x,top,x-w,top+h/3,x+w,top+h/3,c);
        if(snow) tri(x,top,x-w/3,top+h/9,x+w/3,top+h/9,0xF4F8FC); } }
static void broadleaf(int x,int base,int h,uint32_t c){
    rect(x-h/16,base-h*2/3,h/8,h*2/3,0x3A2A1A);
    circle(x,base-h*3/4,h/3,c); circle(x-h/4,base-h*2/3,h/4,shade(c,0.85f)); circle(x+h/4,base-h*2/3,h/4,shade(c,1.1f)); }
static void draw_world_bg(int biome,int sh){
    float t=G.time;
    if(!sky_ok[land][biome]){ sky_ok[land][biome]=1; for(int y=PF_Y;y<GROUND+40;y++) SKYROW[land][biome][y]=rgb(lerpc(SKY[biome][1],SKY[biome][0],(float)(y-PF_Y)/(GROUND-PF_Y))); }
    for(int y=PF_Y;y<GROUND+40;y++){ uint16_t k=SKYROW[land][biome][y]; for(int x=0;x<CW;x++) CV(x,y)=k; }
    int tms=(int)(t*1000), c15=(int)(cam*0.15f), c50=(int)(cam*0.5f), c40=(int)(cam*0.4f), c70=(int)(cam*0.7f);
    switch(biome){
    case BIOME_FOREST:
        for(int x=0;x<CW;x++){ int wx=x+c15; int top=PF_Y+210-(30*ih(wx/22)>>8)-(14*isin(wx*13/4)>>15)+sh;   /* far tree line */
            uint16_t k=rgb(0x2E5A44); for(int y=top;y<GROUND+40;y++) CV(x,y)=k; }
        for(int i=-1;i<8;i++){ float wx=floorf(cam*0.45f/90)*90+i*90; int x=(int)(wx-cam*0.45f); float r=hsh(wx);
            broadleaf(x+(int)(r*40),GROUND+sh,150+(int)(r*90),r<0.5f?0x3A7A3A:0x2A6A40); }
        for(int i=-1;i<10;i++){ float wx=floorf(cam*0.8f/60)*60+i*60; int x=(int)(wx-cam*0.8f); float r=hsh(wx+7);   /* jungle vines */
            if(r<0.6f){ int len=40+(int)(r*120); line(x,PF_Y,x+(int)(sinf(t+r*9)*4),PF_Y+len,2,0x2A5A2A); circle(x+(int)(sinf(t+r*9)*4),PF_Y+len,4,0x4A8A3A); } }
        rect(0,PF_Y,CW,14,0x1E3A24);
        break;
    case BIOME_DESERT:
        circle(370,PF_Y+80,40,0xFFF4C0); circle(370,PF_Y+80,32,0xFFFFE8);
        for(int i=-1;i<6;i++){ float wx=floorf(cam*0.15f/160)*160+i*160; int x=(int)(wx-cam*0.15f); float r=hsh(wx);
            if(r<0.7f){ int w=60+(int)(r*80), h=50+(int)(r*60); rect(x,PF_Y+250-h+sh,w,h+60,0xC07850); rect(x-6,PF_Y+250-h+sh,w+12,8,0xB06840); } }
        for(int layer=0;layer<2;layer++){ int off=layer?c70:c40; uint16_t k=rgb(layer?0xE8B870:0xD8A060);
            for(int x=0;x<CW;x++){ int wx=x+off; int top=PF_Y+(layer?330:300)-(22*isin(wx*2+layer*163)>>15)-(10*isin(wx*5)>>15)+sh;
                for(int y=top;y<GROUND+40;y++) CV(x,y)=k; } }
        break;
    case BIOME_SNOW:
        for(int i=-1;i<6;i++){ float wx=floorf(cam*0.12f/140)*140+i*140; int x=(int)(wx-cam*0.12f); float r=hsh(wx);
            int h=120+(int)(r*90); tri(x,PF_Y+300-h+sh,x-90,PF_Y+300+sh,x+90,PF_Y+300+sh,0x7A90B0); tri(x,PF_Y+300-h+sh,x-30,PF_Y+300-h+40+sh,x+30,PF_Y+300-h+40+sh,0xF4F8FC); }
        rect(0,PF_Y+300+sh,CW,GROUND-PF_Y-300+40,0xB8CCE0);
        for(int i=-1;i<10;i++){ float wx=floorf(cam*0.5f/55)*55+i*55; int x=(int)(wx-cam*0.5f); float r=hsh(wx+3);
            pine(x+(int)(r*20),GROUND+sh,70+(int)(r*60),0x2A4A3A,1); }
        for(int k=0;k<40;k++){ int x=(k*73+tms*(18+k%5*6)/1000+c15*2)%CW, y=PF_Y+(k*41+tms*(40+k%7*8)/1000)%(GROUND-PF_Y); px(x,y,0xFFFF); px(x+1,y,0xFFFF); }
        break;
    case BIOME_VOLCANO: {
        int vx=(int)(300-cam*0.08f)%700; if(vx<-200) vx+=700;
        tri(vx,PF_Y+120+sh,vx-220,PF_Y+330+sh,vx+220,PF_Y+330+sh,0x3A1A16); rect(vx-28,PF_Y+120+sh,56,8,0xFF6020);
        for(int k=0;k<5;k++){ int r=20+k*12+(int)(sinf(t*1.5f+k)*4); circle(vx+(int)(sinf(t*0.5f+k)*20),PF_Y+100-k*24+sh,r,0x4A3A38); }
        { uint16_t k=rgb(0x2A1410); for(int x=0;x<CW;x++){ int wx=x+c50; int top=PF_Y+320-(30*ih(wx/40)>>8)-(12*isin(wx*5)>>15)+sh;
            for(int y=top;y<GROUND+40;y++) CV(x,y)=k; } }
        for(int k=0;k<30;k++){ int x=(k*97+tms*20*(k%3+1)/1000)%CW, y=GROUND-(k*53+tms*35*(k%2+1)/1000)%(GROUND-PF_Y); px(x,y,rgb(0xFFB040)); px(x+1,y,rgb(0xFF8020)); }
        } break;
    }
    uint32_t gtop = biome==0?0x3A6A2A : biome==1?0xE0B070 : biome==2?0xF0F6FA : 0x3A2018,
             gfill= biome==0?0x3A2A1A : biome==1?0xC89858 : biome==2?0xB8CCE0 : 0x221010;
    rect(0,GROUND+sh,CW,4,gtop); rect(0,GROUND+4+sh,CW,PLAY_BOT-GROUND-4,gfill);
    for(int i=-2;i<14;i++){ float wx=floorf(cam/40)*40+i*40; int x=sxw(wx); float r=hsh(wx+1);
        if(biome==0 && r<0.4f){ line(x,GROUND+sh,x-4,GROUND-10+sh,2,0x4A8A3A); line(x,GROUND+sh,x+5,GROUND-12+sh,2,0x5A9A3A); }
        if(biome==1 && r<0.12f){ rect(x-3,GROUND-34+sh,6,34,0x4A7A3A); rect(x-12,GROUND-24+sh,9,4,0x4A7A3A); rect(x-12,GROUND-30+sh,4,8,0x4A7A3A); }
        if(biome==2 && r<0.3f) ellipse(x,GROUND+2+sh,10,3,0xFFFFFF);
        if(biome==3 && r<0.35f){ line(x,GROUND+8+sh,x+18,GROUND+14+sh,2,lerpc(0xFF4010,0xFFC040,0.5f+0.5f*sinf(t*3+r*9))); }
        if(r>0.85f) ellipse(x+12,GROUND+12+sh,5,3,shade(gfill,1.3f)); }
    /* the banner at the end */
    int bx=sxw((float)G.m->length); if(bx>-40 && bx<CW+40){ line(bx,GROUND+sh,bx,GROUND-110+sh,4,0x3A2A1A);
        float wv=sinf(t*4)*4; tri(bx+2,GROUND-110+sh,bx+50,GROUND-96+(int)wv+sh,bx+2,GROUND-80+sh,0xE8731C);
        circle(bx+18,GROUND-96+sh,5,0xFFE0A0); }
}
/* the fortress wall behind an iron gate (drawn even once the gate has fallen) */
static void draw_walls(int sh){
    for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&G.e[k]; if(!e->on || e->type!=ET_IRONGATE) continue;
        int x=sxw(e->x); if(x<-400||x>CW+400) continue; int top=GROUND-130+sh;
        rect(x-170,top,360,130,0x6A6058); for(int c=-170;c<190;c+=24) rect(x+c,top-14,14,14,0x6A6058);
        for(int r=0;r<5;r++) for(int c=-170+(r%2)*15;c<190;c+=30) frame(x+c,top+r*26,30,26,1,0x4A443E);
        if(e->hp<=0){ rect(x-30,top+20,60,110,0x2A2420); for(int i=0;i<6;i++) ellipse(x-30+i*12,GROUND-4+sh,9,6,0x5A524A); } } }
static const uint32_t SHIELD_C[7]={0,0xC8A060,0x8A5A3A,0xD0A040,0x8A9098,0xC0E8FF,0x5A8A3A};
static void draw_unit(int cx,int gy,int cls,int weapon,int shield,float swing,int guard,int flash,float stepph,int sc,float hpf){
    uint32_t c=DG_CLASS[cls].color, dark=shade(c,0.55f); if(flash) c=dark=0xFFFFFF;
    int s=sc;
    /* legs */
    int lo=(int)(sinf(stepph)*4*s);
    line(cx-4*s,gy-8*s,cx-4*s+lo,gy,2*s,0x2A1A10); line(cx+4*s,gy-8*s,cx+4*s-lo,gy,2*s,0x2A1A10);
    /* cloak + hood */
    tri(cx,gy-42*s,cx-13*s,gy-6*s,cx+13*s,gy-6*s,c);
    rect(cx-13*s,gy-9*s,26*s,3*s,dark);
    circle(cx,gy-38*s,10*s,dark); ellipse(cx+2*s,gy-37*s,6*s,5*s,0x1A1010);
    rect(cx+1*s,gy-38*s,2*s,2*s,0xFFFFFF); rect(cx+5*s,gy-38*s,2*s,2*s,0xFFFFFF);
    /* weapon */
    int hx=cx+8*s, hy=gy-22*s; float sw=swing;
    const weapon_t*w = weapon>=0 ? &DG_WEAPON[weapon] : 0;
    uint32_t ec = !w?0xC0C0C0 : w->elem==EL_FIRE?0xFF7020 : w->elem==EL_FROST?0x90E0FF : w->elem==EL_STORM?0xFFF060 : 0xD8D8D0;
    switch(cls){
    case CL_LANCER: { int tx=hx+(int)((10+sw*22)*s), ty=hy-(int)((26-sw*20)*s); line(hx-6*s,hy+8*s,tx,ty,2*s,0x8A6A40); tri(tx,ty-4*s,tx+8*s,ty-2*s,tx,ty+3*s,ec); } break;
    case CL_WARDEN: case CL_BREAKER: { float a=-1.2f+sw*2.2f; int len=(cls==CL_BREAKER?26:20)*s; int tx=hx+(int)(cosf(a)*len), ty=hy+(int)(sinf(a)*len);
        line(hx,hy,tx,ty,2*s,0x6A4A2A);
        if(w && w->dtype==DT_SLASH && cls==CL_WARDEN) line(hx,hy,tx,ty,3*s,ec);
        else if(cls==CL_BREAKER && w && w->dtype==DT_SLASH) tri(tx,ty,tx+9*s,ty+6*s,tx-2*s,ty+9*s,ec);
        else rect(tx-5*s,ty-5*s,10*s,10*s,w&&w->elem?ec:0x8A8A90); } break;
    case CL_SLINGER: if(w && w->dtype==DT_CRUSH){ line(hx,hy,hx+(int)(sw*14*s),hy-(int)(16*s*(1-sw)),1*s+1,0x6A4A2A); circle(hx+(int)(sw*14*s),hy-(int)(16*s*(1-sw)),3*s,0x9A9A9A); }
        else { for(int k=-12;k<=12;k++){ int yy=hy+k*s, xx=hx+(int)((6-k*k/24.0f)*s); px(xx,yy,rgb(0x6A4A2A)); px(xx+1,yy,rgb(0x6A4A2A)); }
            line(hx+(int)(6*s*(1-sw)),hy-12*s,hx+(int)(6*s*(1-sw)),hy+12*s,1,ec); } break;
    case CL_MENDER: line(hx,gy-48*s,hx,gy,2*s,0x8A6A40); circle(hx,gy-50*s,(4+(int)(sw*3))*s,ec==0xD8D8D0?0x80FFA0:ec); break;
    }
    /* shield */
    if(shield>0){ int gyo=guard?-10*s:0; uint32_t sc2=SHIELD_C[shield];
        if(shield==4) rect(cx+8*s,gy-34*s+gyo,10*s,30*s,sc2);
        else ellipse(cx+12*s,gy-20*s+gyo,6*s,10*s,sc2);
        if(shield==5) line(cx+10*s,gy-26*s+gyo,cx+13*s,gy-14*s+gyo,1,0xFFFFFF);
        if(shield==6) for(int k=-1;k<=1;k++) tri(cx+17*s,gy-22*s+k*6*s+gyo,cx+23*s,gy-20*s+k*6*s+gyo,cx+17*s,gy-18*s+k*6*s+gyo,0x2A4A1A); }
    if(guard) ring(cx+12*s,gy-22*s,14*s,2,0xFFFFFF);
    if(hpf<0.999f){ rect(cx-12,gy-58*s,24,4,0x300000); rect(cx-12,gy-58*s,(int)(24*hpf),4,hpf>0.4f?0x60E060:0xE04040); }
}

/* foes: procedural, scaled. gy = ground (or flight) y on screen, s = scale */
static void eyes(int x,int y,int r,uint32_t c){ circle(x,y,r,0xFFF0C0); circle(x-r/3,y,r/2,c); }
static void draw_enemy(const enemy_t*e,int sh){
    const etype_t*t=&DG_ETYPE[e->type];
    int x=sxw(e->x), gy=GROUND+(int)e->y+sh; float bob=sinf(e->bob*3)*3, act=e->act>0?e->act:0;
    if(e->traits&TR_FLYING) gy+=(int)(bob*2);
    int fl=e->flash>0 && ((int)(e->flash*30)&1);
    uint32_t c=fl?0xFFFFFF:t->color, d=fl?0xFFFFFF:shade(t->color,0.6f), l=fl?0xFFFFFF:shade(t->color,1.3f);
    int lunge=-(int)(act*20);
    const bmove_t*mv=dg_boss_move(&G,(int)(e-G.e));
    if((mv || e->windup) && ((int)(G.time*8)&1)){                                     /* telegraph glow */
        int r = t->boss>=0 ? t->hw+30 : 34; ellipse(x,gy-r/2-10,r,r*2/3,0xFF3020); }
    switch(e->type){
    case ET_GRUNT: case ET_CHIEFTAIN: { int s=e->type==ET_CHIEFTAIN?2:1;
        ellipse(x+lunge,gy-26*s,22*s,26*s,c); circle(x-14*s+lunge,gy-50*s,12*s,d); eyes(x-18*s+lunge,gy-52*s,3*s,0);
        line(x-24*s+lunge,gy-34*s,x-40*s+lunge-(int)(act*20),gy-60*s+(int)(act*40),4*s,0x5A3A1A); circle(x-40*s+lunge-(int)(act*20),gy-62*s+(int)(act*40),7*s,0x5A3A1A);
        rect(x-14*s,gy-6*s,8*s,6*s,d); rect(x+6*s,gy-6*s,8*s,6*s,d);
        if(s==2){ tri(x-22*s,gy-58*s,x-30*s,gy-74*s,x-16*s,gy-60*s,0xE0D0B0); tri(x-6*s,gy-58*s,x+2*s,gy-74*s,x-12*s,gy-60*s,0xE0D0B0); } } break;
    case ET_IRONBACK: case ET_RUSTJAW: case ET_BULWARK: { int s=e->type==ET_RUSTJAW?2:1; if(e->type==ET_BULWARK) s=1;
        ellipse(x+lunge,gy-20*s,30*s,22*s,c); for(int k=-2;k<=2;k++) line(x+k*10*s+lunge,gy-40*s,x+k*12*s+lunge,gy-4*s,2,d);
        circle(x-30*s+lunge,gy-14*s,10*s,d); eyes(x-34*s+lunge,gy-16*s,3*s,0xC02020);
        if(e->type==ET_RUSTJAW){ tri(x-40*s+lunge,gy-10*s,x-52*s+lunge,gy-4*s-(int)(act*10),x-40*s+lunge,gy-2*s,0xE0D0B0); }
        if(e->type==ET_BULWARK){ rect(x-44+lunge,gy-60,14,58,0x8A9098); circle(x+6,gy-50,12,d); eyes(x+2,gy-52,3,0); } } break;
    case ET_WINGSPITE: { float f=sinf(e->bob*14); ellipse(x,gy-10,12,9,c);
        tri(x-4,gy-12,x-30,gy-12-(int)(f*18),x-10,gy-4,d); tri(x+4,gy-12,x+30,gy-12-(int)(f*18),x+10,gy-4,d); eyes(x-6,gy-12,3,0xC02020); } break;
    case ET_PALISADE: case ET_TOTEM:
        if(e->type==ET_PALISADE) for(int k=-3;k<=3;k++){ int sx=x+k*10; rect(sx-4,gy-70+abs(k)*3,8,70-abs(k)*3,c); tri(sx-4,gy-70+abs(k)*3,sx,gy-80+abs(k)*3,sx+4,gy-70+abs(k)*3,l); }
        else for(int k=0;k<3;k++){ rect(x-14,gy-30*(k+1),28,28,k==1?l:c); eyes(x-6,gy-30*k-18,3,0x101010); eyes(x+6,gy-30*k-18,3,0x101010); rect(x-8,gy-30*k-8,16,3,0x101010); }
        break;
    case ET_STONESLINGER: case ET_GOURDTHROWER:
        ellipse(x,gy-18,14,18,c); circle(x-4,gy-38,9,d); eyes(x-8,gy-40,2,0);
        if(e->type==ET_GOURDTHROWER){ ellipse(x+4,gy-14,12,10,0xC0C040); circle(x-16-(int)(act*10),gy-44,6,0xC0C040); }
        else line(x-10,gy-30,x-20-(int)(act*16),gy-46,2,0x6A4A2A); break;
    case ET_BRISTLER: ellipse(x+lunge,gy-12,16,12,c); for(int k=0;k<7;k++){ float a=3.3f+k*0.45f; line(x+lunge,gy-12,x+lunge+(int)(cosf(a)*24),gy-12+(int)(sinf(a)*20),2,d); }
        eyes(x-12+lunge,gy-14,2,0); break;
    case ET_HEXCALLER: case ET_HEXMOTHER: { int s=e->type==ET_HEXMOTHER?2:1;
        tri(x,gy-56*s,x-18*s,gy,x+18*s,gy,c); circle(x,gy-56*s,9*s,d); eyes(x-4*s,gy-57*s,2*s,0x40FF80);
        circle(x-20*s,gy-40*s+(int)(bob*s),(5+(int)(act*6))*s,0xB070FF);
        if(s==2){ tri(x-8*s,gy-64*s,x,gy-76*s,x+8*s,gy-64*s,0xE0C040); circle(x+20*s,gy-46*s-(int)bob,6*s,0xB070FF); } } break;
    case ET_SCOUT: { float run=e->fleeing?sinf(G.time*20):0; int lx=(int)(run*6);
        line(x-3,gy-14,x-3+lx,gy,2,0x2A1A10); line(x+3,gy-14,x+3-lx,gy,2,0x2A1A10);
        tri(x,gy-40,x-9,gy-12,x+9,gy-12,c); circle(x,gy-38,7,d); eyes(x-3,gy-38,2,0);
        line(x+6,gy-30,x+14,gy-48,1,0x6A4A2A); tri(x+14,gy-48,x+26,gy-44,x+14,gy-40,0xC04030);
        if(e->fleeing) textcs(x,gy-70,"!",2,0xFFE040); } break;
    case ET_GATE: rect(x-24,gy-112,6,112,d); rect(x+18,gy-112,6,112,d);
        for(int k=0;k<5;k++) rect(x-18+k*8,gy-104,7,104,k%2?c:l);
        rect(x-18,gy-84,36,5,0x3A3A40); rect(x-18,gy-30,36,5,0x3A3A40);
        if(e->hp<e->maxhp*0.5f) line(x-14,gy-90,x+8,gy-40,2,0x2A1A0A); break;
    case ET_WATCHTOWER: line(x-16,gy,x-10,gy-120,3,d); line(x+16,gy,x+10,gy-120,3,d); line(x-14,gy-40,x+14,gy-80,2,d); line(x+14,gy-40,x-14,gy-80,2,d);
        rect(x-20,gy-126,40,8,c); tri(x-24,gy-150,x,gy-170,x+24,gy-150,0x8A3A2A); rect(x-2,gy-150,4,24,d);
        tri(x-6,gy-146,x-12,gy-126,x,gy-126,0xA08860); circle(x-6,gy-146,5,0x7A6040); line(x-12,gy-140,x-22-(int)(act*10),gy-150,1,0x6A4A2A); break;
    case ET_IRONGATE: rect(x-34,gy-140,68,140,0x5A524A); rect(x-26,gy-120,52,120,0x1A1614);
        for(int k=0;k<5;k++) rect(x-24+k*12,gy-118,3,118,c); for(int k=0;k<5;k++) rect(x-26,gy-112+k*24,52,3,c);
        if(e->hp<e->maxhp*0.5f){ line(x-20,gy-100,x+4,gy-60,2,0x0A0A0A); line(x+10,gy-90,x-6,gy-30,2,0x0A0A0A); } break;
    case ET_BALLISTA: { rect(x-20,gy-130,40,130,0x7A7068); for(int c2=-20;c2<20;c2+=12) rect(x+c2,gy-142,8,12,0x7A7068);
        int draw=e->windup?(int)(10+sinf(G.time*12)*2):0;
        line(x-30,gy-150,x+10,gy-150,4,0x5A3A1A); line(x-26,gy-162,x-26+draw,gy-150,1,0xE0E0D0); line(x-26,gy-138,x-26+draw,gy-150,1,0xE0E0D0);
        line(x-30,gy-162,x-30,gy-138,3,d); if(e->windup) textcs(x,gy-190,"!",3,0xFF3020); } break;
    case ET_FROSTLING: tri(x,gy-60,x-16,gy,x+16,gy,c); tri(x-10,gy-40,x-24,gy-54,x-4,gy-44,l); tri(x+10,gy-40,x+24,gy-54,x+4,gy-44,l); eyes(x-4,gy-46,3,0x2050C0); break;
    case ET_CINDERHORN: { int s=2; ellipse(x+lunge,gy-24*s,26*s,22*s,c); circle(x-24*s+lunge,gy-34*s,12*s,d);
        tri(x-30*s+lunge,gy-42*s,x-44*s+lunge,gy-60*s,x-24*s+lunge,gy-44*s,0xFFE0A0); eyes(x-28*s+lunge,gy-36*s,3*s,0xFF4000);
        for(int k=0;k<4;k++) line(x-10*s+k*8*s,gy-40*s,x-6*s+k*8*s,gy-10*s,2,0xFFB040);
        rect(x-16*s,gy-6*s,8*s,6*s,d); rect(x+8*s,gy-6*s,8*s,6*s,d); } break;
    case ET_THORNHIDE: {                                                           /* the titan boar */
        int bx=x+lunge*2; ellipse(bx,gy-80,130,78,c); ellipse(bx+20,gy-95,100,50,l);
        for(int k=0;k<9;k++){ int sx=bx-70+k*20; tri(sx-8,gy-140+abs(k-4)*6,sx,gy-190+abs(k-4)*8,sx+8,gy-140+abs(k-4)*6,0xE8D8B0); }
        ellipse(bx-120,gy-70,46,40,d); eyes(bx-134,gy-86,7,0xC02020); eyes(bx-108,gy-88,6,0xC02020);
        tri(bx-150,gy-50,bx-190,gy-100-(int)(act*20),bx-140,gy-40,0xF0E8D0); tri(bx-110,gy-46,bx-130,gy-96-(int)(act*20),bx-100,gy-40,0xF0E8D0);
        for(int k=0;k<4;k++) rect(bx-90+k*55,gy-24,24,24,d); } break;
    case ET_GLOOMWING: { float f=e->grounded? 0.3f : sinf(e->bob*5);              /* moth-raptor */
        tri(x,gy-60,x-150,gy-130-(int)(f*70),x-30,gy-10,d); tri(x,gy-60,x+150,gy-130-(int)(f*70),x+30,gy-10,d);
        for(int k=0;k<3;k++){ circle(x-80+k*12,gy-90-(int)(f*35),9,0xE0C060); circle(x+80-k*12,gy-90-(int)(f*35),9,0xE0C060); }
        ellipse(x,gy-60,34,56,c); circle(x-26,gy-110,24,c); eyes(x-36,gy-116,7,0xFF2020); eyes(x-14,gy-118,6,0xFF2020);
        tri(x-48,gy-104,x-70,gy-96+(int)(act*10),x-46,gy-94,0xE0D0B0); } break;
    case ET_GLACIERWYRM: {                                                        /* serpent of ice */
        for(int k=6;k>=0;k--){ int sx=x+60-k*-28+(int)(sinf(e->bob*2+k)*6), sy=gy-30-k*22; circle(sx,sy,40-k*2,k%2?c:l);
            tri(sx-10,sy-36+k*2,sx,sy-60+k,sx+10,sy-36+k*2,0xE8F8FF); }
        int hx=x-90+lunge*2, hy=gy-190;
        ellipse(hx,hy,60,38,c); tri(hx-50,hy+10,hx-110,hy+20+(int)(act*30),hx-40,hy+30,l);
        tri(hx-20,hy-30,hx-10,hy-80,hx+10,hy-30,0xE8F8FF); tri(hx+10,hy-30,hx+30,hy-70,hx+30,hy-26,0xE8F8FF);
        eyes(hx-30,hy-10,8,0x2050C0); } break;
    case ET_EMBERMAW: {                                                           /* the fireproof beast */
        int bx=x+lunge*2; ellipse(bx+30,gy-100,140,96,c);
        for(int k=0;k<6;k++) line(bx-60+k*36,gy-180+(k%2)*20,bx-40+k*36,gy-40,3,0xFFB040);
        ellipse(bx-110,gy-120,70,60,d); int open=(int)(act*30)+((mv&&(mv->kind==MV_SWEEP))?(int)(sinf(G.time*20)*6+12):0);
        ellipse(bx-150,gy-96,40,10+open,0x200800); ellipse(bx-150,gy-96,30,4+open*2/3,0xFF6010);
        eyes(bx-130,gy-150,9,0xFFE000); eyes(bx-96,gy-152,8,0xFFE000);
        tri(bx-100,gy-170,bx-130,gy-240,bx-80,gy-176,0x3A1A10); tri(bx-60,gy-170,bx-60,gy-236,bx-40,gy-170,0x3A1A10);
        for(int k=0;k<4;k++) rect(bx-60+k*60,gy-30,30,30,d); } break;
    }
    if(e->burn>0) for(int k=0;k<3;k++) circle(x-10+k*10,gy-30-(int)(fmodf(G.time*60+k*20,40)),3,0xFF8020);
    if(e->frost>0) ring(x,gy-30,26,2,0x90E0FF);
    if(e->windup && !mv && e->type!=ET_BALLISTA) textcs(x,gy-(t->traits&TR_FLYING?50:(e->type==ET_WATCHTOWER?186:96)),"!",3,0xFF3020);
    if(t->boss<0 && e->hp<e->maxhp){ int w=40; rect(x-w/2,gy-(t->traits&TR_FLYING?44:90),w,4,0x300000); rect(x-w/2,gy-(t->traits&TR_FLYING?44:90),(int)(w*e->hp/e->maxhp),4,0xE04040); }
}
static void draw_proj(const proj_t*p,int sh){
    float t=p->t/p->dur; if(t>1)t=1;
    float tx,ty;
    if(!p->foe){ const enemy_t*e=&G.e[p->target]; tx=dg_front(e)+DG_ETYPE[e->type].hw*0.4f; ty=e->y-30-(DG_ETYPE[e->type].boss>=0?60:0); }
    else { const unit_t*u=&G.u[p->target]; tx=u->x; ty=-24; }
    float arc = p->kind==1?40:70;
    float x=p->x0+(tx-p->x0)*t, y=p->y0+(ty-p->y0)*t-arc*4*t*(1-t);
    int X=sxw(x), Y=GROUND+(int)y+sh;
    float t2=t+0.04f, x2=p->x0+(tx-p->x0)*t2, y2=p->y0+(ty-p->y0)*t2-arc*4*t2*(1-t2);
    float dx=x2-x, dy=y2-y, n=sqrtf(dx*dx+dy*dy)+1e-3f; dx/=n; dy/=n;
    uint32_t ec = p->elem==EL_FIRE?0xFF7020 : p->elem==EL_FROST?0x90E0FF : p->elem==EL_STORM?0xFFF060 : 0xE0E0D0;
    if(p->elem) for(int k=1;k<4;k++) circle(X-(int)(dx*k*7),Y-(int)(dy*k*7),4-k,ec);
    switch(p->kind){
    case 0: line(X-(int)(dx*16),Y-(int)(dy*16),X,Y,2,0x8A6A40); circle(X,Y,2,ec); break;            /* javelin */
    case 1: line(X-(int)(dx*12),Y-(int)(dy*12),X,Y,1,0xD0C0A0); break;                               /* arrow */
    case 2: circle(X,Y,3,0x9A9A9A); break;                                                            /* sling stone */
    case 3: circle(X,Y,5,0x7A6A5A); break;                                                            /* rock / spit */
    case 4: circle(X,Y,6,0xC0C040); line(X,Y-6,X+3,Y-10,1,0xFF8020); break;                           /* gourd bomb */
    case 5: line(X-(int)(dx*30),Y-(int)(dy*30),X,Y,4,0x5A3A1A); tri(X,Y-4,X+(int)(dx*10),Y+(int)(dy*10),X,Y+4,0xD0D0D8); break;   /* ballista bolt */
    }
}

/* ======================================================================= battle screen */
#define PAD_Y 506
static const char*PAD_LBL[4]={"DUM","KA","TOK","CHI"};
static const uint32_t PAD_C[4]={0xB0302A,0xE8731C,0x8A5A30,0xD8B030};
static void pad_rect(int d,int*x,int*y,int*w,int*h){   /* DUM bottom-left, KA bottom-right, TOK top-left, CHI top-right */
    int col = d==DR_DUM||d==DR_TOK ? 0 : 1, row = d==DR_TOK||d==DR_CHI ? 0 : 1;
    *x=12+col*232; *y=PAD_Y+row*106; *w=224; *h=100; }
static void draw_pads(void){
    rect(0,PAD_Y-6,CW,CH-PAD_Y+6,0x0A0A0A);
    for(int d=0;d<4;d++){ int x,y,w,h; pad_rect(d,&x,&y,&w,&h); float lit=pad_t[d]>0?pad_t[d]/0.15f:0;
        rbox(x,y,w,h,16,lerpc(shade(PAD_C[d],0.45f),shade(PAD_C[d],1.4f),lit));
        ring(x+w/2,y+h/2,40,4,shade(PAD_C[d],lit>0?1.6f:0.9f));
        textc(x+w/2,y+h/2-10,PAD_LBL[d],3,lit>0?0x101010:0xF4E4D0); }
}
static void hud(void){
    int y=446; rect(0,y,CW,56,0x141010); rect(0,y,CW,2,0x3A2A20);
    /* the command being drummed: four slots */
    for(int i=0;i<4;i++){ int x=12+i*46; rbox(x,y+8,40,40,8,0x2A221A);
        if(i<rc.nseq){ int d=rc.seq[i]; rbox(x+3,y+11,34,34,6,PAD_C[d]); textc(x+20,y+21,PAD_LBL[d],1,0x101010); } }
    /* metronome */
    int64_t ns=now_sample(); float ph=(float)((ns-rc.t0)%rc.beat)/(float)rc.beat; if(ns<rc.t0) ph=1;
    int pr=(int)(16*(1-ph)); circle(222,y+28,6+pr,ph<0.2f?0xF4E4D0:0x5A4A3A);
    /* command / combo / fever */
    int bi; int cmd=rc_responding(&rc,ns,&bi);
    if(cmd!=CMD_NONE) textcs(330,y+12,DR_CMD_NAME[cmd],3,rc.fever?0xFFE040:0xF4E4D0);
    else if(rc.combo>0 && rc.nseq==0){ int64_t b=rc_nearest_beat(&rc,ns); if(b==rc.next_start) textcs(330,y+12,"NOW!",3,0x80FF80); }
    if(rc.combo>0){ char b[16]; snprintf(b,sizeof b,"X%d",rc.combo); text(420,y+34,b,2,rc.fever?0xFFE040:0xC0A080); }
    if(rc.fever && ((int)(G.time*6)&1)) text(250,y+38,"FEVER",2,0xFFE040);
    if(judge_t>0){ const char*j= judge==J_PERFECT?"PERFECT":judge==J_GOOD?"GOOD":"MISS";
        uint32_t c=judge==J_PERFECT?0xFFE040:judge==J_GOOD?0x80E0FF:0xFF5040; textcs(CW/2,PAD_Y-60,j,3,c); }
}
static void hud_land(void){
    int y=PLAY_BOT; rect(0,y,CW,CH-y,0x141010); rect(0,y,CW,2,0x3A2A20);
    for(int i=0;i<4;i++){ int x=12+i*46; rbox(x,y+10,40,40,8,0x2A221A);
        if(i<rc.nseq){ int d=rc.seq[i]; rbox(x+3,y+13,34,34,6,PAD_C[d]); textc(x+20,y+23,PAD_LBL[d],1,0x101010); } }
    int64_t ns=now_sample(); float ph=(float)((ns-rc.t0)%rc.beat)/(float)rc.beat; if(ns<rc.t0) ph=1;
    int pr=(int)(16*(1-ph)); circle(222,y+30,6+pr,ph<0.2f?0xF4E4D0:0x5A4A3A);
    int bi; int cmd=rc_responding(&rc,ns,&bi);
    if(cmd!=CMD_NONE) textcs(380,y+16,DR_CMD_NAME[cmd],3,rc.fever?0xFFE040:0xF4E4D0);
    else if(rc.combo>0 && rc.nseq==0){ int64_t b=rc_nearest_beat(&rc,ns); if(b==rc.next_start) textcs(380,y+16,"NOW!",3,0x80FF80); }
    if(rc.combo>0){ char b[16]; snprintf(b,sizeof b,"X%d",rc.combo); text(UW-70,y+14,b,2,rc.fever?0xFFE040:0xC0A080); }
    if(rc.fever && ((int)(G.time*6)&1)) text(UW-80,y+36,"FEVER",2,0xFFE040);
    /* the drum buttons by place, as a reminder: left TOK, up CHI, down DUM, right KA */
    if(judge_t>0){ const char*j= judge==J_PERFECT?"PERFECT":judge==J_GOOD?"GOOD":"MISS";
        uint32_t c=judge==J_PERFECT?0xFFE040:judge==J_GOOD?0x80E0FF:0xFF5040; textcs(UW/2,PLAY_BOT-70,j,3,c); }
}
static void legend(int y){
    int lx=(UW-420)/2; (void)lx;
    rbox(30+(UW-480)/2,y,420,6*22+14,10,0x000000);
    for(int c=0;c<CMD_N;c++){ int yy=y+10+c*22; text(44,yy+3,DR_CMD_NAME[c],2,0xF4E4D0);
        for(int i=0;i<4;i++){ int d=DR_PATTERN[c][i]; rbox(170+i*68,yy,62,18,6,PAD_C[d]); textc(201+i*68,yy+5,PAD_LBL[d],1,0x101010); } }
    textc(CW/2,y+10+5*22+2,"ONE HIT PER BEAT. KEEP THE STREAK.",1,0xC0A080);
}
static void draw_bubbles(int sh);
static int mv_banner_busy(void){ int b=dg_boss(&G); return b>=0 && dg_boss_move(&G,b)!=0; }
static void draw_battle(void){
    float target=G.sx-(land?230:150); cam+= (target-cam)*0.15f;
    int sh = shake>0 ? (int)((rand()%9-4)*shake) : 0;
    draw_world_bg(G.m->biome,sh); draw_walls(sh);
    for(int k=0;k<DG_MAXE;k++) if(G.e[k].on && G.e[k].hp>0) draw_enemy(&G.e[k],sh);
    int guard = G.beat<=G.guard_until;
    for(int i=0;i<DG_MAXU;i++){ const unit_t*u=&G.u[i]; if(!u->on) continue;
        if(u->hp<=0){ ellipse(sxw(u->x),GROUND-4+sh,12,4,0x2A1A10); continue; }
        draw_unit(sxw(u->x),GROUND+(int)dg_unit_y(&G,i)+sh,u->cls,u->weapon,u->shield,u->swing,guard,u->flash>0&&((int)(u->flash*30)&1),
                  G.move_left!=0?G.time*12.566f+i:0,1,u->hp/u->maxhp); }
    for(int k=0;k<DG_MAXP;k++) if(G.p[k].on) draw_proj(&G.p[k],sh);
    draw_bubbles(sh);
    for(int k=0;k<160;k++){ part_t*p=&P[k]; if(p->life<=0) continue;
        int X=p->world?sxw(p->x):(int)p->x, Y=p->world?GROUND+(int)p->y:(int)p->y; circle(X,Y+sh,(int)(p->size*(p->life>0.3f?1:p->life/0.3f)),p->c); }
    if(rc.fever) for(int k=0;k<6;k++){ int x=(int)((G.time*80+k*90))%CW; px(x,PF_Y+40+(k*37)%200,rgb(0xFFF080)); }
    /* boss bar + telegraph banner */
    int b=dg_boss(&G);
    if(b>=0 && dg_front(&G.e[b])-G.sx<700){ const enemy_t*e=&G.e[b]; const etype_t*t=&DG_ETYPE[e->type];
        texts(12,PF_Y+6,t->name,2,e->enraged?0xFF5040:0xF4E4D0);
        rect(12,PF_Y+24,UW-24,8,0x300000); rect(12,PF_Y+24,(int)((UW-24)*e->hp/e->maxhp),8,e->enraged?0xFF3020:0xE0A020);
        const bmove_t*mv=dg_boss_move(&G,b);
        if(mv){ char s[40]; snprintf(s,sizeof s,"%s!",mv->name); textcs(UW/2,PF_Y+44,s,3,((int)(G.time*6)&1)?0xFFE040:0xFF5040);
            for(int i=0;i<e->windup && i<11;i++) circle(UW/2-(e->windup-1)*9+i*18,PF_Y+76,5,0xFF5040); } }
    /* progress */
    rect(12,PLAY_BOT-8,UW-24,3,0x2A2018); rect(12,PLAY_BOT-8,(int)((UW-24)*G.sx/G.m->length),3,0xE8731C);
    if(banner_t>0 && !mv_banner_busy()){ int a=banner_t>2.4f?(int)((2.8f-banner_t)/0.4f*255):255; (void)a;
        textcs(UW/2,PF_Y+120,banner,4,0xFFE0A0); if(banner2) textcs(UW/2,PF_Y+160,banner2,2,0xF4E4D0); }
    if(G.time<10 || paused) legend(PF_Y+40);
    if(music_live && G.time<12){ char b[80]; snprintf(b,sizeof b,"MUSIC: %s",song_title); textcs(UW/2,PLAY_BOT-24,b,1,0xF4E4D0); }
    if(land) hud_land(); else { hud(); draw_pads(); }
    if(paused){ dim(0,PF_Y,CW,CH-PF_Y); legend(PF_Y+(land?20:92)); textcs(UW/2,PF_Y+(land?180:260),"PAUSED",4,0xF4E4D0);
        textc(UW/2,PF_Y+(land?222:310),land?"MINUS / POWER: RESUME   HOLD POWER: LEAVE":"POWER OR TAP: RESUME   HOLD POWER: LEAVE",1,0xC0A080); }
    if(st==ST_BATTLE && end_t>0) textcs(UW/2,PF_Y+200,G.won?"VICTORY!":"THE SQUAD FELL",4,G.won?0xFFE040:0xFF5040);
}

/* ---- speech bubbles: the squad chatters, picked at random per situation ---- */
enum { SAY_MARCH, SAY_STRIKE, SAY_GUARD, SAY_RETREAT, SAY_RALLY, SAY_FEVER, SAY_BREAK, SAY_BOSS_SEEN,
       SAY_TELEGRAPH, SAY_KILL, SAY_HURT, SAY_BOSS_DOWN, SAY_IDLE, SAY_WIN,
       SAY_SCOUT, SAY_ALARM, SAY_FORT, SAY_GATE, SAY_N };
static const char*const LINES[SAY_N][6]={
    {"ONWARD!","LEFT, RIGHT!","TO THE BANNER!","KEEP THE BEAT!","MARCH ON!","STEP LIVELY!"},
    {"HAVE AT YOU!","FOR THE DRUM!","TAKE THAT!","SPEARS UP!","CHAAARGE!","NOW!"},
    {"SHIELDS!","HOLD FAST!","BRACE!","STEADY...","NOT TODAY!","HUNKER DOWN!"},
    {"BACK! BACK!","FALL BACK!","REGROUP!","TACTICAL!","NOPE NOPE NOPE","RUN!"},
    {"TOGETHER!","BREATHE...","SING WITH ME!","CLOSE RANKS!","ONE MORE TIME!","FEEL THE RHYTHM"},
    {"FEVER!!","WE'RE ON FIRE!","CAN'T STOP US!","THE DRUMS SING!","YEAAAH!","UNSTOPPABLE!"},
    {"WHO MISSED?","OOPS...","OFF BEAT!","HEY, DRUMMER!","WHAT WAS THAT?","AGAIN!"},
    {"WHAT IS THAT?!","IT'S HUGE...","DON'T FAIL ME, DRUMS","STAY IN STEP!","BIG ONE AHEAD!","GULP."},
    {"IT'S CHARGING!","LOOK OUT!","GUARD, NOW!","HERE IT COMES!","GET BACK!","BRACE YOURSELVES!"},
    {"GOT ONE!","DOWN IT GOES!","NEXT!","HA!","TOO EASY","STAY DOWN!"},
    {"OUCH!","I NEED A MENDER!","THAT STINGS","I'M FINE. I'M FINE.","ARGH!","HELP!"},
    {"WE DID IT!","THE BEAST FALLS!","SING OF THIS DAY!","HUZZAH!","IS IT DEAD?","LEGENDARY!"},
    {"DRUMMER?","WELL?","WE'RE WAITING...","*YAWN*","HELLO?","ANY TIME NOW"},
    {"VICTORY!","THE BANNER IS OURS!","HOME FOR SUPPER!","WHAT A RHYTHM!","AGAIN! AGAIN!","WELL DRUMMED!"},
    {"A SCOUT! STOP IT!","IT'S RUNNING!","DON'T LET IT REACH THE FORT!","SLINGERS!","AFTER IT!","QUICK, SHOOT!"},
    {"THEY RANG THE ALARM!","HERE THEY COME...","MORE OF THEM!","THAT SCOUT GOT THROUGH","BRACE FOR COMPANY!","DRAT."},
    {"A FORT!","WALLS AHEAD","BREAK THE GATE!","LOOK AT THOSE TOWERS","CRUSH IT OPEN!","SIEGE TIME!"},
    {"THE GATE IS DOWN!","WE'RE IN!","CHARGE THE BREACH!","SPLINTERS!","OPEN SESAME","ONWARD, INSIDE!"},
};
static void say_by(int kind,int unit,int force){
    if(!force && (say_cool>0 || rand()%100>=45)) return;
    if(unit<0){ int c[DG_MAXU],n=0; for(int i=0;i<DG_MAXU;i++) if(G.u[i].on&&G.u[i].hp>0) c[n++]=i; if(!n) return; unit=c[rand()%n]; }
    for(int k=0;k<3;k++) if(BB[k].t>0 && BB[k].unit==unit) BB[k].t=0;           /* one bubble per head */
    int k=0; for(int i=0;i<3;i++) if(BB[i].t<BB[k].t) k=i;
    BB[k]=(bubble_t){unit,LINES[kind][rand()%6],2.2f}; say_cool=2.5f;
}
static int unit_of(int cls){ for(int i=0;i<DG_MAXU;i++) if(G.u[i].on&&G.u[i].hp>0&&G.u[i].cls==cls) return i; return -1; }
static void say(int kind){ say_by(kind,-1,0); }
static void say_cmd(int cmd){   /* class flavour first */
    static const char*W_GUARD="BEHIND ME!", *M_RALLY="MENDING!", *B_STRIKE="SMASH!", *S_STRIKE="NICE SHOT!";
    int u=-1; const char*s=0;
    if(cmd==CMD_GUARD && (u=unit_of(CL_WARDEN))>=0 && rand()%3==0) s=W_GUARD;
    if(cmd==CMD_RALLY && (u=unit_of(CL_MENDER))>=0 && rand()%2==0) s=M_RALLY;
    if(cmd==CMD_STRIKE && rand()%3==0){ if((u=unit_of(CL_BREAKER))>=0) s=B_STRIKE; else if((u=unit_of(CL_SLINGER))>=0) s=S_STRIKE; }
    if(s && say_cool<=0){ int k=0; for(int i=0;i<3;i++) if(BB[i].t<BB[k].t) k=i; BB[k]=(bubble_t){u,s,2.0f}; say_cool=2.5f; return; }
    say(cmd==CMD_MARCH?SAY_MARCH:cmd==CMD_STRIKE?SAY_STRIKE:cmd==CMD_GUARD?SAY_GUARD:cmd==CMD_RETREAT?SAY_RETREAT:SAY_RALLY);
}
static void draw_bubbles(int sh){
    for(int k=0;k<3;k++){ bubble_t*b=&BB[k]; if(b->t<=0) continue; const unit_t*u=&G.u[b->unit]; if(!u->on||u->hp<=0){ b->t=0; continue; }
        int sc=strlen(b->s)>16?1:2, w=text_w(b->s,sc)+16, h=sc==2?26:18;
        int ux=sxw(u->x), uy=GROUND+(int)dg_unit_y(&G,b->unit)+sh-70-k*4;
        int x=ux-w/2; if(x<6) x=6; if(x+w>UW-6) x=UW-6-w; int y=uy-h;
        if(y<PF_Y+90) y=PF_Y+90;                                     /* below the boss bar / banner */
        uint32_t bg = b->t<0.25f ? lerpc(0x120E0C,0xF4ECDC,b->t/0.25f) : 0xF4ECDC;
        tri(ux-6,y+h-1,ux+6,y+h-1,ux,y+h+9,bg); rbox(x,y,w,h,8,bg);
        text(x+8,y+(h-7*sc)/2,b->s,sc,0x201810); }
}

/* battle logic, every frame */
static void show_banner(const char*a,const char*b){ banner=a; banner2=b; banner_t=2.8f; }
static void battle_events(void){
    if(G.ev_scout_flee) say_by(SAY_SCOUT,-1,1);
    if(G.ev_alarm){ say_by(SAY_ALARM,-1,1); show_banner("ALARM!","REINFORCEMENTS AT THE FORT"); for(int i=0;i<3;i++) snd_at(S_TONE,now_sample()+DR_RATE/6*i,1,220); }
    if(G.ev_gate_down){ say_by(SAY_GATE,-1,1); snd(S_QUAKE,1,0); shake=1.0f; }
    { static int seen_gate[DG_MAXE]; static const mission_t*sm; if(sm!=G.m){ memset(seen_gate,0,sizeof seen_gate); sm=G.m; }
      for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&G.e[k]; if(!e->on||e->hp<=0||!(e->traits&TR_GATE)||seen_gate[k]||G.time<1) continue;
        if(dg_front(e)-G.sx<560){ seen_gate[k]=1; say_by(SAY_FORT,-1,1);
            show_banner(e->type==ET_IRONGATE?"THE FORTRESS":"OUTPOST AHEAD",e->type==ET_IRONGATE?"CRUSH THE IRON GATE":"BURN OR BREAK THE GATE"); } } }
    { static int bseen=-1; int b=dg_boss(&G); if(b>=0 && b!=bseen && dg_front(&G.e[b])-G.sx<600){ bseen=b;
        const boss_t*bo=&DG_BOSS[DG_ETYPE[G.e[b].type].boss]; show_banner(bo->name,bo->lore); } }
    if(G.ev_kill) say(SAY_KILL);
    if(G.ev_boss_down) say_by(SAY_BOSS_DOWN,-1,1);
    if(G.ev_windup && G.ev_boss_move){ int b=dg_boss(&G); const bmove_t*mv=b>=0?dg_boss_move(&G,b):0;
        if(mv && mv->windup>=2) say_by(SAY_TELEGRAPH,-1,1); }
    if(G.ev_hurt){ for(int i=0;i<DG_MAXU;i++) if(G.u[i].on&&G.u[i].hp>0&&G.u[i].hp<G.u[i].maxhp*0.3f){ say_by(SAY_HURT,i,0); break; } }
    { static int seen=-1; int b=dg_boss(&G); if(b>=0 && b!=seen && dg_front(&G.e[b])-G.sx<520){ seen=b; say_by(SAY_BOSS_SEEN,-1,1); } }
    if(G.ev_attack) snd(S_HIT,0.25f,0);
    if(G.ev_hit) snd(S_HIT,0.6f,0);
    if(G.ev_kill){ snd(S_KILL,1,0); }
    if(G.ev_hurt) snd(S_HURT,1,0);
    if(G.ev_heal){ snd(S_HEAL,1,0); for(int i=0;i<DG_MAXU;i++) if(G.u[i].on&&G.u[i].hp>0) burst(G.u[i].x,-30,4,0x80FF90,40,3,1); }
    if(G.ev_windup && G.ev_boss_move) snd(S_WARN,1,0);
    if(G.ev_roar){ snd(S_ROAR,1.2f,0); shake=1.0f; }
    if(G.ev_quake||G.ev_boss_land){ snd(S_QUAKE,G.ev_quake?1:0.6f,0); shake=G.ev_quake?1.2f:0.7f; }
    if(G.ev_summon) snd(S_ROAR,0.5f,0);
    if(G.ev_boss_down){ snd(S_ROAR,1,0); shake=1.5f; }
    if(G.ev_drop && ngot<16){ got[ngot++]=G.ev_drop-1; snd(S_TONE,1,1320); }
    G.ev_attack=G.ev_hit=G.ev_kill=G.ev_hurt=G.ev_heal=G.ev_windup=G.ev_boss_move=G.ev_roar=G.ev_quake=G.ev_boss_land=G.ev_summon=G.ev_boss_down=G.ev_drop=0;
    G.ev_scout_flee=G.ev_alarm=G.ev_gate_down=0;
    G.ev_block=0;
}
static void resume_play(void){
    paused=0; rc.nseq=0;
    if(music_live) music_reanchor();
    else { int64_t t=now_sample()+DR_RATE; rc_init(&rc,120,t); last_beat=-1; bk_t0=t; bk_fever=0; bk_on=1; }
}
static void hit_fx(void){ /* sparks where enemies just got hit: cheap approximation from flash */
    for(int k=0;k<DG_MAXE;k++){ enemy_t*e=&G.e[k]; if(!e->on) continue;
        if(e->flash>0.24f){ burst(dg_front(e)+10,e->y-30-(DG_ETYPE[e->type].boss>=0?50:0),5,0xFFE0A0,90,2,1); }
        if(e->hp<=0 && e->flash>0.2f){ burst(e->x,e->y-30,14,DG_ETYPE[e->type].color,120,4,1); e->flash=0.19f; } }
}
static void test_hit(int src,int64_t t);
static void drum(int d,int64_t t,int src){
    if(st==ST_TEST){ test_hit(src,t); return; }
    if(st!=ST_BATTLE || paused || end_t>0) return;
    pad_t[d]=0.15f;
    snd(S_DUM+d,1,0);
    int j=rc_hit(&rc,d,t-(int64_t)off_src[src]*DR_RATE/1000);
    judge=j; judge_t=0.5f;
    if(j==J_MISS) snd(S_MISS,1,0);
    if(rc.ev_cmd!=CMD_NONE){ int c=rc.ev_cmd; rc.ev_cmd=CMD_NONE; say_cmd(c);
        static const float PITCH[CMD_N]={260,320,220,200,290};
        for(int i=0;i<4;i++) snd_at(S_CHANT,rc_beat_time(&rc,rc.resp_beat+i),0.9f,PITCH[c]*(DR_PATTERN[c][i]==DR_KA||DR_PATTERN[c][i]==DR_CHI?1.25f:1)); }
    if(rc.ev_fever_on){ rc.ev_fever_on=0; bk_fever=1; say_by(SAY_FEVER,-1,1); for(int i=0;i<4;i++) snd_at(S_TONE,now_sample()+DR_RATE/12*i,0.8f,660+i*220); }
    if(rc.ev_break||rc.ev_fever_off){ if(rc.ev_break) say(SAY_BREAK); rc.ev_break=0; rc.ev_fever_off=0; bk_fever=0; }
}
static void battle_tick(float dt){
    if(paused) return;
    int64_t ns=now_sample();
    { static int seen; if(music_live && beat_ver!=seen && rc.nseq==0 && rc_responding(&rc,ns,0)==CMD_NONE){ seen=beat_ver; music_reanchor(); } }
    { int mo=off_src[0]; for(int i=1;i<SRC_N;i++) if(off_src[i]>mo) mo=off_src[i];   /* lenient: the latest input */
      rc_tick(&rc,ns-(int64_t)mo*DR_RATE/1000); }
    if(rc.ev_break||rc.ev_fever_off){ rc.ev_break=0; rc.ev_fever_off=0; bk_fever=0; }
    int64_t cb=(ns-rc.t0)>=0 ? (ns-rc.t0)/rc.beat : -1;
    while(last_beat<cb && end_t<=0){ last_beat++;
        int bi=0; int cmd=rc_responding(&rc,rc_beat_time(&rc,last_beat),&bi);
        dg_beat(&G,last_beat,cmd,bi,rc.fever);
        static int64_t quiet; if(cmd!=CMD_NONE || rc.nseq>0) quiet=last_beat;          /* idle drummer */
        if(last_beat-quiet>=12 && last_beat>8){ quiet=last_beat; say(SAY_IDLE); } }
    say_cool-=dt; for(int k=0;k<3;k++) if(BB[k].t>0) BB[k].t-=dt;
    if(banner_t>0) banner_t-=dt;
    if(end_t<=0) dg_step(&G,dt);
    battle_events(); hit_fx();
    if(shake>0) shake-=dt*2.5f;
    if(judge_t>0) judge_t-=dt;
    for(int d=0;d<4;d++) if(pad_t[d]>0) pad_t[d]-=dt;
    for(int k=0;k<160;k++){ part_t*p=&P[k]; if(p->life<=0) continue; p->life-=dt; p->x+=p->vx*dt; p->y+=p->vy*dt; p->vy+=200*dt; }
    if((G.won||G.lost) && end_t<=0){ end_t=0.01f; bk_on=0; if(G.won) say_by(SAY_WIN,-1,1); }
    if(end_t>0){ end_t+=dt; if(end_t>2.5f) end_battle(G.won); }
}

/* ======================================================================= camp */
static void draw_mini_unit(int cx,int gy,const loadout_t*l,int sc){ draw_unit(cx,gy,l->cls,l->weapon,l->shield,0,0,0,0,sc,1); }
static const char*matchup(const weapon_t*w){
    static char b[64];
    const char*t = w->dtype==DT_PIERCE?"PIERCE: WEAK VS ARMOUR" : w->dtype==DT_CRUSH?"CRUSH: BREAKS ARMOUR" : "SLASH: ALL-ROUND";
    const char*e = w->elem==EL_FIRE?", BURNS" : w->elem==EL_FROST?", SLOWS FOES" : w->elem==EL_STORM?", CHAINS" : "";
    snprintf(b,sizeof b,"%s%s",t,e); return b; }
static void draw_camp(void){
    rect(0,TOP,CW,CH-TOP,0x120E0C);
    for(int y=TOP;y<TOP+70;y++){ uint16_t k=rgb(lerpc(0x3A1A10,0x120E0C,(float)(y-TOP)/70)); for(int x=0;x<CW;x++) CV(x,y)=k; }
    textcs(CW/2,TOP+10,"DRUMFOLK",5,0xE8731C);
    rbox(120,TOP+48,240,20,10,music_opt?0x3A2A1A:0x1E1814); frame(120,TOP+48,240,20,1,music_opt?0xE8731C:0x4A3A2A);
    textc(CW/2,TOP+54,music_opt?"BEAT: MY MUSIC":"BEAT: DRUMS",1,music_opt?0xFFE0A0:0xC0A080);
    for(int i=0;i<DG_NMISSION;i++){ int y=TOP+70+i*50; int locked=i>A.cleared, on=i==sel;
        rbox(12,y,456,44,10,on?0x3A2A1A:0x1E1814); if(on) frame(12,y,456,44,2,0xE8731C);
        char t[48]; snprintf(t,sizeof t,"%d. %s",i+1,DG_MISSION[i].name);
        text(24,y+8,t,2,locked?0x5A4A3A:0xF4E4D0);
        text(24,y+28,locked?"LOCKED":DG_MISSION[i].brief,1,locked?0x4A3A2A:0xC0A080);
        if(i<A.cleared) text(424,y+14,"OK",2,0x80E080); }
    /* scout report */
    int y=TOP+276; text(16,y,"SCOUT REPORT",2,0xE8731C); y+=22;
    const mission_t*m=&DG_MISSION[sel]; int seen[ET_N]={0};
    for(int pass=0;pass<2;pass++) for(int i=m->ne-1;i>=0;i--){ int t=m->e[i].type; if(seen[t]) continue;
        int boss=DG_ETYPE[t].boss>=0; if(pass==0 && !boss) continue; if(pass==1 && boss) continue; seen[t]=1;
        if(y>TOP+446) break;
        text(16,y,DG_ETYPE[t].name,1,boss?0xFF7050:0xF4E4D0); text(130,y,DG_ETYPE[t].hint,1,0xA08870); y+=13; }
    /* squad */
    y=TOP+462; text(16,y-14,"SQUAD - TAP TO EQUIP",1,0xE8731C);
    for(int i=0;i<DG_MAXU;i++){ int x=12+(i%2)*232, yy=y+(i/2)*84; rbox(x,yy,224,78,10,0x1E1814);
        if(i<A.nsquad){ const loadout_t*l=&A.squad[i];
            draw_mini_unit(x+30,yy+70,l,1);
            text(x+60,yy+10,DG_CLASS[l->cls].name,2,DG_CLASS[l->cls].color);
            text(x+60,yy+34,l->weapon>=0?DG_WEAPON[l->weapon].name:"-",1,0xF4E4D0);
            text(x+60,yy+50,DG_SHIELD[l->shield].name,1,l->shield?0xC0A080:0x5A4A3A); }
        else textc(x+112,yy+32,"+ ADD",2,0x8A6A4A); }
    /* buttons */
    y=TOP+636; rbox(12,y,224,52,10,0x2A2018); frame(12,y,224,52,1,0x5A4A3A);
    textc(124,y+10,"RHYTHM TEST",2,0xF4E4D0);
    { char b[48]; snprintf(b,sizeof b,"TOUCH %+d  PAD %+d  KEYS %+d",off_src[0],off_src[1],off_src[2]); textc(124,y+34,b,1,0xC0A080); }
    text(8,TOP+4,"HOLD POWER: LEAVE",1,0x6A5A4A);
    rbox(244,y,224,52,12,0xE8731C); textc(356,y+12,"MARCH!",4,0x101010);
}
static void cycle_weapon(loadout_t*l,int dir){
    for(int n=1;n<=DG_NWEAPON;n++){ int w=((l->weapon<0?0:l->weapon)+dir*n+DG_NWEAPON*4)%DG_NWEAPON;
        if(dg_weapon_ok(l->cls,w) && owned(w)){ l->weapon=w; return; } } }
static void cycle_shield(loadout_t*l,int dir){
    for(int n=1;n<=DG_NSHIELD;n++){ int s=(l->shield+dir*n+DG_NSHIELD*4)%DG_NSHIELD;
        if(dg_shield_ok(l->cls,s) && owned(100+s)){ l->shield=s; return; } } }
static void fix_loadout(loadout_t*l){
    if(!(dg_weapon_ok(l->cls,l->weapon)&&owned(l->weapon))){ l->weapon=-1; cycle_weapon(l,1); }
    if(!(dg_shield_ok(l->cls,l->shield)&&owned(100+l->shield))){ l->shield=0;
        if(!DG_CLASS[l->cls].two_handed) for(int s=DG_NSHIELD-1;s>0;s--) if(dg_shield_ok(l->cls,s)&&owned(100+s)){ l->shield=s; break; } } }
static void draw_edit(void){
    rect(0,TOP,CW,CH-TOP,0x120E0C);
    loadout_t*l=&A.squad[slot]; char b[64];
    snprintf(b,sizeof b,"SQUAD MEMBER %d",slot+1); textcs(CW/2,TOP+12,b,3,0xE8731C);
    const char*lab[3]={"CLASS","WEAPON","SHIELD"};
    for(int r=0;r<3;r++){ int y=TOP+56+r*112; rbox(12,y,456,100,12,0x1E1814);
        text(28,y+8,lab[r],1,0xC0A080);
        rbox(20,y+28,52,52,10,0x3A2A1A); textc(46,y+40,"<",4,0xF4E4D0);
        rbox(408,y+28,52,52,10,0x3A2A1A); textc(434,y+40,">",4,0xF4E4D0);
        if(r==0){ textc(CW/2,y+32,DG_CLASS[l->cls].name,3,DG_CLASS[l->cls].color); textc(CW/2,y+62,DG_CLASS[l->cls].role,1,0xC0A080);
            snprintf(b,sizeof b,"HP %d",DG_CLASS[l->cls].hp); textc(CW/2,y+78,b,1,0x8A6A4A); }
        if(r==1){ if(l->weapon>=0){ const weapon_t*w=&DG_WEAPON[l->weapon];
                textc(CW/2,y+30,w->name,2,w->elem==EL_FIRE?0xFF8040:w->elem==EL_FROST?0x90E0FF:w->elem==EL_STORM?0xFFF060:0xF4E4D0);
                if(w->heal) snprintf(b,sizeof b,"DMG %d  HEAL %d",w->dmg,w->heal); else snprintf(b,sizeof b,"DMG %d  RANGE %d  %s",w->dmg,w->range,DG_ELEM_NAME[w->elem]);
                textc(CW/2,y+54,b,1,0xF4E4D0); textc(CW/2,y+70,matchup(w),1,0xC0A080); } }
        if(r==2){ const shield_t*s=&DG_SHIELD[l->shield]; textc(CW/2,y+30,s->name,2,l->shield?0xF4E4D0:0x8A6A4A);
            if(DG_CLASS[l->cls].two_handed) textc(CW/2,y+56,"TWO-HANDED: NO SHIELD",1,0xC0A080);
            else { snprintf(b,sizeof b,"BLOCK %d%% (X2 ON GUARD)",s->block); textc(CW/2,y+56,b,1,0xF4E4D0);
                textc(CW/2,y+72,s->special&SH_REFLECT?"REFLECTS MISSILES ON GUARD":s->special&SH_THORNS?"HURTS MELEE ATTACKERS":s->special&SH_HEAVY?"HEAVY: SLOWER MARCH":"",1,0xC0A080); } } }
    rbox(12,TOP+396,456,180,12,0x1A1410);
    draw_unit(CW/2,TOP+560,l->cls,l->weapon,l->shield,0,0,0,0,3,1);
    int y=TOP+590; if(A.nsquad>1){ rbox(12,y,180,52,10,0x3A1A14); textc(102,y+18,"REMOVE",2,0xFF8070); }
    rbox(288,y,180,52,12,0xE8731C); textc(378,y+14,"DONE",3,0x101010);
}
static void draw_result(void){
    rect(0,TOP,CW,CH-TOP,0x120E0C);
    textcs(CW/2,TOP+40,victory?"VICTORY!":"RETREAT...",5,victory?0xFFE040:0xFF6050);
    textc(CW/2,TOP+100,DG_MISSION[sel].name,2,0xC0A080);
    int y=TOP+150; if(nnew){ textc(CW/2,y,"NEW GEAR",3,0xE8731C); y+=40;
        for(int i=0;i<nnew && y<TOP+600;i++){ textc(CW/2,y,code_name(newly[i]),2,0xF4E4D0); y+=26; } }
    else textc(CW/2,y,victory?"NOTHING NEW THIS TIME":"THE SQUAD WILL BE BACK",2,0x8A6A4A);
    textc(CW/2,TOP+640,"TAP TO RETURN TO CAMP",2,0xC0A080);
}
static void test_enter(void);
static void tap(int x,int y){
    if(st==ST_CAMP){
        if(y>=TOP+40 && y<TOP+70 && x>=110 && x<370){ music_opt=!music_opt; save_game(); snd(S_TOK,0.7f,0); return; }
        for(int i=0;i<DG_NMISSION;i++){ int yy=TOP+70+i*50; if(y>=yy&&y<yy+44&&i<=A.cleared){ sel=i; snd(S_TOK,0.7f,0); return; } }
        int sy=TOP+462; for(int i=0;i<DG_MAXU;i++){ int xx=12+(i%2)*232, yy=sy+(i/2)*84;
            if(x>=xx&&x<xx+224&&y>=yy&&y<yy+78){ if(i>=A.nsquad){ i=A.nsquad++; A.squad[i]=(loadout_t){CL_LANCER,-1,0}; fix_loadout(&A.squad[i]); }
                slot=i; st=ST_EDIT; snd(S_KA,0.7f,0); return; } }
        int by=TOP+636; if(y>=by&&y<by+52){ if(x<240){ test_enter(); snd(S_TOK,0.6f,0); }
            else if(x>=244){ save_game(); snd(S_DUM,1,0); start_battle(); } }
        return; }
    if(st==ST_EDIT){ loadout_t*l=&A.squad[slot];
        for(int r=0;r<3;r++){ int yy=TOP+56+r*112; if(y<yy||y>=yy+100) continue; int dir = x<120?-1 : x>360?1 : 0; if(!dir) return;
            if(r==0){ l->cls=(l->cls+dir+CL_N)%CL_N; fix_loadout(l); } if(r==1) cycle_weapon(l,dir); if(r==2 && !DG_CLASS[l->cls].two_handed) cycle_shield(l,dir);
            snd(S_TOK,0.6f,0); return; }
        int by=TOP+590; if(y>=by&&y<by+52){
            if(x<192 && A.nsquad>1){ for(int i=slot;i<A.nsquad-1;i++) A.squad[i]=A.squad[i+1]; A.nsquad--; st=ST_CAMP; save_game(); }
            else if(x>=288){ st=ST_CAMP; save_game(); snd(S_KA,0.7f,0); } }
        return; }
    if(st==ST_RESULT){ st=ST_CAMP; if(sel<A.cleared && sel<DG_NMISSION-1 && victory) sel++; return; }
}

/* ======================================================================= rhythm test
 * A metronome (120 BPM, through the same output as the game) and a tap area: every hit is placed
 * against the nearest click. Per input: mean offset = latency to compensate, spread = jitter,
 * interval spread = how regular the taps are. USE THESE stores the offsets. With the click off,
 * only regularity is measured. */
typedef struct { int src, off; } thit_t;
static thit_t TH[24]; static int nth, test_click=1; static int64_t test_t0, test_beat, last_hit[SRC_N];
static int64_t ts_n[SRC_N], ts_sum[SRC_N], ts_sq[SRC_N], ti_n[SRC_N], ti_sum[SRC_N], ti_sq[SRC_N];
static float test_flash, test_src_flash[SRC_N];
static const uint32_t SRC_C[SRC_N]={0xE8731C,0x5AB0F0,0x80E080};
static void test_reset(void){ nth=0; memset(ts_n,0,sizeof ts_n); memset(ts_sum,0,sizeof ts_sum); memset(ts_sq,0,sizeof ts_sq);
    memset(ti_n,0,sizeof ti_n); memset(ti_sum,0,sizeof ti_sum); memset(ti_sq,0,sizeof ti_sq); memset(last_hit,0,sizeof last_hit); }
static void test_enter(void){
    st=ST_TEST; test_reset(); test_beat=DR_RATE/2; test_t0=now_sample()+DR_RATE/2;
    bk_t0=test_t0; bk_beat=test_beat; bk_fever=0; bk_on=test_click;
}
static void test_leave(void){ bk_on=0; st=ST_CAMP; save_game(); }
static void test_hit(int src,int64_t t){
    test_src_flash[src]=0.15f; snd(S_TOK,0.8f,0);
    int64_t rel=t-test_t0, b=(rel+test_beat/2)/test_beat; if(rel<-test_beat/2) b=-1;
    int off=(int)((rel-b*test_beat)*1000/DR_RATE);
    if(test_click){ ts_n[src]++; ts_sum[src]+=off; ts_sq[src]+=(int64_t)off*off;
        memmove(TH+1,TH,sizeof TH-sizeof TH[0]); TH[0]=(thit_t){src,off}; if(nth<24) nth++; }
    if(last_hit[src]){ int iv=(int)((t-last_hit[src])*1000/DR_RATE); if(iv>200 && iv<1500){ ti_n[src]++; ti_sum[src]+=iv; ti_sq[src]+=(int64_t)iv*iv; } }
    last_hit[src]=t;
    if(!test_click){ memmove(TH+1,TH,sizeof TH-sizeof TH[0]); TH[0]=(thit_t){src,0}; if(nth<24) nth++; }
}
static int isd(int64_t n,int64_t sum,int64_t sq){ if(n<2) return 0; int64_t m=sum/n; int64_t v=sq/n-m*m; return v>0?isqrt(v):0; }
#define TA_Y (TOP+64)
#define TA_H 250
static int test_tap(int x,int y,int64_t t){
    if(y>=TA_Y && y<TA_Y+TA_H){ test_hit(SRC_TOUCH,t); return 1; }
    if(y>=TOP+30 && y<TOP+58){ test_click=!test_click; test_reset(); bk_on=test_click; return 1; }
    int by=TOP+636; if(y>=by && y<by+52){
        if(x<160){ for(int s2=0;s2<SRC_N;s2++) if(ts_n[s2]>=8){ int m=(int)(ts_sum[s2]/ts_n[s2]); if(m<-300)m=-300; if(m>400)m=400; off_src[s2]=m; }
            save_game(); snd(S_DUM,1,0); }
        else if(x<320){ test_reset(); snd(S_TOK,0.6f,0); }
        else test_leave();
        return 1; }
    return 0;
}
static void draw_test(float dt){
    rect(0,TOP,CW,CH-TOP,0x120E0C);
    textcs(CW/2,TOP+4,"RHYTHM TEST",3,0xE8731C);
    rbox(140,TOP+32,200,24,10,test_click?0x3A2A1A:0x1E1814); textc(CW/2,TOP+40,test_click?"CLICK: ON (120 BPM)":"CLICK: OFF (REGULARITY)",1,0xF4E4D0);
    /* tap area, flashing on the click */
    int64_t ns=now_sample(); float ph = ns>=test_t0 ? (float)((ns-test_t0)%test_beat)/test_beat : 1;
    if(test_click && ph<0.08f) test_flash=0.12f;
    uint32_t c = test_flash>0 ? 0x5A3A1A : 0x2A2018; rbox(12,TA_Y,456,TA_H,16,c);
    textc(CW/2,TA_Y+90,test_click?"TAP HERE ON THE CLICK":"TAP HERE, STEADY",2,0xF4E4D0);
    textc(CW/2,TA_Y+120,"OR PRESS JOY-CON / R3 KEYS",1,0xC0A080);
    for(int s2=0;s2<SRC_N;s2++){ if(test_src_flash[s2]>0){ circle(80+s2*160,TA_Y+190,22,SRC_C[s2]); test_src_flash[s2]-=dt; } else ring(80+s2*160,TA_Y+190,22,2,SRC_C[s2]);
        textc(80+s2*160,TA_Y+222,SRC_NAME[s2],1,SRC_C[s2]); }
    if(test_flash>0) test_flash-=dt;
    /* hits against the click: -150..+150 ms */
    int gy=TOP+324, gh=200, cx=CW/2; rect(12,gy,456,gh,0x0A0806);
    if(test_click){ rect(cx-(int)(110*1.4f),gy,(int)(220*1.4f),gh,0x1A1410); rect(cx-(int)(50*1.4f),gy,(int)(100*1.4f),gh,0x24201A); rect(cx,gy,1,gh,0xF4E4D0);
        text(16,gy+4,"EARLY",1,0x8A6A4A); text(CW-46,gy+4,"LATE",1,0x8A6A4A); }
    for(int i=0;i<nth;i++){ int x=test_click? cx+(int)(TH[i].off*1.4f) : 24+(i%20)*22; if(x<16) x=16; if(x>CW-16) x=CW-16;
        circle(x,gy+gh-10-i*8,4,SRC_C[TH[i].src]); }
    /* stats */
    int y=TOP+532;
    for(int s2=0;s2<SRC_N;s2++){ char b[96];
        if(test_click && ts_n[s2]) snprintf(b,sizeof b,"%-7s %2d HITS  OFFSET %+4d MS  SPREAD %3d MS  STEADY %3d MS",SRC_NAME[s2],(int)ts_n[s2],
            (int)(ts_sum[s2]/ts_n[s2]),isd(ts_n[s2],ts_sum[s2],ts_sq[s2]),isd(ti_n[s2],ti_sum[s2],ti_sq[s2]));
        else if(ti_n[s2]) snprintf(b,sizeof b,"%-7s %2d HITS  EVERY %4d MS  STEADY %3d MS",SRC_NAME[s2],(int)ti_n[s2]+1,(int)(ti_sum[s2]/ti_n[s2]),isd(ti_n[s2],ti_sum[s2],ti_sq[s2]));
        else snprintf(b,sizeof b,"%-7s -",SRC_NAME[s2]);
        text(16,y+s2*16,b,1,SRC_C[s2]); }
    { char b[64]; snprintf(b,sizeof b,"IN USE: TOUCH %+d  PAD %+d  KEYS %+d MS",off_src[0],off_src[1],off_src[2]); text(16,y+54,b,1,0x8A6A4A); }
    int by=TOP+636; rbox(12,by,148,52,10,0xE8731C); textc(86,by+12,"USE THESE",2,0x101010); textc(86,by+34,"8+ HITS EACH",1,0x3A2010);
    rbox(166,by,148,52,10,0x2A2018); textc(240,by+18,"RESET",2,0xF4E4D0);
    rbox(320,by,148,52,10,0x2A2018); textc(394,by+18,"BACK",2,0xF4E4D0);
}

/* ======================================================================= input */
static int ts_fd=-1, kp_fd=-1, km_fd=-1;
#define NS 10
static struct { int x,y,down,was; } sl[NS]; static int cur_slot;
static int64_t ev_sample(const struct input_event*e){            /* evdev time (monotonic) -> sample clock */
    int64_t eu=(int64_t)e->input_event_sec*1000000+e->input_event_usec, nu=now_us(), d=nu-eu; if(d<0||d>200000) d=0;
    return now_sample()-d*DR_RATE/1000000; }
static int test_tap(int x,int y,int64_t t);
static void press_at(int x,int y,int64_t t){
    if(land) return;                                 /* landscape: no pads, the controller plays */
    static int logged; if(logged<20){ logged++; fprintf(stderr,"[touch] press %d,%d state %d\n",x,y,st); }
    if(st==ST_BATTLE){ if(y>=PAD_Y) for(int d=0;d<4;d++){ int px_,py,w,h; pad_rect(d,&px_,&py,&w,&h); if(x>=px_&&x<px_+w&&y>=py&&y<py+h){ drum(d,t,SRC_TOUCH); return; } }
        if(paused && y<PAD_Y) resume_play();
        return; }
    if(st==ST_TEST && test_tap(x,y,t)) return;
    tap(x,y);
}
/* The R3's touchscreen speaks multitouch protocol A: every frame lists the fingers down, each ended
 * by SYN_MT_REPORT; a lifted finger is just not listed any more (no tracking id -1). Waiting for a
 * release (protocol B) kept the first finger "down" forever, so only the first tap ever counted.
 * Contacts are rebuilt each SYN_REPORT; protocol B (ABS_MT_SLOT) is handled too. */
static void poll_touch(void){
    struct input_event e; static int typeB, fx, fy, fhas, nfc, cx_[NS], cy_[NS], btn=0;
    while(read(ts_fd,&e,sizeof e)==sizeof e){
        { static int raw; if(raw<60){ raw++; fprintf(stderr,"[touch] ev %d %d %d\n",e.type,e.code,e.value); } }
        if(e.type==EV_ABS){
            if(e.code==ABS_MT_SLOT){ typeB=1; cur_slot=e.value<0||e.value>=NS?0:e.value; }
            else if(e.code==ABS_MT_TRACKING_ID){ if(typeB) sl[cur_slot].down=e.value>=0; }
            else if(e.code==ABS_MT_POSITION_X||e.code==ABS_X){ if(typeB&&e.code==ABS_MT_POSITION_X) sl[cur_slot].x=e.value; fx=e.value; fhas=1; }
            else if(e.code==ABS_MT_POSITION_Y||e.code==ABS_Y){ if(typeB&&e.code==ABS_MT_POSITION_Y) sl[cur_slot].y=e.value; fy=e.value; fhas=1; }
        } else if(e.type==EV_KEY && e.code==BTN_TOUCH){ btn=e.value; }
        else if(e.type==EV_SYN && e.code==SYN_MT_REPORT){ if(fhas && nfc<NS){ cx_[nfc]=fx; cy_[nfc]=fy; nfc++; } fhas=0; }
        else if(e.type==EV_SYN && e.code==SYN_REPORT){
            if(!typeB){                                   /* protocol A (or plain single touch) */
                if(fhas && nfc<NS){ cx_[nfc]=fx; cy_[nfc]=fy; nfc++; } fhas=0;
                if(!btn) nfc=0;
                for(int i=0;i<NS;i++){ sl[i].down=i<nfc; if(i<nfc){ sl[i].x=cx_[i]; sl[i].y=cy_[i]; } } }
            else if(!btn) for(int i=0;i<NS;i++) sl[i].down=0;
            nfc=0;
            int64_t t=ev_sample(&e);
            for(int i=0;i<NS;i++){ if(sl[i].down && !sl[i].was) press_at(sl[i].x,sl[i].y,t); sl[i].was=sl[i].down; } }
    }
}
static int64_t pwr_t; static int pwr_down;
static void test_leave(void);
static void short_back(void){                                       /* short power / Joy-Con minus: pause, or back to camp */
    if(st==ST_BATTLE && end_t<=0){ if(!paused){ paused=1; bk_on=0; } else resume_play(); }
    else if(st==ST_TEST) test_leave();
    else if(st!=ST_CAMP && st!=ST_BATTLE){ if(st==ST_LISTEN) music_stop(); st=ST_CAMP; save_game(); }
}
static void poll_keys(void){
    struct input_event e;
    while(kp_fd>=0 && read(kp_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY) continue;
        if(e.code==116){ fprintf(stderr,"[keys] power %d\n",e.value); if(e.value==1){ pwr_down=1; pwr_t=now_us(); }
            else if(e.value==0 && pwr_down){ pwr_down=0; if(now_us()-pwr_t<1000000) short_back(); } }
        else if(e.code==165 && e.value==1) drum(DR_DUM,ev_sample(&e),SRC_KEYS); }
    if(pwr_down && now_us()-pwr_t>=1200000){ pwr_down=0;            /* hold power: leave Drumfolk, from anywhere */
        if(st==ST_BATTLE && end_t<=0){ bk_on=0; end_battle(0); }
        fprintf(stderr,"[r3drum] power held: leaving\n"); music_stop(); quit=1; }
    while(km_fd>=0 && read(km_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY || e.value!=1) continue;
        if(e.code==163) drum(DR_KA,ev_sample(&e),SRC_KEYS);
        else if(e.code==164) drum(DR_TOK,ev_sample(&e),SRC_KEYS);
        else if(e.code==115||e.code==114){ vol+= e.code==115?12:-12; if(vol<0)vol=0; if(vol>255)vol=255; hw_volume(); vb_save(vol); vb_until=r3_tick_ms()+2000; } }
}
/* Drums by the buttons' physical places, matching the pads on screen:
 * bottom = DUM (bottom-left pad), right = KA (bottom-right), top = CHI (top-right), left = TOK (top-left).
 * pad.c names a sideways Joy-Con (L)'s Down/Right/Left/Up A/X/B/Y; standard pads are A south, B east,
 * X north, Y west. Polled every 2 ms between frames (see main), so a press is timed within ~2 ms. */
static void poll_pads(void){
    int a; pad_poll();
    int jc=0; for(int i=0;i<pad_count();i++){ const char*n=pad_name(i); if(n && strstr(n,"Joy-Con")) jc=1; }
    while(pad_next(&a)){ int64_t t=now_sample(); int d=-1;
        if(jc){ if(a==PA_A) d=DR_DUM; else if(a==PA_X) d=DR_KA; else if(a==PA_Y) d=DR_CHI; else if(a==PA_B) d=DR_TOK; }
        else  { if(a==PA_A) d=DR_DUM; else if(a==PA_B) d=DR_KA; else if(a==PA_X) d=DR_CHI; else if(a==PA_Y) d=DR_TOK; }
        if(d>=0){ drum(d,t,SRC_PAD); continue; }
        if(a==PA_HOME||a==PA_PLUS||a==PA_MINUS) short_back(); }
}

static void on_crash(int sig){ fprintf(stderr,"[r3drum] crash %d\n",sig); _exit(128+sig); }

/* --shot SCENE OUT.ppm: draw one scene into a picture, no screen / sound (art checks on a PC).
 * SCENE: camp | edit | battle:M:X[:boss] (mission M, squad at x X, boss telegraphing) */
static void write_ppm(const char*path){ FILE*f=fopen(path,"wb"); if(!f) return; int t0=land?0:TOP; fprintf(f,"P6 %d %d 255\n",CW,CH-t0);
    for(int y=t0;y<CH;y++) for(int x=0;x<CW;x++){ uint16_t c=CV(x,y); fputc((c>>11)<<3,f); fputc(((c>>5)&63)<<2,f); fputc((c&31)<<3,f); }
    fclose(f); }
static int shot(const char*scene,const char*out){
    dg_armory_default(&A); A.cleared=3; A.weapons=0xFFFFFFFFu; A.shields=0xFFu;
    if(!strcmp(scene,"camp")) draw_camp();
    else if(!strcmp(scene,"edit")){ slot=1; A.squad[1]=(loadout_t){CL_LANCER,4,5}; draw_edit(); }
    else if(!strcmp(scene,"test")){ test_enter(); for(int i=0;i<14;i++){ test_hit(i%3==0?SRC_PAD:SRC_TOUCH,test_t0+i*test_beat+(i%3==0?7200:1200+(i%4)*700)); } draw_test(0); }
    else if(!strncmp(scene,"battle:",7)){ int m=0,x=600; char boss[8]=""; sscanf(scene+7,"%d:%d:%7s",&m,&x,boss); if(strstr(scene,"land")){ set_mode(1); if(!strncmp(boss,"land",4)) boss[0]=0; }
        sel=m; dg_start(&G,m,&A,7); rc_init(&rc,120,0); G.sx=(float)x; cam=G.sx-150;
        for(int i=0;i<90;i++) dg_step(&G,1/60.0f);
        for(int k=0;k<DG_MAXE;k++) if(G.e[k].on&&(G.e[k].traits&TR_SCOUT)&&fabsf(G.e[k].x-G.sx)<300) G.e[k].fleeing=1;
        if(boss[0]=='b'){ int b=dg_boss(&G); if(b>=0){ G.e[b].move=1; G.e[b].windup=7; } G.guard_until=(int)G.beat+4; }
        BB[0]=(bubble_t){1,LINES[boss[0]?SAY_TELEGRAPH:SAY_MARCH][0],1}; G.time= x<700 ? 5 : 12;
        draw_battle(); }
    else { fprintf(stderr,"unknown scene %s\n",scene); return 2; }
    write_ppm(out); return 0;
}
int main(int argc,char**argv){
    setvbuf(stderr,NULL,_IONBF,0); signal(SIGSEGV,on_crash); signal(SIGBUS,on_crash); signal(SIGFPE,on_crash);
    synth_init();
    if(argc>3 && !strcmp(argv[1],"--shot")) return shot(argv[2],argv[3]);
    if(argc>3 && !strcmp(argv[1],"--audiotest")){        /* r3drum --audiotest PERIOD COUNT: 4 s of beat */
        struct pcm_config cfg; memset(&cfg,0,sizeof cfg); int P=atoi(argv[2]), N=atoi(argv[3]);
        cfg.channels=2; cfg.rate=DR_RATE; cfg.period_size=P; cfg.period_count=N; cfg.format=PCM_FORMAT_S16_LE;
        if(argc<=4){ cfg.start_threshold=P; cfg.stop_threshold=P*N; cfg.avail_min=P; }   /* "def" or a rate: tinyalsa defaults, like r3player */
        if(argc>4 && atoi(argv[4])>=8000) cfg.rate=(unsigned)atoi(argv[4]);
        if(P==0){ /* mixing only, no DAC: how much CPU does the synth take */
            bk_t0=DR_RATE/4; bk_beat=DR_RATE/2; bk_on=1; static int16_t b[1024*2]; struct timespec c0,c1;
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&c0); int64_t t0=now_us();
            while(written<4*DR_RATE){ mix_block(b,256); written+=256; }
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&c1);
            printf("mix only: 4 s of audio in %.2f s wall, %.2f s cpu\n",(now_us()-t0)/1e6,(c1.tv_sec-c0.tv_sec)+(c1.tv_nsec-c0.tv_nsec)/1e9); return 0; }
        pcm=pcm_open(0,0,PCM_OUT,&cfg); if(!pcm||!pcm_is_ready(pcm)){ printf("%dx%d refused: %s\n",P,N,pcm?pcm_get_error(pcm):"?"); return 1; }
        mx=mixer_open(0); dac_route(); hw_volume();
        bk_t0=DR_RATE/4; bk_beat=DR_RATE/2; bk_on=1; static int16_t b[4096*2]; int errs=0, writes=0;
        struct timespec c0,c1; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&c0); int64_t t0=now_us();
        while(written<4*DR_RATE){ mix_block(b,P); int r=pcm_write(pcm,b,P*4); writes++; if(r){ errs++; pcm_prepare(pcm); } written+=P; }
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&c1);
        printf("%dx%d: %d writes, %d failed, %.2f s wall, %.2f s cpu\n",P,N,writes,errs,(now_us()-t0)/1e6,
               (c1.tv_sec-c0.tv_sec)+(c1.tv_nsec-c0.tv_nsec)/1e9);
        pcm_close(pcm); return 0; }
    load_game(); for(int i=0;i<A.nsquad;i++) fix_loadout(&A.squad[i]);
    int fd=open("/dev/fb0",O_RDWR); struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if(fd<0||ioctl(fd,FBIOGET_VSCREENINFO,&vi)||ioctl(fd,FBIOGET_FSCREENINFO,&fi)){ fprintf(stderr,"no fb\n"); return 1; }
    vi.xoffset=0; vi.yoffset=0; ioctl(fd,FBIOPAN_DISPLAY,&vi);
    fb=mmap(0,fi.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); fbstride=fi.line_length/2;
    for(int y=0;y<TOP;y++) memset(fb+y*fbstride,0,CW*2);    /* rows under the bezel: no leftovers */
    int clk=CLOCK_MONOTONIC;
    ts_fd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK); kp_fd=open("/dev/input/event0",O_RDONLY|O_NONBLOCK); km_fd=open("/dev/input/event2",O_RDONLY|O_NONBLOCK);
    if(ts_fd>=0) ioctl(ts_fd,EVIOCSCLOCKID,&clk); if(kp_fd>=0) ioctl(kp_fd,EVIOCSCLOCKID,&clk); if(km_fd>=0) ioctl(km_fd,EVIOCSCLOCKID,&clk);
    vol=vb_load();
    mx=mixer_open(0);
    bt_fd=bt_open(); if(bt_fd<0) open_dac();

    anchor_us=now_us();
    pthread_t at; pthread_create(&at,NULL,audio_thread,NULL);
    /* no real-time priority: a misbehaving audio loop must never be able to starve the device */
    fprintf(stderr,"[r3drum] start, sound %s, cleared %d\n",pcm||bt_fd>=0?out_name:"off",A.cleared);
    if(argc>2 && !strcmp(argv[1],"--battle")){ sel=atoi(argv[2]); if(sel<0||sel>=DG_NMISSION) sel=0; start_battle(); }   /* testing */
    int64_t last=now_us(); uint32_t vt=0;
    while(!quit){
        int64_t t0=now_us(); float dt=(float)(t0-last)/1e6f; if(dt>0.1f) dt=0.1f; last=t0;
        poll_touch(); poll_keys(); poll_pads();
        if(st==ST_BATTLE) battle_tick(dt);
        { int want=(st==ST_BATTLE||st==ST_LISTEN) && pad_count()>0; if(want!=land) set_mode(want); }
        if(st==ST_LISTEN) listen_tick(dt);
        if(st==ST_BATTLE) draw_battle(); else if(st==ST_CAMP) draw_camp(); else if(st==ST_EDIT) draw_edit();
        else if(st==ST_LISTEN) draw_listen(); else if(st==ST_TEST) draw_test(dt); else draw_result();
        present();
        if(r3_tick_ms()<vb_until) vb_draw(fb,fbstride,land,vol);
        if(++vt%60==0) hw_volume();
        /* ~40 fps drawing; inputs polled every 2 ms in between (the Joy-Con has no timestamps) */
        int64_t until=t0+1000000/40;
        while(now_us()<until && !quit){ poll_pads(); poll_touch(); poll_keys(); usleep(2000); }
    }
    fprintf(stderr,"[r3drum] leaving: quit=%d st=%d land=%d paused=%d nsquad=%d pads=%d\n",quit,st,land,paused,A.nsquad,pad_count());
    save_game(); music_stop();
    audio_run=0; pthread_join(at,NULL);
    if(pcm) pcm_close(pcm); if(mx) mixer_close(mx); bt_close();
    memset(cvbuf,0,sizeof cvbuf); present();
    fprintf(stderr,"[r3drum] quit\n");
    return 0;
}
