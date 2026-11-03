/* audiotest — prove MP3 decode + DAC output on R3 Pro II.
 * Streams an .mp3 (minimp3) to the hardware PCM (tinyalsa, card0 dev0).
 * Usage: audiotest "/usr/data/mnt/sd_0/Music/.../song.mp3" */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include "tinyalsa/pcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: audiotest <file.mp3>\n"); return 1; }
    mp3dec_ex_t dec;
    if(mp3dec_ex_open(&dec,argv[1],MP3D_SEEK_TO_SAMPLE)){ fprintf(stderr,"open/decoder fail\n"); return 1; }
    int hz=dec.info.hz, ch=dec.info.channels;
    printf("mp3: %d Hz, %d ch, ~%llu samples\n",hz,ch,(unsigned long long)dec.samples);
    struct pcm_config cfg; memset(&cfg,0,sizeof cfg);
    cfg.channels=ch?ch:2; cfg.rate=hz?hz:44100;
    cfg.period_size=1024; cfg.period_count=4; cfg.format=PCM_FORMAT_S16_LE;
    struct pcm*pcm=pcm_open(0,0,PCM_OUT,&cfg);
    if(!pcm||!pcm_is_ready(pcm)){ fprintf(stderr,"pcm_open fail: %s\n",pcm?pcm_get_error(pcm):"null"); return 1; }
    printf("pcm open ok (card0 dev0 %dHz %dch). playing...\n",cfg.rate,cfg.channels);
    static mp3d_sample_t buf[4096];
    size_t n; long total=0;
    while((n=mp3dec_ex_read(&dec,buf,4096))>0){
        if(pcm_write(pcm,buf,n*sizeof(mp3d_sample_t))){ fprintf(stderr,"pcm_write err: %s\n",pcm_get_error(pcm)); break; }
        total+=n;
    }
    printf("done, wrote %ld samples\n",total);
    pcm_close(pcm); mp3dec_ex_close(&dec);
    return 0;
}
