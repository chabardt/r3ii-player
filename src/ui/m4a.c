/* M4A/AAC decoder: minimp4 (MP4 demux) + faad2 (AAC decode). Isolated TU. */
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include "neaacdec.h"
#include "m4a.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct m4a { MP4D_demux_t mp4; NeAACDecHandle aac; FILE*f; unsigned atrack,nsample,ntotal; int hz,ch; };

static int rd(int64_t off,void*b,size_t sz,void*tok){
    FILE*f=(FILE*)tok; if(fseeko(f,(off_t)off,SEEK_SET)!=0) return -1;
    return fread(b,1,sz,f)!=sz; }

m4a_t* m4a_open(const char*path,int*hz,int*ch,long*dur_ms){
    m4a_t*m=calloc(1,sizeof *m); if(!m) return 0;
    m->f=fopen(path,"rb"); if(!m->f){ free(m); return 0; }
    fseeko(m->f,0,SEEK_END); int64_t sz=ftello(m->f); fseeko(m->f,0,SEEK_SET);
    if(!MP4D_open(&m->mp4,rd,m->f,sz)){ fclose(m->f); free(m); return 0; }
    m->atrack=(unsigned)-1;
    for(unsigned t=0;t<m->mp4.track_count;t++)
        if(m->mp4.track[t].handler_type==0x736f756e){ m->atrack=t; break; }   /* 'soun' */
    if(m->atrack==(unsigned)-1) for(unsigned t=0;t<m->mp4.track_count;t++)
        if(m->mp4.track[t].dsi_bytes>0){ m->atrack=t; break; }
    if(m->atrack==(unsigned)-1){ MP4D_close(&m->mp4); fclose(m->f); free(m); return 0; }
    m->aac=NeAACDecOpen();
    NeAACDecConfigurationPtr c=NeAACDecGetCurrentConfiguration(m->aac);
    c->outputFormat=FAAD_FMT_16BIT; NeAACDecSetConfiguration(m->aac,c);
    unsigned long sr=0; unsigned char chn=0;
    if(NeAACDecInit2(m->aac,m->mp4.track[m->atrack].dsi,m->mp4.track[m->atrack].dsi_bytes,&sr,&chn)<0){
        NeAACDecClose(m->aac); MP4D_close(&m->mp4); fclose(m->f); free(m); return 0; }
    m->hz=(int)sr; m->ch=chn?chn:2; m->ntotal=m->mp4.track[m->atrack].sample_count; m->nsample=0;
    if(hz)*hz=m->hz; if(ch)*ch=m->ch;
    if(dur_ms)*dur_ms = m->hz ? (long)((long long)m->ntotal*1024*1000/m->hz) : 0;
    return m;
}
long m4a_decode(m4a_t*m,short*out,int maxsamp){
    static unsigned char in[16384];
    for(;;){
        if(m->nsample>=m->ntotal) return 0;
        unsigned fb=0,ts=0,du=0;
        MP4D_file_offset_t ofs=MP4D_frame_offset(&m->mp4,m->atrack,m->nsample,&fb,&ts,&du); m->nsample++;
        if(!fb) continue;
        if(fb>sizeof in) fb=sizeof in;
        if(fseeko(m->f,(off_t)ofs,SEEK_SET)!=0) return 0;
        if(fread(in,1,fb,m->f)!=fb) return 0;
        NeAACDecFrameInfo fi; memset(&fi,0,sizeof fi);
        void*p=NeAACDecDecode(m->aac,&fi,in,fb);
        if(fi.error||!p||fi.samples==0) continue;
        long ns=(long)fi.samples; if(ns>maxsamp)ns=maxsamp;
        memcpy(out,p,ns*sizeof(short));
        return ns/(m->ch?m->ch:1);
    }
}
void m4a_seek(m4a_t*m,long ms){ if(!m||!m->hz)return;
    unsigned fr=(unsigned)((long long)ms*m->hz/1000/1024);
    if(fr>m->ntotal)fr=m->ntotal; m->nsample=fr; NeAACDecPostSeekReset(m->aac,(long)fr); }
void m4a_close(m4a_t*m){ if(!m)return; if(m->aac)NeAACDecClose(m->aac); MP4D_close(&m->mp4);
    if(m->f)fclose(m->f); free(m); }
