#include "hiscore.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void hs_load(hiscore_t*h){
    h->n=0; FILE*f=fopen(h->file,"r"); if(!f) return; char l[64];
    while(h->n<HS_N && fgets(l,sizeof l,f)){ int v; char d[12]="-";
        if(sscanf(l,"%d %11s",&v,d)<1) continue;
        h->s[h->n]=v; snprintf(h->d[h->n],12,"%s",d); h->n++; }
    fclose(f);
}
static void hs_save(const hiscore_t*h){ FILE*f=fopen(h->file,"w"); if(!f) return;
    for(int i=0;i<h->n;i++) fprintf(f,"%d %s\n",h->s[i],h->d[i]); fclose(f); }
int hs_add(hiscore_t*h,int score){
    if(score<=0) return -1;
    int r=0; while(r<h->n && h->s[r]>=score) r++;
    if(r>=HS_N) return -1;
    for(int i=(h->n<HS_N?h->n:HS_N-1);i>r;i--){ h->s[i]=h->s[i-1]; memcpy(h->d[i],h->d[i-1],12); }
    h->s[r]=score; time_t t=time(0); struct tm tm; localtime_r(&t,&tm);
    strftime(h->d[r],12,"%d/%m/%y",&tm);
    if(h->n<HS_N) h->n++;
    hs_save(h); return r;
}
void hs_format(const hiscore_t*h,int rank,int score,char*b,int cap){
    int n=snprintf(b,cap,"GAME OVER\n%s%d\n\nHIGH SCORES\n",rank==0?"NEW BEST  ":"",score);
    for(int i=0;i<h->n && n<cap;i++)
        n+=snprintf(b+n,cap-n,i==rank?"#E8731C %d.  %6d   %s#\n":"%d.  %6d   %s\n",i+1,h->s[i],h->d[i]);
    if(!h->n && n<cap) n+=snprintf(b+n,cap-n,"-\n");
    if(n<cap) snprintf(b+n,cap-n,"\ntap to play again");
}
