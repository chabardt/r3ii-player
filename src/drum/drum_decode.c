/* drum_decode.c — see drum_decode.h */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "drum_decode.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int dec_is_song(const char*n){ const char*x=strrchr(n,'.'); return x && (!strcasecmp(x,".mp3")||!strcasecmp(x,".flac")); }
int dec_open(dec_t*d,const char*path){
    memset(d,0,sizeof *d); const char*x=strrchr(path,'.'); if(!x) return -1;
    if(!strcasecmp(x,".mp3")){ mp3dec_ex_t*m=calloc(1,sizeof *m); if(!m) return -1;
        if(mp3dec_ex_open(m,path,MP3D_SEEK_TO_SAMPLE)){ free(m); return -1; }
        d->kind=1; d->h=m; d->rate=m->info.hz; d->ch=m->info.channels; }
    else if(!strcasecmp(x,".flac")){ drflac*f=drflac_open_file(path,0); if(!f) return -1;
        d->kind=2; d->h=f; d->rate=(int)f->sampleRate; d->ch=f->channels; }
    else return -1;
    if(d->rate<8000||d->ch<1||d->ch>8){ dec_close(d); return -1; }
    d->tmp=malloc(sizeof(int16_t)*4096*8); if(!d->tmp){ dec_close(d); return -1; }
    return 0;
}
int dec_read(dec_t*d,int16_t*out,int frames){
    int16_t*tmp=d->tmp; if(frames>4096) frames=4096;
    int got=0;
    if(d->kind==1){ mp3dec_ex_t*m=d->h; size_t n=mp3dec_ex_read(m,(mp3d_sample_t*)tmp,(size_t)frames*d->ch); got=(int)(n/d->ch); }
    else if(d->kind==2) got=(int)drflac_read_pcm_frames_s16((drflac*)d->h,(drflac_uint64)frames,tmp);
    for(int i=0;i<got;i++){ int l=tmp[i*d->ch], r=d->ch>1?tmp[i*d->ch+1]:l; out[2*i]=(int16_t)l; out[2*i+1]=(int16_t)r; }
    return got;
}
void dec_close(dec_t*d){
    if(d->kind==1){ mp3dec_ex_close(d->h); free(d->h); }
    else if(d->kind==2) drflac_close(d->h);
    free(d->tmp); d->tmp=0; d->kind=0; d->h=0;
}
