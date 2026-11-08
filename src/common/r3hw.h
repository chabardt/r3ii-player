#ifndef R3_HW_H
#define R3_HW_H
/* Shared hardware helpers for the framebuffer games (r3gb, r3gba, r3doom).
 *
 * Touch: the R3's screen speaks multitouch protocol A — each frame lists the fingers that are
 * down (each ended by SYN_MT_REPORT, with a tracking id), and a lifted finger is simply no longer
 * listed: there is no "tracking id -1". Parsers waiting for that release kept the first finger
 * down forever (deck buttons stuck after the first touch). r3_touch_event rebuilds the contact
 * list every SYN_REPORT; protocol B screens (ABS_MT_SLOT) are handled too.
 *
 * Audio: route like r3player (src/ui/audio.c mx_fixed): the 4.4 mm balanced jack is
 * "Output Port Switch" 3 + "Balance Lineout En", 3.5 mm headphones port 1. Forcing port 1 left the
 * codec without a path on the balanced jack and nothing played. And r3player releases the DAC from
 * its audio thread a moment after a game starts: retry opening it instead of giving up. */
#include <stdio.h>
#include <unistd.h>
#include <linux/input.h>
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"

#define R3_NS 10
typedef struct { int x, y, down; } r3_contact_t;
typedef struct { int typeB, slot, fx, fy, fhas, nfc, btn; int cx[R3_NS], cy[R3_NS]; } r3_touch_t;

/* feed one input event; returns 1 at the end of a frame, with c[0..R3_NS-1] up to date */
static inline int r3_touch_event(r3_touch_t*t,const struct input_event*e,r3_contact_t*c){
    if(e->type==EV_ABS){
        switch(e->code){
        case ABS_MT_SLOT: t->typeB=1; t->slot=e->value<0||e->value>=R3_NS?0:e->value; break;
        case ABS_MT_TRACKING_ID: if(t->typeB) c[t->slot].down = e->value>=0; break;
        case ABS_MT_POSITION_X: if(t->typeB) c[t->slot].x=e->value; t->fx=e->value; t->fhas=1; break;
        case ABS_MT_POSITION_Y: if(t->typeB) c[t->slot].y=e->value; t->fy=e->value; t->fhas=1; break;
        case ABS_X: t->fx=e->value; t->fhas=1; break;
        case ABS_Y: t->fy=e->value; t->fhas=1; break; }
        return 0; }
    if(e->type==EV_KEY && e->code==BTN_TOUCH){ t->btn=e->value; return 0; }
    if(e->type==EV_SYN && e->code==SYN_MT_REPORT){
        if(t->fhas && t->nfc<R3_NS){ t->cx[t->nfc]=t->fx; t->cy[t->nfc]=t->fy; t->nfc++; } t->fhas=0; return 0; }
    if(e->type==EV_SYN && e->code==SYN_REPORT){
        if(!t->typeB){
            if(t->fhas && t->nfc<R3_NS){ t->cx[t->nfc]=t->fx; t->cy[t->nfc]=t->fy; t->nfc++; }
            if(!t->btn) t->nfc=0;
            for(int i=0;i<R3_NS;i++){ c[i].down = i<t->nfc; if(i<t->nfc){ c[i].x=t->cx[i]; c[i].y=t->cy[i]; } } }
        else if(!t->btn) for(int i=0;i<R3_NS;i++) c[i].down=0;
        t->fhas=0; t->nfc=0;
        return 1; }
    return 0;
}

static inline int r3_read_int(const char*p){ FILE*f=fopen(p,"r"); int v=-1; if(f){ if(fscanf(f,"%d",&v)!=1) v=-1; fclose(f); } return v; }
static inline void r3_dac_route(struct mixer*mx){
    if(!mx) return;
    struct mixer_ctl*mo=mixer_get_ctl_by_name(mx,"Mute Output"),*sm=mixer_get_ctl_by_name(mx,"Soft Mute"),
                    *op=mixer_get_ctl_by_name(mx,"Output Port Switch"),*be=mixer_get_ctl_by_name(mx,"Balance Lineout En");
    int bal=r3_read_int("/sys/class/switch/balance/state"), hs=r3_read_int("/sys/class/switch/headset/state");
    int port = bal==1 ? 3 : (hs==1 ? 1 : 3);
    if(mo) mixer_ctl_set_value(mo,0,0);
    if(sm) mixer_ctl_set_value(sm,0,0);
    if(op) mixer_ctl_set_value(op,0,port);
    if(be) mixer_ctl_set_value(be,0,bal==1?1:0);
}
/* open the DAC, retrying for up to 3 s while r3player lets go of it; NULL if it never opens */
static inline struct pcm* r3_pcm_open(struct pcm_config*cfg){
    for(int i=0;i<30;i++){
        struct pcm*p=pcm_open(0,0,PCM_OUT,cfg);
        if(p && pcm_is_ready(p)) return p;
        if(p){ if(i%10==0) fprintf(stderr,"[r3hw] DAC: %s\n",pcm_get_error(p)); pcm_close(p); }
        usleep(100000); }
    return NULL;
}
#endif
