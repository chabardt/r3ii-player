/* audiotest — play an mp3/flac/wav to the DAC with full mixer setup, no UI/tapping.
 * Single-threaded (no fork). Usage: audiotest "<file>" [volume 0-255] */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int readint(const char*p){ FILE*f=fopen(p,"r"); if(!f)return -1; int v=-1; if(fscanf(f,"%d",&v)!=1)v=-1; fclose(f); return v; }
static void set_vol(int v){
    struct mixer*m=mixer_open(0);
    if(!m){ printf("mixer_open FAIL\n"); return; }
    struct mixer_ctl*l =mixer_get_ctl_by_name(m,"Left Playback Volume");
    struct mixer_ctl*r =mixer_get_ctl_by_name(m,"Right Playback Volume");
    struct mixer_ctl*mo=mixer_get_ctl_by_name(m,"Mute Output");
    struct mixer_ctl*sm=mixer_get_ctl_by_name(m,"Soft Mute");
    struct mixer_ctl*op=mixer_get_ctl_by_name(m,"Output Port Switch");
    if(l)mixer_ctl_set_value(l,0,v);
    if(r)mixer_ctl_set_value(r,0,v);
    if(mo)mixer_ctl_set_value(mo,0,0);
    if(sm)mixer_ctl_set_value(sm,0,0);
    int bal=readint("/sys/class/switch/balance/state");
    int hs =readint("/sys/class/switch/headset/state");
    int port = bal==1 ? 3 : (hs==1 ? 1 : 3);
    if(op)mixer_ctl_set_value(op,0,port);
    int rb = l?mixer_ctl_get_value(l,0):-1;
    printf("[vol] set=%d readback=%d port=%d (bal=%d hs=%d) ctls l=%d r=%d mo=%d sm=%d op=%d\n",
           v,rb,port,bal,hs,l?1:0,r?1:0,mo?1:0,sm?1:0,op?1:0);
}
static const char* ext(const char*p){ const char*d=strrchr(p,'.'); return d?d+1:""; }

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<2){ printf("usage: audiotest <file> [vol]\n"); return 1; }
    int vol = argc>2?atoi(argv[2]):110;
    const char*e=ext(argv[1]); int type=0; /*1 mp3 2 flac 3 wav*/
    if(!strcasecmp(e,"mp3"))type=1; else if(!strcasecmp(e,"flac"))type=2; else if(!strcasecmp(e,"wav"))type=3;
    mp3dec_ex_t mp3; drflac*flac=0; drwav wav; int hz=0,ch=0;
    if(type==1){ if(mp3dec_ex_open(&mp3,argv[1],MP3D_SEEK_TO_SAMPLE)){printf("mp3 open fail\n");return 1;} hz=mp3.info.hz; ch=mp3.info.channels; }
    else if(type==2){ flac=drflac_open_file(argv[1],0); if(!flac){printf("flac open fail\n");return 1;} hz=flac->sampleRate; ch=flac->channels; }
    else if(type==3){ if(!drwav_init_file(&wav,argv[1],0)){printf("wav open fail\n");return 1;} hz=wav.sampleRate; ch=wav.channels; }
    else { printf("unknown type\n"); return 1; }
    printf("[dec] type=%d hz=%d ch=%d\n",type,hz,ch);
    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=ch; cfg.rate=hz; cfg.period_size=1024; cfg.period_count=4; cfg.format=PCM_FORMAT_S16_LE;
    struct pcm*pcm=pcm_open(0,0,PCM_OUT,&cfg);
    if(!pcm||!pcm_is_ready(pcm)){ printf("pcm_open FAIL: %s\n",pcm?pcm_get_error(pcm):"null"); return 1; }
    printf("[pcm] open ok\n");
    static short buf[8192]; const int FR=4096/ch<1?1:4096/ch; long tot=0; int nw=0;
    for(;;){ long fr;
        if(type==1){ size_t n=mp3dec_ex_read(&mp3,buf,FR*ch); if(!n)break; fr=n/ch; }
        else if(type==2){ fr=drflac_read_pcm_frames_s16(flac,FR,buf); if(!fr)break; }
        else { fr=drwav_read_pcm_frames_s16(&wav,FR,buf); if(!fr)break; }
        if(pcm_write(pcm,buf,fr*ch*sizeof(short))){ printf("[xrun] @%lds recover\n",tot/hz); pcm_prepare(pcm);
            if(pcm_write(pcm,buf,fr*ch*sizeof(short))){ printf("write FAIL\n"); break; } }
        tot+=fr;
        if(nw==2||nw==20) set_vol(vol);
        nw++;
        if((tot % (hz*2))<fr) printf("[play] %lds (%ld frames)\n",tot/hz,tot);
    }
    printf("[done] frames=%ld\n",tot);
    pcm_close(pcm);
    return 0;
}
