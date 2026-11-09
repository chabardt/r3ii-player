/* drum_music.c — see drum_music.h. Integer only. */
#include "drum_music.h"
#include <string.h>

void dm_init(dm_t*d,int rate){ memset(d,0,sizeof *d); d->rate=rate; d->hop=rate/200; if(d->hop<1) d->hop=1; }
void dm_feed(dm_t*d,const int16_t*x,int n){
    for(int i=0;i<n;i++){ int v=x[i]; d->acc+= v<0?-v:v;
        if(++d->fill<d->hop) continue;
        int32_t e=(int32_t)(d->acc/d->hop); d->acc=0; d->fill=0;
        int32_t o=e-d->avg; if(o<0) o=0; if(o>65535) o=65535;   /* rise above the recent level */
        d->avg+=(e-d->avg)/16; d->prev=e;
        if(d->n<DM_MAXHOPS) d->on[d->n++]=(uint16_t)o; }
}
/* onset energy on the best beat grid of period P (samples x100); returns score, sets phase (samples x100) */
static int64_t comb(const dm_t*d,int n,int64_t P,int64_t*phase,int NB){
    int64_t h[512]; memset(h,0,sizeof h); int64_t hop100=(int64_t)d->hop*100, sum=0;
    for(int i=0;i<n;i++){ int w=d->on[i]; if(!w) continue; int64_t t=(int64_t)i*hop100+hop100/2;
        int b=(int)((t%P)*NB/P); h[b]+=w; sum+=w; }
    int best=0; int64_t bs=-1;
    for(int b=0;b<NB;b++){ int64_t s=h[(b+NB-1)%NB]+2*h[b]+h[(b+1)%NB]; if(s>bs){ bs=s; best=b; } }
    if(phase){ /* centroid of the onsets around the winning bin, for ~1 ms phase */
        int64_t c=(int64_t)best*P/NB+P/NB/2, wsum=0, dsum=0;
        for(int i=0;i<n;i++){ int w=d->on[i]; if(!w) continue; int64_t t=(int64_t)i*hop100+hop100/2;
            int64_t dd=(t%P)-c; if(dd>P/2) dd-=P; if(dd<-P/2) dd+=P;
            if(dd>-P*3/(2*NB) && dd<P*3/(2*NB)){ wsum+=w; dsum+=w*dd; } }
        c+= wsum? dsum/wsum : 0; if(c<0) c+=P; *phase=c; }
    return sum? bs*1000/sum : 0;                     /* share of the onset energy on the grid, x1000 (x4 smoothing) */
}
dm_beat_t dm_estimate(const dm_t*d){
    dm_beat_t r={0}; int n=d->n; if(n<200*6) return r;          /* need ~6 s */
    /* coarse: autocorrelation of the mean-removed envelope over 80..170 BPM, on <= 40 s */
    int m=n<200*40?n:200*40; int64_t mean=0; for(int i=0;i<m;i++) mean+=d->on[i]; mean/=m;
    int Lmin=200*60/170, Lmax=200*60/80; int64_t ac[2*(200*60/80)+2]; int bestL=Lmin; int64_t bestS=-((int64_t)1<<62);
    for(int L=Lmin;L<=2*Lmax && L<m;L++){ int64_t s=0; for(int i=0;i+L<m;i++) s+=(int64_t)(d->on[i]-mean)*(d->on[i+L]-mean); ac[L]=s; }
    for(int L=Lmin;L<=Lmax;L++){ int64_t s=ac[L]+(2*L<m && 2*L<=2*Lmax ? ac[2*L]/2 : 0); if(s>bestS){ bestS=s; bestL=L; } }
    /* rough grid: period and phase that put the most onset energy on the beat (64 bins) */
    int64_t P=(int64_t)bestL*d->hop*100, span=P*3/100, ph=0;
    { int64_t bp=P, bs=-1, st=P/2000>1?P/2000:1; for(int64_t q=P-span;q<=P+span;q+=st){ int64_t sc=comb(d,n,q,0,64); if(sc>bs){ bs=sc; bp=q; } } P=bp; }
    int64_t score=comb(d,n,P,&ph,64);
    /* precise grid: least-squares fit of beat time = t0 + k*period over the onset peaks of the
     * whole span (every beat heard adds precision; done once per estimate, so doubles are fine) */
    { int64_t mean2=0; for(int i=0;i<n;i++) mean2+=d->on[i]; mean2=mean2/n*3;   /* strong peaks: kicks, snares */
      double per=P/100.0, t0=ph/100.0;
      for(int it=0;it<10;it++){ double Sw=0,Sk=0,Skk=0,St=0,Skt=0;
          for(int i=1;i+1<n;i++){ int w=d->on[i]; if(w<=mean2||w<d->on[i-1]||w<d->on[i+1]) continue;
              double t=(double)i*d->hop+d->hop/2.0, kf=(t-t0)/per; double k=kf>=0?(double)(int64_t)(kf+0.5):-(double)(int64_t)(-kf+0.5);
              double res=t-(t0+k*per); if(res<-per*0.08||res>per*0.08) continue;
              double ww=w; Sw+=ww; Sk+=ww*k; Skk+=ww*k*k; St+=ww*t; Skt+=ww*k*t; }
          double den=Sw*Skk-Sk*Sk; if(Sw<1||den<=0) break;
          double nb=(Sw*Skt-Sk*St)/den, na=(St-nb*Sk)/Sw;
          if(nb<per*0.97||nb>per*1.03) break;
          per=nb; t0=na; }
      P=(int64_t)(per*100+0.5); while(t0<0) t0+=per; while(t0>=per) t0-=per; ph=(int64_t)(t0*100); }
    ph-=(int64_t)d->rate*100*17/1000; if(ph<0) ph+=P;          /* onset energy peaks ~17 ms after the hit */
    r.period_x100=P; r.phase=ph/100; r.bpm_x10=(int)((int64_t)d->rate*60*10*100/P);
    /* on a random grid ~3/64 of the energy lands in the smoothed best bin (x4 => ~190/1000) */
    int c=(int)((score-190)*100/600); r.confidence=c<0?0:c>100?100:c; r.ok=r.confidence>=25;
    return r;
}
