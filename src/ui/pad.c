/* pad.c — see pad.h.
 *
 * Mapping. Controllers that the kernel knows report standard codes (BTN_SOUTH = A ...);
 * through hid-generic most report "joystick button N" codes (BTN_JOYSTICK + N) plus a hat
 * or stick. Joy-Con in their default HID mode (report 0x3F) send 16 buttons + a hat:
 *   button 0..3  face buttons (held sideways: right/bottom/top/left of the 4 buttons)
 *   button 4,5   SL, SR      button 8 minus, 9 plus, 12 home, 13 capture,
 *   button 14    L or R      button 15 ZL or ZR      hat = the stick (8 directions)
 * Unknown codes are still visible on the Settings > Controllers > Test buttons page. */
#include "pad.h"
#include "r3_tick.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <errno.h>

#define MAXP 4
#define BIT(arr,b) ((arr[(b)/8]>>((b)%8))&1)
typedef struct { int fd, num, joycon; char name[64]; int ax_min,ax_max,ay_min,ay_max;
                 int hidfd, batt, chg; uint32_t batt_t; } pad_t;   /* batt 0..100, -1 unknown */
static pad_t P[MAXP]; static int np;
static int held[PA_N]; static uint32_t held_since[PA_N], next_rep[PA_N];
static int q[32], qr, qw;
static float stick_x, stick_y; static int hat_x, hat_y;
static int raw_type, raw_code, raw_value; static const char*raw_dev="";
static uint32_t last_scan;

static int errno_gone(void){ return errno==ENODEV; }   /* controller switched off / out of range */
static void push(int a){ int n=(qw+1)%32; if(n!=qr){ q[qw]=a; qw=n; } }
static void set(int a,int on){
    if(a<0) return;
    if(on && !held[a]){ held[a]=1; held_since[a]=r3_tick_ms(); next_rep[a]=held_since[a]+300; push(a); }
    else if(!on) held[a]=0;
}
/* standard (kernel-mapped) codes first, then generic joystick button numbers */
static int map_key(const pad_t*p,int code){
    /* hid-generic numbers a gamepad's HID buttons 0x130+N (seen on the R3: Joy-Con (L)
     * advertises keys 0x130-0x13f). For Joy-Con N is the Nintendo button index, not the
     * standard BTN_SOUTH... meaning, so map by index. */
    if(p->joycon && code>=BTN_GAMEPAD && code<BTN_GAMEPAD+16){
        /* measured on a Joy-Con (L) held sideways (stick on the left): arrow buttons
         * Down/Right/Left/Up = 0x130/131/132/133 = A (east) / X (north) / B (south) / Y (west);
         * SL/SR 0x134/135, minus 0x138, stick click 0x13a, capture 0x13d, L 0x13e, ZL 0x13f.
         * The stick arrives as HAT0X/HAT0Y already in the sideways frame. */
        static const int JC[16]={PA_A,PA_X,PA_B,PA_Y, PA_L,PA_R,-1,-1, PA_MINUS,PA_PLUS,-1,-1,
                                 PA_HOME,PA_HOME, PA_L,PA_R};   /* capture = Home (L has no Home) */
        return JC[code-BTN_GAMEPAD]; }
    switch(code){
    case BTN_SOUTH: return PA_A;   case BTN_EAST: return PA_B;    /* A / B (Nintendo layout swaps) */
    case BTN_NORTH: return PA_X;   case BTN_WEST: return PA_Y;
    case BTN_TL: case BTN_TL2: return PA_L;   case BTN_TR: case BTN_TR2: return PA_R;
    case BTN_SELECT: return PA_MINUS;   case BTN_START: return PA_PLUS;   case BTN_MODE: return PA_HOME;
    case BTN_DPAD_UP: return PA_UP; case BTN_DPAD_DOWN: return PA_DOWN;
    case BTN_DPAD_LEFT: return PA_LEFT; case BTN_DPAD_RIGHT: return PA_RIGHT;
    case KEY_PLAYPAUSE: return PA_A; case KEY_NEXTSONG: return PA_R; case KEY_PREVIOUSSONG: return PA_L;
    case KEY_VOLUMEUP: return PA_PLUS; case KEY_VOLUMEDOWN: return PA_MINUS;
    }
    int n=-1;
    if(code>=BTN_JOYSTICK && code<BTN_JOYSTICK+16) n=code-BTN_JOYSTICK;
    else if(code>=BTN_TRIGGER_HAPPY && code<BTN_TRIGGER_HAPPY+16) n=16+code-BTN_TRIGGER_HAPPY;
    if(n<0) return -1;
    if(p->joycon){ static const int JC[16]={PA_A,PA_B,PA_X,PA_Y,PA_L,PA_R,-1,-1,PA_MINUS,PA_PLUS,-1,-1,PA_HOME,-1,PA_L,PA_R};
        return JC[n]; }
    static const int GEN[16]={PA_A,PA_B,PA_X,PA_Y,PA_L,PA_R,PA_L,PA_R,PA_MINUS,PA_PLUS,PA_HOME,-1,-1,-1,-1,-1};
    return GEN[n];
}
static void dirs(void){       /* stick + hat -> the four directions */
    int l = hat_x<0 || stick_x<-0.5f, r = hat_x>0 || stick_x>0.5f;
    int u = hat_y<0 || stick_y<-0.5f, d = hat_y>0 || stick_y>0.5f;
    set(PA_LEFT,l); set(PA_RIGHT,r); set(PA_UP,u); set(PA_DOWN,d);
}
static float norm(int v,int mn,int mx){ if(mx<=mn) return 0; float f=(2.0f*(v-mn)/(mx-mn))-1; if(f>-0.18f&&f<0.18f) f=0; return f; }

/* A Joy-Con sweeps its four lights until the host assigns it a player number. Send
 * subcommand 0x30 (set player lights) = player 1 through its hidraw node, like a Switch:
 * output report 0x01: [0x01, counter, 8 bytes neutral rumble, subcmd, arg], 49 bytes. */
/* Joy-Con hidraw: kept open to send subcommands and read their replies. Every subcommand
 * reply (input report 0x21) carries the battery in the high nibble of byte 2:
 * 8 full, 6 medium, 4 low, 2 critical, 0 empty, +1 = charging. We re-send "player 1
 * light" (0x30) every 30 s, which also keeps the light right. */
static int jc_open_hidraw(int evnum){
    char p[128]; snprintf(p,sizeof p,"/sys/class/input/event%d/device/device/hidraw",evnum);
    DIR*d=opendir(p); if(!d) return -1; struct dirent*e; char dev[64]="";
    while((e=readdir(d))) if(!strncmp(e->d_name,"hidraw",6)){ snprintf(dev,sizeof dev,"/dev/%s",e->d_name); break; }
    closedir(d); if(!dev[0]) return -1;
    int fd=open(dev,O_RDWR|O_NONBLOCK); if(fd<0) fprintf(stderr,"[pad] %s: %s\n",dev,strerror(errno));
    return fd;
}
static void jc_subcmd(int fd,unsigned char cmd,unsigned char arg){
    static unsigned char counter;
    if(fd<0) return;
    unsigned char r[49]={0x01,(unsigned char)(counter++&0x0f), 0x00,0x01,0x40,0x40,0x00,0x01,0x40,0x40, cmd,arg};
    if(write(fd,r,sizeof r)<0){}
}
static int is_pad(int fd){
    unsigned char ev[EV_MAX/8+1]={0}, key[KEY_MAX/8+1]={0}, abs_[ABS_MAX/8+1]={0};
    if(ioctl(fd,EVIOCGBIT(0,sizeof ev),ev)<0) return 0;
    ioctl(fd,EVIOCGBIT(EV_KEY,sizeof key),key); ioctl(fd,EVIOCGBIT(EV_ABS,sizeof abs_),abs_);
    int buttons=0; for(int c=BTN_JOYSTICK;c<BTN_THUMBR+1;c++) buttons+=BIT(key,c);
    for(int c=BTN_TRIGGER_HAPPY;c<BTN_TRIGGER_HAPPY+16;c++) buttons+=BIT(key,c);
    int media = BIT(key,KEY_PLAYPAUSE)||BIT(key,KEY_NEXTSONG);
    int axes = BIT(abs_,ABS_X)||BIT(abs_,ABS_HAT0X);
    return BIT(ev,EV_KEY) && (buttons>=4 || (axes&&buttons) || media);
}
static void scan(void){
    DIR*d=opendir("/dev/input"); if(!d) return; struct dirent*e;
    while((e=readdir(d)) && np<MAXP){
        int num; if(sscanf(e->d_name,"event%d",&num)!=1 || num<=3) continue;   /* 0-3: built-in keys/touch */
        int have=0; for(int i=0;i<np;i++) if(P[i].num==num) have=1; if(have) continue;
        char path[64]; snprintf(path,sizeof path,"/dev/input/%s",e->d_name);
        int fd=open(path,O_RDONLY|O_NONBLOCK); if(fd<0) continue;
        if(!is_pad(fd)){ close(fd); continue; }
        pad_t*p=&P[np]; memset(p,0,sizeof *p); p->fd=fd; p->num=num;
        ioctl(fd,EVIOCGNAME(sizeof p->name-1),p->name);
        p->joycon = strstr(p->name,"Joy-Con")!=NULL;
        p->hidfd=-1; p->batt=-1;
        if(p->joycon){ p->hidfd=jc_open_hidraw(num); jc_subcmd(p->hidfd,0x30,0x01); p->batt_t=r3_tick_ms(); }
        struct input_absinfo ai;
        if(ioctl(fd,EVIOCGABS(ABS_X),&ai)==0){ p->ax_min=ai.minimum; p->ax_max=ai.maximum; }
        if(ioctl(fd,EVIOCGABS(ABS_Y),&ai)==0){ p->ay_min=ai.minimum; p->ay_max=ai.maximum; }
        fprintf(stderr,"[pad] %s on event%d%s\n",p->name,num,p->joycon?" (Joy-Con)":"");
        np++;
    }
    closedir(d);
}
void pad_poll(void){
    uint32_t now=r3_tick_ms();
    if(now-last_scan>2000){ last_scan=now; scan(); }
    for(int i=0;i<np;i++){ struct input_event e; pad_t*p=&P[i]; int n;
        if(p->hidfd>=0){ unsigned char r[64]; int k;     /* Joy-Con subcommand replies -> battery */
            while((k=(int)read(p->hidfd,r,sizeof r))>2) if(r[0]==0x21||r[0]==0x30||r[0]==0x31){
                int lv=r[2]>>4; p->chg=lv&1; p->batt=(lv&0x0e)*100/8; if(p->batt>100) p->batt=100; }
            if(now-p->batt_t>30000){ p->batt_t=now; jc_subcmd(p->hidfd,0x30,0x01); } }
        while((n=read(p->fd,&e,sizeof e))==sizeof e){
            if(e.type==EV_SYN) continue;
            raw_type=e.type; raw_code=e.code; raw_value=e.value; raw_dev=p->name;
            if(e.type==EV_KEY && e.value!=2) set(map_key(p,e.code),e.value);
            else if(e.type==EV_ABS){
                if(e.code==ABS_HAT0X) hat_x=e.value; else if(e.code==ABS_HAT0Y) hat_y=e.value;
                else if(p->joycon) continue;   /* Joy-Con HID mode: the stick is the hat; X/Y are filler */
                else if(e.code==ABS_X) stick_x=norm(e.value,p->ax_min,p->ax_max);
                else if(e.code==ABS_Y) stick_y=norm(e.value,p->ay_min,p->ay_max);
                else continue;
                dirs(); }
        }
        if(n<0 && errno_gone()){ /* disconnected: drop it */
            close(p->fd); if(p->hidfd>=0) close(p->hidfd); P[i]=P[--np]; i--; memset(held,0,sizeof held); stick_x=stick_y=0; hat_x=hat_y=0; }
    }
    /* auto-repeat for held directions (menus, falling blocks) */
    for(int a=PA_UP;a<=PA_RIGHT;a++) if(held[a] && now>=next_rep[a]){ push(a); next_rep[a]=now+110; }
}
int pad_next(int*a){ if(qr==qw) return 0; *a=q[qr]; qr=(qr+1)%32; return 1; }
int pad_held(int a){ return a>=0 && a<PA_N && held[a]; }
float pad_stick_x(void){ float x=stick_x; if(hat_x) x=(float)hat_x; return x; }
int pad_count(void){ return np; }
const char* pad_name(int i){ return i>=0&&i<np?P[i].name:""; }
void pad_last_raw(int*t,int*c,int*v,const char**d){ *t=raw_type; *c=raw_code; *v=raw_value; *d=raw_dev; }
int pad_battery(int i,int*charging){ if(i<0||i>=np) return -1; if(charging) *charging=P[i].chg; return P[i].batt; }
