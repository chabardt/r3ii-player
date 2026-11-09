/* host test for drum_music (beat finding) on synthetic songs.
 * gcc -O1 -Wall -I src/drum -o build/drum_music_test tests/drum_music_test.c src/drum/drum_music.c -lm && build/drum_music_test */
#include "drum_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL line %d: ",__LINE__); printf(__VA_ARGS__); printf("\n"); fail++; } }while(0)
static dm_t d;
/* a song: kick on every beat, snare on 2 and 4, hats on eighths, a bass drone, some noise */
static void song(int rate,double bpm,double first,double secs,double noise,int swing){
    dm_init(&d,rate); int n=(int)(secs*rate); static int16_t buf[4096]; int k=0; unsigned rs=1;
    double beat=60.0/bpm;
    for(int i=0;i<n;i++){ double t=(double)i/rate, x=0;
        double tb=t-first; if(tb>=0){ double b=fmod(tb,beat), half=fmod(tb,beat/2); int bn=(int)(tb/beat);
            x+=0.8*exp(-b*30)*sin(2*M_PI*(50+80*exp(-b*40))*b);
            if(bn%2==1) { rs=rs*1664525u+1013904223u; x+=0.4*exp(-b*25)*((int)(rs>>16)-32768)/32768.0; }
            if(swing==0){ rs=rs*1664525u+1013904223u; x+=0.15*exp(-half*80)*((int)(rs>>16)-32768)/32768.0; } }
        x+=0.2*sin(2*M_PI*55*t);
        rs=rs*1664525u+1013904223u; x+=noise*((int)(rs>>16)-32768)/32768.0;
        if(x>1) x=1;
        if(x<-1) x=-1;
        buf[k++]=(int16_t)(x*30000);
        if(k==4096){ dm_feed(&d,buf,k); k=0; } }
    dm_feed(&d,buf,k);
}
static void measure(int rate,double bpm,double first,double secs,double*gbpm,double*drift,double*ph,int*conf,int*ok){
    song(rate,bpm,first,secs,0.05,0); dm_beat_t b=dm_estimate(&d);
    double period=b.period_x100/100.0, want_p=60.0*rate/bpm;
    *gbpm=b.bpm_x10/10.0; *drift=fabs(period-want_p)*600/rate*1000; *conf=b.confidence; *ok=b.ok;
    double p=fmod(b.phase/(double)rate-first+100*60.0/bpm,60.0/bpm); if(p>30.0/bpm) p-=60.0/bpm; *ph=p*1000;
}
/* in the game: a first estimate from the opening ~30 s to start drumming, then refined on the
 * whole song as it decodes. Check both. */
static void check_song(int rate,double bpm,double first){
    double g,dr,ph; int c,ok;
    measure(rate,bpm,first,30,&g,&dr,&ph,&c,&ok);
    printf("  %d Hz %.2f BPM, 30 s: %.2f BPM conf %d phase %+.1f ms",rate,bpm,g,c,ph);
    CHECK(ok,"no beat found at %.2f",bpm); CHECK(fabs(g-bpm)<0.3,"bpm %.2f vs %.2f",g,bpm); CHECK(fabs(ph)<15,"30 s phase %.1f ms",ph);
    measure(rate,bpm,first,120,&g,&dr,&ph,&c,&ok);
    printf(" | 2 min: drift %.1f ms/600 beats, phase %+.1f ms\n",dr,ph);
    CHECK(dr<40,"drift %.1f ms",dr); CHECK(fabs(ph)<15,"phase %.1f ms",ph);   /* a steady bias is what TIMING corrects */
}
int main(void){
    check_song(48000,120,0.25);
    check_song(44100,97.3,0.61);
    check_song(44100,128,0.05);
    check_song(48000,88.0,1.20);
    check_song(44100,165,0.33);
    /* no beat: noise only */
    song(44100,120,1e9,20,0.4,0); dm_beat_t b=dm_estimate(&d);
    printf("  noise: conf %d\n",b.confidence); CHECK(!b.ok,"found a beat in noise (conf %d)",b.confidence);
    if(fail){ printf("%d failure(s)\n",fail); return 1; }
    printf("drum_music: all tests passed\n"); return 0;
}
