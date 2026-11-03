#include "audio.h"
#include <stdio.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_t th;
static volatile int a_stop,a_pause,a_playing,a_vol=110,a_active;
static char a_path[1024];
static volatile long a_pos_ms,a_dur_ms;

static void set_vol(int v){
    struct mixer*m=mixer_open(0); if(!m)return;
    struct mixer_ctl*l=mixer_get_ctl_by_name(m,"Left Playback Volume");
    struct mixer_ctl*r=mixer_get_ctl_by_name(m,"Right Playback Volume");
    if(l)mixer_ctl_set_value(l,0,v);
    if(r)mixer_ctl_set_value(r,0,v);
    mixer_close(m);
}
static const char* ext(const char*p){ const char*d=strrchr(p,'.'); return d?d+1:""; }

enum { D_NONE, D_MP3, D_FLAC, D_WAV };

static void* play_thread(void*arg){
    (void)arg;
    const char*e=ext(a_path); int type=D_NONE;
    if(!strcasecmp(e,"mp3"))type=D_MP3;
    else if(!strcasecmp(e,"flac"))type=D_FLAC;
    else if(!strcasecmp(e,"wav"))type=D_WAV;

    mp3dec_ex_t mp3; drflac*flac=0; drwav wav; int hz=0,ch=0; long totframes=0;
    if(type==D_MP3){ if(mp3dec_ex_open(&mp3,a_path,MP3D_SEEK_TO_SAMPLE))type=D_NONE;
        else { hz=mp3.info.hz; ch=mp3.info.channels; if(ch)a_dur_ms=(long)((mp3.samples/ch)*1000/ (hz?hz:1)); } }
    else if(type==D_FLAC){ flac=drflac_open_file(a_path,0); if(!flac)type=D_NONE;
        else { hz=flac->sampleRate; ch=flac->channels; if(hz)a_dur_ms=(long)(flac->totalPCMFrameCount*1000/hz); } }
    else if(type==D_WAV){ if(!drwav_init_file(&wav,a_path,0))type=D_NONE;
        else { hz=wav.sampleRate; ch=wav.channels; if(hz)a_dur_ms=(long)(wav.totalPCMFrameCount*1000/hz); } }
    fprintf(stderr,"[audio] type=%d hz=%d ch=%d\n",type,hz,ch); fflush(stderr);
    if(type==D_NONE || !hz || !ch){ a_playing=0; a_active=0; return 0; }

    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=ch; cfg.rate=hz; cfg.period_size=1024; cfg.period_count=4; cfg.format=PCM_FORMAT_S16_LE;
    struct pcm*pcm=pcm_open(0,0,PCM_OUT,&cfg);
    if(!pcm||!pcm_is_ready(pcm)){ if(pcm)pcm_close(pcm);
        if(type==D_MP3)mp3dec_ex_close(&mp3); else if(type==D_FLAC)drflac_close(flac); else drwav_uninit(&wav);
        a_playing=0; a_active=0; return 0; }
    fprintf(stderr,"[audio] pcm open ok, streaming\n"); fflush(stderr);
    set_vol(a_vol); int lastvol=a_vol;

    static short buf[8192];           /* interleaved s16 */
    const int FR=4096/ch<1?1:4096/ch; /* frames per read (<=4096 samples) */
    while(!a_stop){
        while(a_pause && !a_stop) usleep(40000);
        if(a_stop) break;
        long frames=0;
        if(type==D_MP3){ size_t n=mp3dec_ex_read(&mp3,buf,FR*ch); if(!n)break; frames=n/ch; }
        else if(type==D_FLAC){ frames=drflac_read_pcm_frames_s16(flac,FR,buf); if(!frames)break; }
        else { frames=drwav_read_pcm_frames_s16(&wav,FR,buf); if(!frames)break; }
        if(a_vol!=lastvol){ set_vol(a_vol); lastvol=a_vol; }
        if(pcm_write(pcm,buf,frames*ch*sizeof(short))) break;
        totframes+=frames; a_pos_ms=totframes*1000/hz;
    }
    pcm_close(pcm);
    if(type==D_MP3)mp3dec_ex_close(&mp3); else if(type==D_FLAC)drflac_close(flac); else drwav_uninit(&wav);
    a_playing=0; a_active=0; return 0;
}

void audio_play(const char*path){
    audio_stop();
    strncpy(a_path,path,sizeof a_path-1); a_path[sizeof a_path-1]=0;
    a_stop=0; a_pause=0; a_playing=1; a_active=1; a_pos_ms=0; a_dur_ms=0;
    if(pthread_create(&th,0,play_thread,0)!=0){ a_playing=0; a_active=0; }
}
void audio_stop(void){ if(a_active){ a_stop=1; pthread_join(th,0); } a_playing=0; a_active=0; }
void audio_toggle_pause(void){ a_pause=!a_pause; }
int  audio_is_playing(void){ return a_playing; }
int  audio_is_paused(void){ return a_pause; }
void audio_set_volume(int v){ a_vol = v<0?0:(v>255?255:v); }
int  audio_get_volume(void){ return a_vol; }
long audio_pos_ms(void){ return a_pos_ms; }
long audio_dur_ms(void){ return a_dur_ms; }
