/* r3snd.c — Doom sound effects for the R3: a small software mixer feeding the DAC.
 *
 * Doom sound lumps (DMX): u16 format=3, u16 rate, u32 count, then count bytes of 8-bit
 * unsigned PCM whose first and last 16 bytes are padding. Up to 8 channels are resampled
 * to 48 kHz stereo with Doom's volume (0-127) and stereo separation (0-254, 128 = centre),
 * mixed in an audio thread and written with tinyalsa (same DAC routing/volume as r3player).
 * Music (MUS -> OPL synthesis) is not implemented yet: the music module is silent. */
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define RATE 48000
#define NCH 16
#define PERIOD 1024

typedef struct { const uint8_t*data; uint32_t len, pos, step; int vl, vr; volatile int on; } chan_t;
static chan_t ch[NCH];
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static struct pcm*pcm; static int use_prefix, sound_ok;

static void route_and_volume(void){
    struct mixer*mx=mixer_open(0); if(!mx) return;
    struct mixer_ctl*mo=mixer_get_ctl_by_name(mx,"Mute Output"),*sm=mixer_get_ctl_by_name(mx,"Soft Mute"),
                    *op=mixer_get_ctl_by_name(mx,"Output Port Switch"),
                    *l=mixer_get_ctl_by_name(mx,"Left Playback Volume"),*r=mixer_get_ctl_by_name(mx,"Right Playback Volume");
    int vol=160; FILE*f=fopen("/usr/data/r3_vol","r"); if(f){ if(fscanf(f,"%d",&vol)!=1) vol=160; fclose(f); }
    int hw=255-(vol<0?0:vol>255?255:vol);
    if(mo) mixer_ctl_set_value(mo,0,0); if(sm) mixer_ctl_set_value(sm,0,0); if(op) mixer_ctl_set_value(op,0,1);
    if(l) mixer_ctl_set_value(l,0,hw); if(r) mixer_ctl_set_value(r,0,hw);
    mixer_close(mx);
}
static void* mixer_thread(void*arg){ (void)arg;
    static int16_t out[PERIOD*2]; int n=0;
    for(;;){
        pthread_mutex_lock(&mu);
        for(int i=0;i<PERIOD;i++){ int L=0,R=0;
            for(int c=0;c<NCH;c++){ chan_t*k=&ch[c]; if(!k->on) continue;
                uint32_t p=k->pos>>16; if(p>=k->len){ k->on=0; continue; }
                int s=((int)k->data[p]-128)<<8; L+=s*k->vl>>8; R+=s*k->vr>>8; k->pos+=k->step; }
            if(L>32767)L=32767; if(L<-32768)L=-32768; if(R>32767)R=32767; if(R<-32768)R=-32768;
            out[2*i]=(int16_t)L; out[2*i+1]=(int16_t)R; }
        pthread_mutex_unlock(&mu);
        if(pcm_write(pcm,out,sizeof out)) pcm_prepare(pcm);
        if(++n%64==0) route_and_volume();          /* the codec drops settings when idle */
    }
    return NULL;
}

static boolean snd_init(boolean prefix){
    use_prefix=prefix;
    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=2; cfg.rate=RATE; cfg.period_size=PERIOD; cfg.period_count=4; cfg.format=PCM_FORMAT_S16_LE;
    cfg.start_threshold=PERIOD; cfg.stop_threshold=PERIOD*4; cfg.avail_min=PERIOD;
    pcm=pcm_open(0,0,PCM_OUT,&cfg);
    if(!pcm || !pcm_is_ready(pcm)){ fprintf(stderr,"[r3snd] no DAC: %s\n",pcm?pcm_get_error(pcm):"?");
        if(pcm) pcm_close(pcm); pcm=NULL; return false; }
    route_and_volume();
    pthread_t t; if(pthread_create(&t,NULL,mixer_thread,NULL)!=0){ pcm_close(pcm); pcm=NULL; return false; }
    pthread_detach(t); sound_ok=1;
    fprintf(stderr,"[r3snd] sound effects on (48 kHz)\n");
    return true;
}
static void snd_shutdown(void){}
static int snd_lump(sfxinfo_t*sfx){
    char name[16];
    if(sfx->link) sfx=sfx->link;
    snprintf(name,sizeof name,use_prefix?"ds%s":"%s",sfx->name);
    return W_GetNumForName(name);
}
static void snd_update(void){}
static void params(chan_t*k,int vol,int sep){
    if(vol<0)vol=0; if(vol>127)vol=127; if(sep<0)sep=0; if(sep>254)sep=254;
    k->vl = vol*(254-sep)/127;         /* ~0..254 per side: 256 = full scale */
    k->vr = vol*sep/127;
}
static void snd_params(int c,int vol,int sep){ if(c<0||c>=NCH) return;
    pthread_mutex_lock(&mu); params(&ch[c],vol,sep); pthread_mutex_unlock(&mu); }
static int snd_start(sfxinfo_t*sfx,int c,int vol,int sep){
    if(!sound_ok || c<0 || c>=NCH) return -1;
    int lump=sfx->lumpnum; if(lump<0) return -1;
    const uint8_t*d=W_CacheLumpNum(lump,PU_STATIC); int len=W_LumpLength(lump);
    if(len<8+32 || d[0]!=3 || d[1]!=0) return -1;          /* not a DMX digital sound */
    uint32_t rate=d[2]|(d[3]<<8), cnt=d[4]|(d[5]<<8)|(d[6]<<16)|((uint32_t)d[7]<<24);
    if(cnt>(uint32_t)len-8) cnt=len-8;
    if(cnt<=32 || !rate) return -1;
    pthread_mutex_lock(&mu);
    chan_t*k=&ch[c]; k->data=d+8+16; k->len=cnt-32; k->pos=0;
    k->step=(uint32_t)(((uint64_t)rate<<16)/RATE); params(k,vol,sep); k->on=1;
    pthread_mutex_unlock(&mu);
    return c;
}
static void snd_stop(int c){ if(c>=0&&c<NCH){ pthread_mutex_lock(&mu); ch[c].on=0; pthread_mutex_unlock(&mu); } }
static boolean snd_playing(int c){ return c>=0&&c<NCH&&ch[c].on; }
static void snd_cache(sfxinfo_t*s,int n){ (void)s; (void)n; }

static snddevice_t devs[]={ SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                            SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
sound_module_t DG_sound_module={ devs, sizeof devs/sizeof devs[0], snd_init, snd_shutdown, snd_lump,
    snd_update, snd_params, snd_start, snd_stop, snd_playing, snd_cache };

/* music: not yet (needs MUS -> OPL synthesis); silent stubs */
static boolean mus_init(void){ return false; }
static void mus_void(void){}
static void mus_vol(int v){ (void)v; }
static void* mus_reg(void*d,int l){ (void)d; (void)l; return NULL; }
static void mus_unreg(void*h){ (void)h; }
static void mus_play(void*h,boolean loop){ (void)h; (void)loop; }
static boolean mus_playing(void){ return false; }
music_module_t DG_music_module={ devs, sizeof devs/sizeof devs[0], mus_init, mus_void, mus_vol,
    mus_void, mus_void, mus_reg, mus_unreg, mus_play, mus_void, mus_playing, mus_void };

void I_InitTimidityConfig(void){}
/* config variables the FEATURE_SOUND code binds (libsamplerate is not used here) */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
void r3snd_apply_volume(void){ route_and_volume(); }   /* volume keys: apply now */
