#include "audio.h"
#include <stdio.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "m4a.h"
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_t th;
static volatile int a_stop,a_pause,a_playing,a_vol=200,a_active,a_done;  /* a_vol = user-facing loudness 0..255; a_done=track ended naturally */
static volatile long a_seek_ms=-1;   /* >=0 => pending seek request */
static char a_path[1024];
static volatile long a_pos_ms,a_dur_ms;

static int readint(const char*p){ FILE*f=fopen(p,"r"); if(!f)return -1; int v=-1; if(fscanf(f,"%d",&v)!=1)v=-1; fclose(f); return v; }

/* in-process tinyalsa mixer (NO fork — system()/fork from the audio thread deadlocks).
 * The mixer handle + volume ctls are opened ONCE and cached: re-opening the mixer on every
 * buffer (as we did when the slider changed) does blocking control-enumeration I/O that
 * starves the PCM and drops the sound while sliding. Here: mixer_setup() opens once,
 * mx_fixed() sets the routing per track, apply_hwvol() is the cheap per-buffer write. */
static struct mixer *g_mix=0;
static struct mixer_ctl *g_l=0,*g_r=0;
static void mixer_setup(void){
    if(g_mix) return;
    g_mix=mixer_open(0);
    if(!g_mix){ fprintf(stderr,"[audio] mixer_open FAIL\n"); return; }
    g_l=mixer_get_ctl_by_name(g_mix,"Left Playback Volume");
    g_r=mixer_get_ctl_by_name(g_mix,"Right Playback Volume");
    fprintf(stderr,"[audio] mixer opened L=%d R=%d\n",g_l?1:0,g_r?1:0);
}
/* routing/mute — re-evaluated per track (jack may have changed); cheap, runs once per song */
static void mx_fixed(void){
    if(!g_mix) return;
    struct mixer_ctl*mo=mixer_get_ctl_by_name(g_mix,"Mute Output");
    struct mixer_ctl*sm=mixer_get_ctl_by_name(g_mix,"Soft Mute");
    struct mixer_ctl*op=mixer_get_ctl_by_name(g_mix,"Output Port Switch");
    struct mixer_ctl*be=mixer_get_ctl_by_name(g_mix,"Balance Lineout En");
    if(mo)mixer_ctl_set_value(mo,0,0);
    if(sm)mixer_ctl_set_value(sm,0,0);
    int bal=readint("/sys/class/switch/balance/state");
    int hs =readint("/sys/class/switch/headset/state");
    int port = bal==1 ? 3 : (hs==1 ? 1 : 3);
    if(op)mixer_ctl_set_value(op,0,port);
    if(be)mixer_ctl_set_value(be,0, bal==1?1:0);   /* enable balanced (4.4mm) output path */
    fprintf(stderr,"[audio] route port=%d bal=%d hs=%d\n",port,bal,hs);
}
/* fast per-buffer write. The DAC "Playback Volume" is ATTENUATION not gain: hw 0 = 0dB
 * (loudest), hw 255 = muted — so invert the user-facing loudness (0=silent..255=max). */
static void apply_hwvol(int userv){
    int hwv = 255 - (userv<0?0:(userv>255?255:userv));
    if(g_l)mixer_ctl_set_value(g_l,0,hwv);
    if(g_r)mixer_ctl_set_value(g_r,0,hwv);
}
static const char* ext(const char*p){ const char*d=strrchr(p,'.'); return d?d+1:""; }

enum { D_NONE, D_MP3, D_FLAC, D_WAV, D_M4A };

static void* play_thread(void*arg){
    (void)arg;
    const char*e=ext(a_path); int type=D_NONE;
    if(!strcasecmp(e,"mp3")||!strcasecmp(e,"mpga"))type=D_MP3;
    else if(!strcasecmp(e,"flac"))type=D_FLAC;
    else if(!strcasecmp(e,"wav"))type=D_WAV;
    else if(!strcasecmp(e,"m4a")||!strcasecmp(e,"aac")||!strcasecmp(e,"mp4"))type=D_M4A;

    mp3dec_ex_t mp3; drflac*flac=0; drwav wav; m4a_t*m4a=0; int hz=0,ch=0; long totframes=0;
    if(type==D_MP3){ if(mp3dec_ex_open(&mp3,a_path,MP3D_SEEK_TO_SAMPLE))type=D_NONE;
        else { hz=mp3.info.hz; ch=mp3.info.channels; if(ch)a_dur_ms=(long)((mp3.samples/ch)*1000/ (hz?hz:1)); } }
    else if(type==D_FLAC){ flac=drflac_open_file(a_path,0); if(!flac)type=D_NONE;
        else { hz=flac->sampleRate; ch=flac->channels; if(hz)a_dur_ms=(long)(flac->totalPCMFrameCount*1000/hz); } }
    else if(type==D_WAV){ if(!drwav_init_file(&wav,a_path,0))type=D_NONE;
        else { hz=wav.sampleRate; ch=wav.channels; if(hz)a_dur_ms=(long)(wav.totalPCMFrameCount*1000/hz); } }
    else if(type==D_M4A){ m4a=m4a_open(a_path,&hz,&ch,&a_dur_ms); if(!m4a)type=D_NONE; }
    fprintf(stderr,"[audio] type=%d hz=%d ch=%d\n",type,hz,ch); fflush(stderr);
    if(type==D_NONE || !hz || !ch){ a_playing=0; a_active=0; return 0; }

    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=ch; cfg.rate=hz; cfg.period_size=2048; cfg.period_count=8; cfg.format=PCM_FORMAT_S16_LE; /* ~bigger buffer vs UI contention */
    struct pcm*pcm=pcm_open(0,0,PCM_OUT,&cfg);
    if(!pcm||!pcm_is_ready(pcm)){ if(pcm)pcm_close(pcm);
        if(type==D_MP3)mp3dec_ex_close(&mp3); else if(type==D_FLAC)drflac_close(flac);
        else if(type==D_WAV)drwav_uninit(&wav); else if(type==D_M4A)m4a_close(m4a);
        a_playing=0; a_active=0; return 0; }
    fprintf(stderr,"[audio] pcm open ok, streaming\n"); fflush(stderr);
    mixer_setup(); mx_fixed();            /* open mixer once + set routing for this track */
    static short buf[8192];               /* interleaved s16 */
    int cur=0, applied=-1;                /* cur = ramped loudness; starts at 0 => fade-in, no start pop */
    const int STEP=10;                    /* loudness units per buffer (~85ms) => smooth, no dropout */
    const int FR=4096/ch<1?1:4096/ch;     /* frames per read (<=4096 samples) */
    while(!a_stop){
        while(a_pause && !a_stop) usleep(40000);
        if(a_stop) break;
        if(a_seek_ms>=0){ long tf=a_seek_ms*hz/1000; if(tf<0)tf=0;
            if(type==D_MP3)      mp3dec_ex_seek(&mp3,(uint64_t)tf*ch);
            else if(type==D_FLAC)drflac_seek_to_pcm_frame(flac,(drflac_uint64)tf);
            else if(type==D_WAV) drwav_seek_to_pcm_frame(&wav,(drwav_uint64)tf);
            else if(type==D_M4A){ m4a_seek(m4a,a_seek_ms); tf=a_seek_ms*hz/1000; }
            totframes=tf; a_pos_ms=totframes*1000/hz; a_seek_ms=-1; }
        long frames=0;
        if(type==D_MP3){ size_t n=mp3dec_ex_read(&mp3,buf,FR*ch); if(!n)break; frames=n/ch; }
        else if(type==D_FLAC){ frames=drflac_read_pcm_frames_s16(flac,FR,buf); if(!frames)break; }
        else if(type==D_WAV){ frames=drwav_read_pcm_frames_s16(&wav,FR,buf); if(!frames)break; }
        else { frames=m4a_decode(m4a,buf,8192); if(!frames)break; }   /* D_M4A */
        /* Ramp cur toward the target a_vol a few units per buffer: smooth slider changes +
         * a gentle fade-in at track start. Volume only sticks while the DAC is clocking, so
         * this runs inside the streaming loop; apply_hwvol() is cheap (cached ctls). */
        { int tgt=a_vol;
          if(cur<tgt){ cur+=STEP; if(cur>tgt)cur=tgt; }
          else if(cur>tgt){ cur-=STEP; if(cur<tgt)cur=tgt; }
          if(cur!=applied){ apply_hwvol(cur); applied=cur; } }
        int werr=pcm_write(pcm,buf,frames*ch*sizeof(short));
        if(werr){ fprintf(stderr,"[audio] xrun @%ldms err=%d, recovering\n",a_pos_ms,werr);
            pcm_prepare(pcm);
            if(pcm_write(pcm,buf,frames*ch*sizeof(short))){ fprintf(stderr,"[audio] write FAIL, stop\n"); break; } }
        totframes+=frames; a_pos_ms=totframes*1000/hz;
        if((totframes % (hz*2))<frames) fprintf(stderr,"[audio] playing %lds (%ld frames)\n",a_pos_ms/1000,totframes); /* ~every 2s */
    }
    pcm_close(pcm);
    if(type==D_MP3)mp3dec_ex_close(&mp3); else if(type==D_FLAC)drflac_close(flac);
    else if(type==D_WAV)drwav_uninit(&wav); else if(type==D_M4A)m4a_close(m4a);
    if(!a_stop) a_done=1;   /* reached EOF on its own -> UI auto-advances */
    a_playing=0; a_active=0; return 0;
}

void audio_play(const char*path){
    audio_stop();
    strncpy(a_path,path,sizeof a_path-1); a_path[sizeof a_path-1]=0;
    a_stop=0; a_pause=0; a_playing=1; a_active=1; a_pos_ms=0; a_dur_ms=0; a_done=0; a_seek_ms=-1;
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 2*1024*1024);  /* musl default ~128KB is too small for the decoders */
    if(pthread_create(&th,&at,play_thread,0)!=0){ a_playing=0; a_active=0; }
    pthread_attr_destroy(&at);
}
void audio_stop(void){ if(a_active){ a_stop=1; pthread_join(th,0); } a_playing=0; a_active=0; }
void audio_toggle_pause(void){ a_pause=!a_pause; }
int  audio_is_playing(void){ return a_playing; }
int  audio_finished(void){ if(a_done){ a_done=0; return 1; } return 0; }
void audio_seek(long ms){ a_seek_ms = ms<0?0:ms; }
int  audio_is_paused(void){ return a_pause; }
void audio_set_volume(int v){ a_vol = v<0?0:(v>255?255:v); }
int  audio_get_volume(void){ return a_vol; }
long audio_pos_ms(void){ return a_pos_ms; }
long audio_dur_ms(void){ return a_dur_ms; }
