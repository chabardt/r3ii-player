/* r3gb — Game Boy (DMG) player for the HiBy R3 Pro II, on Peanut-GB + minigb_apu (MIT).
 *
 * Launched by r3player (Games > Game Boy) with a ROM path; owns the screen and input
 * until it exits. The player's own ROMs only. Battery saves: <rom>.sav next to the ROM.
 *
 *   y   0..431  game 160x144 x3 (classic green)
 *   y 432..719  touch deck: D-pad (slide between directions), B, A, Select, Start
 * Controllers (pad.c): A/B, X or SR = Start, Y or SL = Select, stick/d-pad, Capture = pause.
 * R3 keys: next = A, prev = B, play = Start, volume +/-; power = pause, hold power = save+quit.
 * Sound: 48 kHz stereo to the DAC; the blocking writes pace emulation to real time. */
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 48000
#endif
#include "minigb_apu.h"
static struct minigb_apu_ctx apu;
static uint8_t audio_read(const uint16_t addr){ return minigb_apu_audio_read(&apu,addr); }
static void audio_write(const uint16_t addr,const uint8_t val){ minigb_apu_audio_write(&apu,addr,val); }
#include "peanut_gb.h"
#include "pad.h"
#include "volbar.h"
#include "r3hw.h"
static uint32_t vb_until;          /* volume bar visible until */
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
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
#include <signal.h>
#include <sys/time.h>
static volatile sig_atomic_t got_alarm;
static void on_alarm(int s){ (void)s; got_alarm=1; }

#define SW 480
#define SH 720
#define GY 432                       /* deck top */
static uint16_t *fb; static int fbstride;
static uint32_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u); }
uint32_t r3_tick_ms(void){ return now_ms(); }          /* for pad.c */

/* ---------- cartridge ---------- */
static uint8_t *rom, *cram; static size_t rom_len, cram_len; static int cram_dirty;
static char save_path[1024];
static uint8_t rom_read(struct gb_s*g,const uint_fast32_t a){ (void)g; return a<rom_len?rom[a]:0xFF; }
static uint8_t cram_read(struct gb_s*g,const uint_fast32_t a){ (void)g; return a<cram_len?cram[a]:0xFF; }
static void cram_write(struct gb_s*g,const uint_fast32_t a,const uint8_t v){ (void)g; if(a<cram_len && cram[a]!=v){ cram[a]=v; cram_dirty=1; } }
static void gb_err(struct gb_s*g,const enum gb_error_e e,const uint16_t a){ (void)g; fprintf(stderr,"[r3gb] emulation error %d at %04x\n",e,a); }
static void save_ram(void){
    if(!cram_len || !cram_dirty) return;
    char tmp[1100]; snprintf(tmp,sizeof tmp,"%s.tmp",save_path);
    FILE*f=fopen(tmp,"wb"); if(!f) return;
    int ok=fwrite(cram,1,cram_len,f)==cram_len; ok&=fflush(f)==0; fsync(fileno(f)); fclose(f);
    if(ok && rename(tmp,save_path)==0) cram_dirty=0;      /* never leave a half-written save */
}

/* ---------- video ---------- */
static const uint32_t SHADE[4]={0xE0F0C8,0x8CBC6C,0x3C6E50,0x0E2218};   /* light..dark */
static uint16_t pal[3][4];
static inline uint16_t rgb(uint32_t c){ return (uint16_t)(((c>>8)&0xF800)|((c>>5)&0x07E0)|((c>>3)&0x1F)); }
static void draw_line(struct gb_s*g,const uint8_t*px,const uint_fast8_t line){ (void)g;
    uint16_t row[SW];
    for(int x=0;x<160;x++){ uint16_t c=pal[(px[x]&LCD_PALETTE_ALL)>>4][px[x]&3]; row[x*3]=row[x*3+1]=row[x*3+2]=c; }
    for(int k=0;k<3;k++){ int r=line*3+k; if(r<VB_THICK && now_ms()<vb_until) continue;   /* under the volume bar */
        memcpy(fb+r*fbstride,row,sizeof row); }
}

/* ---------- deck (touch controls) ---------- */
static inline void put(int x,int y,uint16_t c){ if(x>=0&&x<SW&&y>=GY&&y<SH) fb[y*fbstride+x]=c; }
static void fill(int x0,int y0,int w,int h,uint16_t c){ for(int y=y0;y<y0+h;y++) for(int x=x0;x<x0+w;x++) put(x,y,c); }
static void disc(int cx,int cy,int r,uint16_t c){ for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++) if(x*x+y*y<=r*r) put(cx+x,cy+y,c); }
static void rrect(int x0,int y0,int w,int h,int r,uint16_t c){
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){ int dx=x<r?r-x:(x>=w-r?x-(w-r-1):0), dy=y<r?r-y:(y>=h-r?y-(h-r-1):0);
        if(dx*dx+dy*dy<=r*r) put(x0+x,y0+y,c); } }
/* 5x7 glyphs for the labels */
static const char*GL="ABCELRST"; static const uint8_t GLYPH[8][7]={
 {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
 {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}};
static void text(int x,int y,const char*s,int sc,uint16_t c){
    for(;*s;s++,x+=6*sc){ const char*p=strchr(GL,*s); if(!p) continue; const uint8_t*g=GLYPH[p-GL];
        for(int r=0;r<7;r++) for(int b=0;b<5;b++) if(g[r]&(0x10>>b)) fill(x+b*sc,y+r*sc,sc,sc,c); } }
#define DPX 112
#define DPY 572
#define AX 412
#define AY 548
#define BX 318
#define BY 604
static int held_touch;   /* JOYPAD_* bits currently pressed on the deck */
static void draw_deck(void){
    uint16_t bg=rgb(0x0A0A0A), pad_c=rgb(0x2A221A), acc=rgb(0xE8731C), hi=rgb(0xF4E4D0), dim=rgb(0x8A6A4A);
    fill(0,GY,SW,SH-GY,bg);
    fill(0,GY,SW,2,dim);
    /* D-pad */
    uint16_t up=held_touch&JOYPAD_UP?acc:pad_c, dn=held_touch&JOYPAD_DOWN?acc:pad_c, lf=held_touch&JOYPAD_LEFT?acc:pad_c, rt=held_touch&JOYPAD_RIGHT?acc:pad_c;
    rrect(DPX-30,DPY-96,60,70,10,up); rrect(DPX-30,DPY+26,60,70,10,dn);
    rrect(DPX-96,DPY-30,70,60,10,lf); rrect(DPX+26,DPY-30,70,60,10,rt); fill(DPX-30,DPY-30,60,60,pad_c);
    /* A / B */
    disc(AX,AY,46,held_touch&JOYPAD_A?hi:acc); text(AX-7,AY-10,"A",3,bg);
    disc(BX,BY,46,held_touch&JOYPAD_B?hi:acc); text(BX-7,BY-10,"B",3,bg);
    /* Select / Start */
    rrect(196,678,80,30,15,held_touch&JOYPAD_SELECT?acc:pad_c); text(206,686,"SELECT",2,hi);
    rrect(292,678,80,30,15,held_touch&JOYPAD_START?acc:pad_c);  text(304,686,"START",2,hi);
}
/* which buttons does a finger at (x,y) press */
static int hit(int x,int y){
    if(y<GY) return 0;
    int dx=x-DPX, dy=y-DPY;
    if(dx*dx+dy*dy<120*120){ int b=0;                  /* D-pad: 8-way by angle, dead centre */
        if(abs(dx)<14 && abs(dy)<14) return 0;
        if(dy<-abs(dx)/2) b|=JOYPAD_UP; if(dy>abs(dx)/2) b|=JOYPAD_DOWN;
        if(dx<-abs(dy)/2) b|=JOYPAD_LEFT; if(dx>abs(dy)/2) b|=JOYPAD_RIGHT; return b; }
    if((x-AX)*(x-AX)+(y-AY)*(y-AY)<62*62) return JOYPAD_A;
    if((x-BX)*(x-BX)+(y-BY)*(y-BY)<62*62) return JOYPAD_B;
    if(y>=664 && x>=186 && x<284) return JOYPAD_SELECT;
    if(y>=664 && x>=284 && x<382) return JOYPAD_START;
    return 0;
}
#define NS R3_NS
static r3_contact_t sl[NS]; static r3_touch_t tstate;     /* r3hw.h: protocol A or B */
static int ts_fd=-1, kp_fd=-1, km_fd=-1;
static void poll_touch(void){
    struct input_event e;
    while(read(ts_fd,&e,sizeof e)==sizeof e){
        r3_touch_event(&tstate,&e,sl);
    }
    int b=0; for(int i=0;i<NS;i++) if(sl[i].down) b|=hit(sl[i].x,sl[i].y);
    if(b!=held_touch){ held_touch=b; draw_deck(); }
}

/* ---------- DAC + volume (same controls r3player uses) ---------- */
static struct mixer *mx; static int vol=160;
static void hw_volume(void){ if(!mx) return; int hw=255-(vol<0?0:vol>255?255:vol);
    struct mixer_ctl*l=mixer_get_ctl_by_name(mx,"Left Playback Volume"),*r=mixer_get_ctl_by_name(mx,"Right Playback Volume");
    if(l) mixer_ctl_set_value(l,0,hw); if(r) mixer_ctl_set_value(r,0,hw); }
static void dac_route(void){ r3_dac_route(mx); }     /* balanced 4.4 mm or 3.5 mm, like r3player */

/* ---------- main ---------- */
static int paused, quit;
static uint32_t pwr_t; static int pwr_down;
static void poll_keys(int*bits){
    struct input_event e;
    while(kp_fd>=0 && read(kp_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY) continue;
        if(e.code==116){ if(e.value==1){ pwr_down=1; pwr_t=now_ms(); }
            else if(e.value==0 && pwr_down){ pwr_down=0; if(now_ms()-pwr_t<1000) paused=!paused; } }
        else if(e.code==165) *bits = e.value ? (*bits|JOYPAD_B) : (*bits&~JOYPAD_B); }
    if(pwr_down && now_ms()-pwr_t>=1200) quit=1;
    while(km_fd>=0 && read(km_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY) continue;
        if(e.code==163) *bits = e.value ? (*bits|JOYPAD_A) : (*bits&~JOYPAD_A);
        else if(e.code==164) *bits = e.value ? (*bits|JOYPAD_START) : (*bits&~JOYPAD_START);
        else if(e.value && (e.code==115||e.code==114)){ vol+= e.code==115?12:-12; if(vol<0)vol=0; if(vol>255)vol=255;
            hw_volume(); vb_save(vol); vb_until=now_ms()+2000; } }
}
static int pad_bits(void){
    int b=0;
    if(pad_held(PA_A)) b|=JOYPAD_A; if(pad_held(PA_B)) b|=JOYPAD_B;
    if(pad_held(PA_X)||pad_held(PA_R)||pad_held(PA_PLUS)) b|=JOYPAD_START;
    if(pad_held(PA_Y)||pad_held(PA_L)||pad_held(PA_MINUS)) b|=JOYPAD_SELECT;
    if(pad_held(PA_UP)) b|=JOYPAD_UP; if(pad_held(PA_DOWN)) b|=JOYPAD_DOWN;
    if(pad_held(PA_LEFT)) b|=JOYPAD_LEFT; if(pad_held(PA_RIGHT)) b|=JOYPAD_RIGHT;
    int a; while(pad_next(&a)) if(a==PA_HOME) paused=!paused;
    return b;
}
static void paused_overlay(int on){        /* darken the game picture while paused */
    if(!on) return;
    for(int y=0;y<GY;y++) for(int x=0;x<SW;x++){ uint16_t*p=fb+y*fbstride+x; *p=(uint16_t)((*p>>1)&0x7BEF); }
    text(150,190,"PAUSE",5,rgb(0xF4E4D0));
}
int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: r3gb ROM\n"); return 1; }
    FILE*f=fopen(argv[1],"rb"); if(!f){ perror(argv[1]); return 1; }
    fseek(f,0,SEEK_END); rom_len=(size_t)ftell(f); fseek(f,0,SEEK_SET);
    if(rom_len<0x150 || rom_len>8*1024*1024){ fprintf(stderr,"bad rom size\n"); return 1; }
    rom=malloc(rom_len); if(fread(rom,1,rom_len,f)!=rom_len){ fprintf(stderr,"short read\n"); return 1; } fclose(f);
    snprintf(save_path,sizeof save_path,"%s",argv[1]); char*dot=strrchr(save_path,'.'); if(dot) *dot=0;
    strncat(save_path,".sav",sizeof save_path-strlen(save_path)-1);

    static struct gb_s gb;
    enum gb_init_error_e ie=gb_init(&gb,rom_read,cram_read,cram_write,gb_err,NULL);
    if(ie!=GB_INIT_NO_ERROR){ fprintf(stderr,"[r3gb] init error %d (unsupported cartridge?)\n",ie); return 2; }
    size_t sz=0; gb_get_save_size_s(&gb,&sz); cram_len=sz;
    if(cram_len){ cram=calloc(1,cram_len); FILE*s=fopen(save_path,"rb"); if(s){ if(fread(cram,1,cram_len,s)){} fclose(s); } }
    { time_t t=time(0); struct tm tm; localtime_r(&t,&tm); gb_set_rtc(&gb,&tm); }
    gb_init_lcd(&gb,draw_line);
    minigb_apu_audio_init(&apu);
    for(int p=0;p<3;p++) for(int i=0;i<4;i++) pal[p][i]=rgb(SHADE[i]);
    vol=vb_load();

    int fd=open("/dev/fb0",O_RDWR); struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    ioctl(fd,FBIOGET_VSCREENINFO,&vi); ioctl(fd,FBIOGET_FSCREENINFO,&fi);
    vi.xoffset=0; vi.yoffset=0; ioctl(fd,FBIOPAN_DISPLAY,&vi);
    fb=mmap(0,fi.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); fbstride=fi.line_length/2;
    ts_fd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
    kp_fd=open("/dev/input/event0",O_RDONLY|O_NONBLOCK);
    km_fd=open("/dev/input/event2",O_RDONLY|O_NONBLOCK);
    memset(fb,0,(size_t)fbstride*2*GY);
    draw_deck();

    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=2; cfg.rate=AUDIO_SAMPLE_RATE; cfg.period_size=1024; cfg.period_count=4; cfg.format=PCM_FORMAT_S16_LE;
    cfg.start_threshold=1024; cfg.stop_threshold=4096; cfg.avail_min=1024;   /* start after one period (~21 ms) */
    { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_alarm; sigaction(SIGALRM,&sa,NULL); }   /* no SA_RESTART */
    struct pcm*pcm=r3_pcm_open(&cfg);
    if(pcm && !pcm_is_ready(pcm)){ fprintf(stderr,"[r3gb] no sound: %s\n",pcm_get_error(pcm)); pcm_close(pcm); pcm=NULL; }
    mx=mixer_open(0); dac_route();
    audio_sample_t*snd=calloc(AUDIO_SAMPLES_TOTAL,sizeof *snd);   /* ~800 stereo frames per video frame */
    uint32_t next=now_ms(), last_save=now_ms(); int keybits=0, was_paused=0, vol_tick=0;
    fprintf(stderr,"[r3gb] %s, cart RAM %zu, sound %s\n",argv[1],cram_len,pcm?"on":"off");
    while(!quit){
        pad_poll(); poll_touch(); poll_keys(&keybits);
        int b = held_touch | keybits | pad_bits();
        { static uint32_t t0; static int frames; frames++; if(!t0) t0=now_ms();
          if(now_ms()-t0>=5000){ fprintf(stderr,"[r3gb] %.1f fps\n",frames*1000.0f/(now_ms()-t0)); t0=now_ms(); frames=0; } }
        if(paused){ if(!was_paused){ paused_overlay(1); was_paused=1; save_ram(); } usleep(20000); next=now_ms(); continue; }
        was_paused=0;
        gb.direct.joypad = (uint8_t)~b;                  /* 0 = pressed */
        gb_run_frame(&gb);
        minigb_apu_audio_callback(&apu,snd);
        if(pcm){ /* a write stuck for 0.6 s (codec not clocking) is interrupted by SIGALRM:
                  * then play on without sound instead of freezing the game */
                 struct itimerval it={{0,0},{0,600000}}, off={{0,0},{0,0}};
                 setitimer(ITIMER_REAL,&it,NULL);
                 int werr=pcm_write(pcm,snd,AUDIO_SAMPLES*4);
                 setitimer(ITIMER_REAL,&off,NULL);
                 if(werr && got_alarm){ fprintf(stderr,"[r3gb] DAC not consuming: sound off\n");
                     pcm_close(pcm); pcm=NULL; next=now_ms(); }
                 else if(werr) pcm_prepare(pcm);
                 got_alarm=0;
                 if(++vol_tick%30==0) hw_volume(); }      /* volume only sticks while the codec runs */
        else {   /* no DAC (e.g. busy): pace by the clock, 59.73 Hz */
            next+=17; uint32_t n=now_ms(); if((int32_t)(next-n)>0) usleep((next-n)*1000); else next=n; }
        if(now_ms()<vb_until) vb_draw(fb,fbstride,0,vol);          /* r3player-style volume bar */
        else if(vb_until){ vb_until=0; }                                /* next frame redraws those rows */
        if(now_ms()-last_save>60000){ last_save=now_ms(); save_ram(); }
    }
    save_ram();
    if(pcm) pcm_close(pcm);
    if(mx) mixer_close(mx);
    fprintf(stderr,"[r3gb] quit\n");
    return 0;
}
