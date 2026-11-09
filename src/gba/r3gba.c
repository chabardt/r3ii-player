/* r3gba — Game Boy Advance player for the HiBy R3 Pro II, on gpSP (libretro core, GPLv2,
 * vendor/gpsp) with its MIPS dynamic recompiler. This file is a tiny libretro frontend.
 *
 * Launched by r3player (Games > Game Boy Advance):  r3gba ROM [--fresh]
 *   portrait (no controller): game 240x160 x2 at the top (480x320), touch deck below:
 *     L / R shoulders, D-pad, B, A, Select, Start;
 *   landscape (a controller is connected): R3 turned clockwise, game x3 = 720x480, full screen.
 * Controllers (pad.c): A/B, SL/SR = L/R, X or + = Start, Y or - = Select, Capture = pause.
 * R3 keys: next = A, prev = B, play = Start, volume +/-; power = pause, hold power = save+quit.
 * BIOS: gba_bios.bin next to the ROM or in Games/GBA, else gpSP's open-source BIOS.
 * Saves: <rom>.sav = cartridge save (128 KB, same as RetroArch gpSP .srm), checked every
 * few seconds; <rom>.ss = snapshot written on quit, restored at the next launch (--fresh skips it).
 * Sound: the core renders 32768 Hz, resampled to 48 kHz; blocking DAC writes pace the game. */
#include "libretro.h"
#include "pad.h"
#include "volbar.h"
#include "r3hw.h"
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/input.h>

unsigned r3gba_rom_mb = 8;              /* gpSP ROM buffer (MB), see r3gba_cfg.h */

#define SW 480
#define SH 720
#define GW 240
#define GH 160
#define TOP 24                          /* portrait: the bezel hides the first rows of the panel */
#define GY (TOP+320)                    /* portrait: deck top */
#define OUT_HZ 48000
#define CORE_HZ 32768

static uint16_t *fb; static int fbstride;
static uint32_t vb_until; static int vol=160;
static int landscape, paused, quit;
static uint32_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u); }
uint32_t r3_tick_ms(void){ return now_ms(); }          /* for pad.c */
static inline uint16_t rgb(uint32_t c){ return (uint16_t)(((c>>8)&0xF800)|((c>>5)&0x07E0)|((c>>3)&0x1F)); }

/* GBA buttons, as libretro joypad bits */
#define K_B      (1u<<RETRO_DEVICE_ID_JOYPAD_B)
#define K_SELECT (1u<<RETRO_DEVICE_ID_JOYPAD_SELECT)
#define K_START  (1u<<RETRO_DEVICE_ID_JOYPAD_START)
#define K_UP     (1u<<RETRO_DEVICE_ID_JOYPAD_UP)
#define K_DOWN   (1u<<RETRO_DEVICE_ID_JOYPAD_DOWN)
#define K_LEFT   (1u<<RETRO_DEVICE_ID_JOYPAD_LEFT)
#define K_RIGHT  (1u<<RETRO_DEVICE_ID_JOYPAD_RIGHT)
#define K_A      (1u<<RETRO_DEVICE_ID_JOYPAD_A)
#define K_L      (1u<<RETRO_DEVICE_ID_JOYPAD_L)
#define K_R      (1u<<RETRO_DEVICE_ID_JOYPAD_R)
static unsigned joy;                    /* what the core sees (never X/Y: gpSP turbo) */

/* ---------- libretro host ---------- */
static char rom_dir[1024], sys_dir[1024];
static retro_audio_buffer_status_callback_t abs_cb;
static const char*OPTS[][2]={
    {"gpsp_drc","enabled"}, {"gpsp_bios","auto"}, {"gpsp_boot_mode","game"},
    {"gpsp_rtc","auto"}, {"gpsp_rtc_time_source","system"}, {"gpsp_sound_rate","32768"},
    {"gpsp_frameskip","auto"}, {"gpsp_serial","disabled"}, {"gpsp_rumble","disabled"}, {0,0} };
static void core_log(enum retro_log_level lv,const char*fmt,...){
    if(lv<RETRO_LOG_INFO) return;
    va_list ap; va_start(ap,fmt); fputs("[gpsp] ",stderr); vfprintf(stderr,fmt,ap); va_end(ap); }
static bool env(unsigned cmd,void*data){
    switch(cmd){
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log=core_log; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(const enum retro_pixel_format*)data==RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data=sys_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data=rom_dir; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data=true; return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:     /* auto frameskip */
        abs_cb = data ? ((const struct retro_audio_buffer_status_callback*)data)->callback : NULL; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data=false; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: { struct retro_variable*v=data; v->value=NULL;
        for(int i=0;OPTS[i][0];i++) if(!strcmp(v->key,OPTS[i][0])){
            v->value = (!strcmp(v->key,"gpsp_drc") && getenv("R3GBA_NODRC")) ? "disabled" : OPTS[i][1];
            return true; }
        return false; }
    default: return false; }
}
static void input_poll(void){}
static int16_t input_state(unsigned port,unsigned dev,unsigned idx,unsigned id){ (void)idx;
    if(port || dev!=RETRO_DEVICE_JOYPAD) return 0;
    if(id==RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)joy;
    return id<16 ? (int16_t)((joy>>id)&1) : 0; }

/* ---------- video ---------- */
static short d3[SW];                    /* px / 3 */
static uint16_t *shot;                  /* --bench: copy of the last frame, 240x160 */
static uint32_t blit_us;                /* time spent drawing to the framebuffer */
static uint32_t us_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint32_t)(t.tv_sec*1000000u+t.tv_nsec/1000u); }
static void video_draw(const uint16_t*src,int sp,int vbon);
static void video(const void*data,unsigned w,unsigned h,size_t pitch){ (void)w; (void)h;
    if(!data) return;                   /* duplicate frame */
    const uint16_t*src=data; int sp=(int)(pitch/2);
    if(shot){ for(int y=0;y<GH;y++) memcpy(shot+y*GW,src+y*sp,GW*2); return; }
    uint32_t t=us_now(); video_draw(src,sp,now_ms()<vb_until); blit_us+=us_now()-t; }
/* The framebuffer is slow memory: build each output row in RAM, then only write
 * (never read back) the framebuffer, one memcpy per row. */
static void video_draw(const uint16_t*src,int sp,int vbon){
    static uint32_t row[SW/2];
    if(!landscape){                     /* x2: each pixel doubled as one 32-bit word */
        for(int y=vbon?(VB_THICK-TOP+1)/2:0;y<GH;y++){ const uint16_t*s=src+y*sp;
            for(int x=0;x<GW;x++){ uint32_t p=s[x]; row[x]=p|(p<<16); }
            memcpy(fb+(TOP+2*y)*fbstride,row,SW*2); memcpy(fb+(TOP+2*y+1)*fbstride,row,SW*2); }
        return; }
    /* landscape, R3 turned clockwise: game x runs bottom->top, game y left->right, x3.
     * Transpose in 8x8 tiles first (column reads of the source thrash the cache). */
    static uint16_t tr[GW*GH];          /* tr[gx*GH+gy] */
    for(int by=0;by<GH;by+=8) for(int bx=0;bx<GW;bx+=8)
        for(int y=by;y<by+8;y++){ const uint16_t*s=src+y*sp+bx; uint16_t*t=tr+bx*GH+y;
            for(int x=0;x<8;x++) t[x*GH]=s[x]; }
    uint16_t*r=(uint16_t*)row; int px0=vbon?VB_THICK:0;
    for(int gx=0;gx<GW;gx++){ const uint16_t*c=tr+gx*GH;
        for(int gy=0;gy<GH;gy++){ uint16_t p=c[gy]; r[3*gy]=p; r[3*gy+1]=p; r[3*gy+2]=p; }
        for(int k=0;k<3;k++) memcpy(fb+(SH-1-3*gx-k)*fbstride+px0,r+px0,(size_t)(SW-px0)*2); }
}

/* ---------- audio: 32768 -> 48000 Hz (linear) into one buffer per frame ---------- */
static int16_t abuf[4096*2]; static int an;          /* stereo frames waiting for the DAC */
static uint32_t rs_pos; static int16_t rs_prev[2];   /* 16.16, 0 = rs_prev, 1 = d[0] */
#define RS_STEP ((uint32_t)(((uint64_t)CORE_HZ<<16)/OUT_HZ))
static size_t audio_batch(const int16_t*d,size_t frames){
    uint32_t end=(uint32_t)frames<<16;
    while(rs_pos<end && an<4096){ uint32_t i=rs_pos>>16, f=rs_pos&0xFFFF;
        for(int c=0;c<2;c++){ int a=i?d[(i-1)*2+c]:rs_prev[c], b=d[i*2+c];
            abuf[an*2+c]=(int16_t)(a+(((b-a)*(int32_t)f)>>16)); }
        an++; rs_pos+=RS_STEP; }
    rs_pos = rs_pos>=end ? rs_pos-end : 0;
    if(frames){ rs_prev[0]=d[(frames-1)*2]; rs_prev[1]=d[(frames-1)*2+1]; }
    return frames; }
static void audio_one(int16_t l,int16_t r){ int16_t s[2]={l,r}; audio_batch(s,1); }

/* ---------- drawing helpers (portrait deck / overlays) ---------- */
static inline void put(int x,int y,uint16_t c){ if(x>=0&&x<SW&&y>=0&&y<SH) fb[y*fbstride+x]=c; }
static void fill(int x0,int y0,int w,int h,uint16_t c){ for(int y=y0;y<y0+h;y++) for(int x=x0;x<x0+w;x++) put(x,y,c); }
static void disc(int cx,int cy,int r,uint16_t c){ for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++) if(x*x+y*y<=r*r) put(cx+x,cy+y,c); }
static void rrect(int x0,int y0,int w,int h,int r,uint16_t c){
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){ int dx=x<r?r-x:(x>=w-r?x-(w-r-1):0), dy=y<r?r-y:(y>=h-r?y-(h-r-1):0);
        if(dx*dx+dy*dy<=r*r) put(x0+x,y0+y,c); } }
static const char*GL="ABCELRSTPU"; static const uint8_t GLYPH[10][7]={
 {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
 {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
 {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}};
/* text in viewer coordinates: rot=1 is the landscape view (u right, v down -> x=v, y=719-u) */
static void text(int u,int v,const char*s,int sc,uint16_t c,int rot){
    for(;*s;s++,u+=6*sc){ const char*p=strchr(GL,*s); if(!p) continue; const uint8_t*g=GLYPH[p-GL];
        for(int r=0;r<7;r++) for(int b=0;b<5;b++) if(g[r]&(0x10>>b))
            for(int i=0;i<sc;i++) for(int j=0;j<sc;j++){ int uu=u+b*sc+j, vv=v+r*sc+i;
                if(rot) put(vv,SH-1-uu,c); else put(uu,vv,c); } } }

/* ---------- portrait deck ---------- */
#define DPX 112
#define DPY 548
#define AX 412
#define AY 520
#define BX 318
#define BY 582
#define SHY 356                         /* shoulder buttons */
#define SHH 48
static unsigned held_touch;
static void draw_deck(void){
    uint16_t bg=rgb(0x0A0A0A), pad_c=rgb(0x2A221A), acc=rgb(0xE8731C), hi=rgb(0xF4E4D0), dim=rgb(0x8A6A4A);
    fill(0,GY,SW,SH-GY,bg); fill(0,GY,SW,2,dim);
    rrect(12,SHY,150,SHH,16,held_touch&K_L?acc:pad_c);  text(80,SHY+15,"L",3,hi,0);
    rrect(318,SHY,150,SHH,16,held_touch&K_R?acc:pad_c); text(386,SHY+15,"R",3,hi,0);
    uint16_t up=held_touch&K_UP?acc:pad_c, dn=held_touch&K_DOWN?acc:pad_c, lf=held_touch&K_LEFT?acc:pad_c, rt=held_touch&K_RIGHT?acc:pad_c;
    rrect(DPX-30,DPY-96,60,70,10,up); rrect(DPX-30,DPY+26,60,70,10,dn);
    rrect(DPX-96,DPY-30,70,60,10,lf); rrect(DPX+26,DPY-30,70,60,10,rt); fill(DPX-30,DPY-30,60,60,pad_c);
    disc(AX,AY,46,held_touch&K_A?hi:acc); text(AX-7,AY-10,"A",3,bg,0);
    disc(BX,BY,46,held_touch&K_B?hi:acc); text(BX-7,BY-10,"B",3,bg,0);
    rrect(196,678,80,30,15,held_touch&K_SELECT?acc:pad_c); text(206,686,"SELECT",2,hi,0);
    rrect(292,678,80,30,15,held_touch&K_START?acc:pad_c);  text(304,686,"START",2,hi,0);
}
static unsigned hit(int x,int y){
    if(y<GY) return 0;
    if(y>=GY && y<SHY+SHH+12){ if(x<200) return K_L; if(x>=280) return K_R; return 0; }
    int dx=x-DPX, dy=y-DPY;
    if(dx*dx+dy*dy<120*120){ unsigned b=0;              /* D-pad: 8-way by angle, dead centre */
        if(abs(dx)<14 && abs(dy)<14) return 0;
        if(dy<-abs(dx)/2) b|=K_UP; if(dy>abs(dx)/2) b|=K_DOWN;
        if(dx<-abs(dy)/2) b|=K_LEFT; if(dx>abs(dy)/2) b|=K_RIGHT; return b; }
    if((x-AX)*(x-AX)+(y-AY)*(y-AY)<62*62) return K_A;
    if((x-BX)*(x-BX)+(y-BY)*(y-BY)<62*62) return K_B;
    if(y>=664 && x>=186 && x<284) return K_SELECT;
    if(y>=664 && x>=284 && x<382) return K_START;
    return 0;
}
#define NS R3_NS
static r3_contact_t sl[NS]; static r3_touch_t tstate;     /* r3hw.h: protocol A or B */
static int ts_fd=-1, kp_fd=-1, km_fd=-1;
static void poll_touch(void){
    struct input_event e;
    while(read(ts_fd,&e,sizeof e)==sizeof e){
        if(landscape) continue;                          /* touch off in landscape */
        r3_touch_event(&tstate,&e,sl);
    }
    if(landscape){ held_touch=0; return; }
    unsigned b=0; for(int i=0;i<NS;i++) if(sl[i].down) b|=hit(sl[i].x,sl[i].y);
    if(b!=held_touch){ held_touch=b; draw_deck(); }
}

/* ---------- DAC + volume (same controls r3player uses) ---------- */
static struct mixer *mx;
static void hw_volume(void){ if(!mx) return; int hw=255-(vol<0?0:vol>255?255:vol);
    struct mixer_ctl*l=mixer_get_ctl_by_name(mx,"Left Playback Volume"),*r=mixer_get_ctl_by_name(mx,"Right Playback Volume");
    if(l) mixer_ctl_set_value(l,0,hw); if(r) mixer_ctl_set_value(r,0,hw); }
static void dac_route(void){ r3_dac_route(mx); }     /* balanced 4.4 mm or 3.5 mm, like r3player */

/* ---------- keys + controllers ---------- */
static uint32_t pwr_t; static int pwr_down;
static void poll_keys(unsigned*bits){
    struct input_event e;
    while(kp_fd>=0 && read(kp_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY) continue;
        if(e.code==116){ if(e.value==1){ pwr_down=1; pwr_t=now_ms(); }
            else if(e.value==0 && pwr_down){ pwr_down=0; if(now_ms()-pwr_t<1000) paused=!paused; } }
        else if(e.code==165) *bits = e.value ? (*bits|K_B) : (*bits&~K_B); }
    if(pwr_down && now_ms()-pwr_t>=1200) quit=1;
    while(km_fd>=0 && read(km_fd,&e,sizeof e)==sizeof e){ if(e.type!=EV_KEY) continue;
        if(e.code==163) *bits = e.value ? (*bits|K_A) : (*bits&~K_A);
        else if(e.code==164) *bits = e.value ? (*bits|K_START) : (*bits&~K_START);
        else if(e.value && (e.code==115||e.code==114)){ vol+= e.code==115?12:-12; if(vol<0)vol=0; if(vol>vb_max())vol=vb_max();
            hw_volume(); vb_save(vol); vb_until=now_ms()+2000; } }
}
static unsigned pad_bits(void){
    unsigned b=0;
    if(pad_held(PA_A)) b|=K_A; if(pad_held(PA_B)) b|=K_B;
    if(pad_held(PA_L)) b|=K_L; if(pad_held(PA_R)) b|=K_R;
    if(pad_held(PA_X)||pad_held(PA_PLUS)) b|=K_START;
    if(pad_held(PA_Y)||pad_held(PA_MINUS)) b|=K_SELECT;
    if(pad_held(PA_UP)) b|=K_UP; if(pad_held(PA_DOWN)) b|=K_DOWN;
    if(pad_held(PA_LEFT)) b|=K_LEFT; if(pad_held(PA_RIGHT)) b|=K_RIGHT;
    int a; while(pad_next(&a)) if(a==PA_HOME) paused=!paused;
    return b;
}
static void clear_screen(void){ for(int y=0;y<SH;y++) memset(fb+y*fbstride,0,SW*2); }
static void set_landscape(int on){
    landscape=on; clear_screen(); held_touch=0;
    memset(sl,0,sizeof sl);
    if(!on) draw_deck();
    fprintf(stderr,"[r3gba] %s\n",on?"landscape (controller)":"portrait (touch)"); }
static void paused_overlay(void){         /* darken the game picture */
    int h = landscape ? SH : GY;
    for(int y=0;y<h;y++) for(int x=0;x<SW;x++){ uint16_t*p=fb+y*fbstride+x; *p=(uint16_t)((*p>>1)&0x7BEF); }
    if(landscape) text(360-75,240-17,"PAUSE",5,rgb(0xF4E4D0),1);
    else text(240-75,TOP+160-17,"PAUSE",5,rgb(0xF4E4D0),0);
}

/* ---------- files ---------- */
static char sav_path[1100], ss_path[1100];
static uint8_t *sram, *sram_shadow; static size_t sram_len;
static int write_file(const char*path,const void*data,size_t n){    /* never leave a half-written file */
    char tmp[1200]; snprintf(tmp,sizeof tmp,"%s.tmp",path);
    FILE*f=fopen(tmp,"wb"); if(!f) return -1;
    int ok=fwrite(data,1,n,f)==n; ok&=fflush(f)==0; fsync(fileno(f)); fclose(f);
    if(!ok || rename(tmp,path)!=0){ unlink(tmp); return -1; }
    return 0; }
static void save_sram(void){
    if(!sram_len || !memcmp(sram,sram_shadow,sram_len)) return;
    if(write_file(sav_path,sram,sram_len)==0){ memcpy(sram_shadow,sram,sram_len); fprintf(stderr,"[r3gba] saved %s\n",sav_path); } }
static void save_state(void){
    size_t n=retro_serialize_size(); void*b=n?malloc(n):NULL; if(!b) return;
    if(retro_serialize(b,n) && write_file(ss_path,b,n)==0) fprintf(stderr,"[r3gba] snapshot %zu bytes\n",n);
    free(b); }
static void load_state(void){
    FILE*f=fopen(ss_path,"rb"); if(!f) return;
    size_t n=retro_serialize_size(); void*b=malloc(n);
    if(b && fread(b,1,n,f)==n && retro_unserialize(b,n)) fprintf(stderr,"[r3gba] resumed from snapshot\n");
    else fprintf(stderr,"[r3gba] snapshot unreadable, starting fresh\n");
    free(b); fclose(f); }
static long mem_available_mb(void){
    FILE*f=fopen("/proc/meminfo","r"); char l[128]; long kb=-1;
    if(f){ while(fgets(l,sizeof l,f)) if(sscanf(l,"MemAvailable: %ld kB",&kb)==1) break; fclose(f); }
    return kb<0 ? 24 : kb/1024; }

static volatile sig_atomic_t got_alarm;
static void on_alarm(int s){ (void)s; got_alarm=1; }
static void on_crash(int sig,siginfo_t*si,void*uc_){ ucontext_t*uc=uc_;
    fprintf(stderr,"[r3gba] CRASH signal %d addr %p pc 0x%lx ra 0x%lx\n",sig,si->si_addr,
        (unsigned long)uc->uc_mcontext.pc,(unsigned long)uc->uc_mcontext.gregs[31]); _exit(128+sig); }

int main(int argc,char**argv){
    setvbuf(stderr,NULL,_IONBF,0);
    { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_crash; sa.sa_flags=SA_SIGINFO;
      sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL); sigaction(SIGILL,&sa,NULL); sigaction(SIGFPE,&sa,NULL); }
    const char*rom=NULL; int fresh=0, bench=0;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--fresh")) fresh=1;
        else if(!strcmp(argv[i],"--bench") && i+1<argc) bench=atoi(argv[++i]);   /* N frames, no screen/sound */
        else rom=argv[i]; }
    if(!rom){ fprintf(stderr,"usage: r3gba ROM [--fresh] [--bench FRAMES]\n"); return 1; }
    struct stat st; if(stat(rom,&st)!=0){ perror(rom); return 1; }

    /* paths: <rom>.sav / <rom>.ss; BIOS dir = first of rom dir, SD Games/GBA, internal Games/GBA */
    snprintf(rom_dir,sizeof rom_dir,"%s",rom); { char*s=strrchr(rom_dir,'/'); if(s) *s=0; else strcpy(rom_dir,"."); }
    snprintf(sav_path,sizeof sav_path,"%s",rom); { char*d=strrchr(sav_path,'.'), *s=strrchr(sav_path,'/'); if(d && (!s||d>s)) *d=0; }
    snprintf(ss_path,sizeof ss_path,"%s.ss",sav_path); strcat(sav_path,".sav");
    { const char*cand[]={rom_dir,"/usr/data/mnt/sd_0/Games/GBA","/usr/data/Games/GBA"}; snprintf(sys_dir,sizeof sys_dir,"%s",rom_dir);
      for(int i=0;i<3;i++){ char p[1100]; snprintf(p,sizeof p,"%s/gba_bios.bin",cand[i]);
          if(access(p,R_OK)==0){ snprintf(sys_dir,sizeof sys_dir,"%s",cand[i]); break; } } }

    /* ROM buffer: the whole ROM if it fits in free memory (minus ~10 MB for the core,
     * JIT cache and us), else what fits — gpSP pages the rest from the file */
    long need=(long)((st.st_size+(1<<20)-1)>>20), avail=mem_available_mb(), budget=avail-10;
    if(need>32) need=32;
    r3gba_rom_mb = (unsigned)(need<=budget ? need : budget<4 ? 4 : budget);
    fprintf(stderr,"[r3gba] %s: %ld MB ROM, %ld MB free, buffer %u MB%s\n",rom,need,avail,r3gba_rom_mb,
        r3gba_rom_mb<need?" (paging)":"");

    for(int px=0;px<SW;px++) d3[px]=(short)(px/3);
    retro_set_environment(env);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio_one);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();
    struct retro_game_info gi; memset(&gi,0,sizeof gi); gi.path=rom;
    if(bench) shot=calloc(GW*GH,2);     /* no screen: keep the last frame */
    if(!retro_load_game(&gi)){ fprintf(stderr,"[r3gba] could not load %s\n",rom); return 2; }
    struct retro_system_av_info av; retro_get_system_av_info(&av);
    fprintf(stderr,"[r3gba] %.2f fps, %.0f Hz, BIOS dir %s\n",av.timing.fps,av.timing.sample_rate,sys_dir);

    sram=retro_get_memory_data(RETRO_MEMORY_SAVE_RAM); sram_len=sram?retro_get_memory_size(RETRO_MEMORY_SAVE_RAM):0;
    if(sram_len){ FILE*s=fopen(sav_path,"rb"); if(s){ size_t n=fread(sram,1,sram_len,s); fclose(s);
            fprintf(stderr,"[r3gba] loaded save %zu bytes\n",n); }
        sram_shadow=malloc(sram_len); memcpy(sram_shadow,sram,sram_len); }
    if(!fresh) load_state();

    if(bench){                           /* emulation speed only: r3gba ROM --bench 600 */
        uint32_t t0=now_ms(); for(int i=0;i<bench;i++){ retro_run(); an=0; }
        uint32_t dt=now_ms()-t0; fprintf(stderr,"[r3gba] bench: %d frames in %u ms = %.1f fps\n",bench,dt,bench*1000.0f/(dt?dt:1));
        FILE*pf=fopen("/tmp/r3gba.ppm","wb");      /* last frame, to read a test ROM's verdict */
        if(pf){ fprintf(pf,"P6 %d %d 255\n",GW,GH);
            for(int i=0;i<GW*GH;i++){ uint16_t c=shot[i]; fputc((c>>11)<<3,pf); fputc(((c>>5)&63)<<2,pf); fputc((c&31)<<3,pf); }
            fclose(pf); }
        save_sram(); return 0; }

    vol=vb_load();
    int fd=open("/dev/fb0",O_RDWR); struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    ioctl(fd,FBIOGET_VSCREENINFO,&vi); ioctl(fd,FBIOGET_FSCREENINFO,&fi);
    vi.xoffset=0; vi.yoffset=0; ioctl(fd,FBIOPAN_DISPLAY,&vi);
    fb=mmap(0,fi.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); fbstride=fi.line_length/2;
    ts_fd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
    kp_fd=open("/dev/input/event0",O_RDONLY|O_NONBLOCK);
    km_fd=open("/dev/input/event2",O_RDONLY|O_NONBLOCK);
    pad_poll(); set_landscape(pad_count()>0);

    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=2; cfg.rate=OUT_HZ; cfg.period_size=1024; cfg.period_count=4; cfg.format=PCM_FORMAT_S16_LE;
    cfg.start_threshold=1024; cfg.stop_threshold=4096; cfg.avail_min=1024;
    { struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_alarm; sigaction(SIGALRM,&sa,NULL); }   /* no SA_RESTART */
    struct pcm*pcm=r3_pcm_open(&cfg);
    if(pcm && !pcm_is_ready(pcm)){ fprintf(stderr,"[r3gba] no sound: %s\n",pcm_get_error(pcm)); pcm_close(pcm); pcm=NULL; }
    mx=mixer_open(0); dac_route();

    uint32_t next=now_ms(), last_save=now_ms(); unsigned keybits=0; int was_paused=0, vol_tick=0;
    uint32_t t0=now_ms(), run_us=0; int frames=0, skipped=0;
    fprintf(stderr,"[r3gba] running, cart save %zu bytes, sound %s\n",sram_len,pcm?"on":"off");
    while(!quit){
        pad_poll(); if((pad_count()>0)!=landscape) set_landscape(pad_count()>0);
        poll_touch(); poll_keys(&keybits);
        joy = held_touch | keybits | pad_bits();
        if(paused){ if(!was_paused){ paused_overlay(); was_paused=1; save_sram(); } usleep(20000); next=now_ms(); continue; }
        if(was_paused){ was_paused=0; if(!landscape) draw_deck(); }
        if(abs_cb){ unsigned avail=0; struct timespec hts;      /* DAC fill -> gpSP skips drawing when low */
            if(pcm && pcm_get_htimestamp(pcm,&avail,&hts)==0){ unsigned occ = avail>=4096 ? 0 : (4096-avail)*100/4096;
                abs_cb(true,occ,occ<40); if(occ<40) skipped++; }
            else abs_cb(false,0,false); }
        struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
        retro_run();
        clock_gettime(CLOCK_MONOTONIC,&b); run_us+=(uint32_t)((b.tv_sec-a.tv_sec)*1000000+(b.tv_nsec-a.tv_nsec)/1000);
        if(++frames, now_ms()-t0>=5000){ uint32_t dt=now_ms()-t0;
            fprintf(stderr,"[r3gba] %.1f fps, emulation %.1f ms/frame (drawing %.1f), %d drawn frames skipped\n",
                frames*1000.0f/dt,run_us/1000.0f/frames,blit_us/1000.0f/frames,skipped);
            t0=now_ms(); frames=0; run_us=0; skipped=0; blit_us=0; }
        if(pcm && an){ /* a write stuck for 0.6 s (codec not clocking) is interrupted by SIGALRM:
                        * then play on without sound instead of freezing the game */
            struct itimerval it={{0,0},{0,600000}}, off={{0,0},{0,0}};
            setitimer(ITIMER_REAL,&it,NULL);
            int werr=pcm_write(pcm,abuf,(unsigned)an*4);
            setitimer(ITIMER_REAL,&off,NULL);
            if(werr && got_alarm){ fprintf(stderr,"[r3gba] DAC not consuming: sound off\n"); pcm_close(pcm); pcm=NULL; next=now_ms(); }
            else if(werr) pcm_prepare(pcm);
            got_alarm=0;
            if(++vol_tick%30==0) hw_volume(); }       /* volume only sticks while the codec runs */
        else {   /* no DAC: pace by the clock, 59.73 Hz */
            next+=17; uint32_t n=now_ms(); if((int32_t)(next-n)>0) usleep((next-n)*1000); else next=n; }
        an=0;
        if(now_ms()<vb_until) vb_draw(fb,fbstride,landscape,vol);
        else if(vb_until){ vb_until=0; if(landscape) vb_erase(fb,fbstride,1); }   /* next frame redraws the game rows */
        if(now_ms()-last_save>3000){ last_save=now_ms(); save_sram(); }
    }
    save_sram(); save_state();
    if(pcm) pcm_close(pcm);
    if(mx) mixer_close(mx);
    clear_screen();
    fprintf(stderr,"[r3gba] quit\n");
    return 0;
}
