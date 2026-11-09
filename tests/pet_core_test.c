/* host test for pet_core: simulated lives with a careful and a neglectful owner.
 * gcc -O1 -Wall -I src/ui -o build/pet_test tests/pet_core_test.c src/ui/pet_core.c && build/pet_test */
#include "pet_core.h"
#include <stdio.h>
#include <string.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL: " __VA_ARGS__); printf("\n"); fail++; return; } }while(0)

/* care: 2 = attentive (checks every 10 min, perfect), 1 = casual (every 3 h, sloppy), 0 = never */
static void life(unsigned seed,int care,const char*label){
    pet_t p; time_t t0=1793750400; /* 2026-11-04 */ pet_new(&p,t0,seed);
    int asleep_min=0, night_min=0, maxpoop=0, evolutions=0, sick=0, stage_seen[7]={0}, forms_adult=-1;
    time_t t=t0;
    for(int m=0;m<20*24*60 && p.stage!=PS_GONE;m++){
        t+=60; pet_catch_up(&p,t);
        if(p.events&1) evolutions++; if(p.events&8) sick++; p.events=0;
        stage_seen[p.stage]=1;
        if(p.stage==PS_ADULT && forms_adult<0) forms_adult=p.form;
        CHECK(p.hunger>=0&&p.hunger<=4&&p.happy>=0&&p.happy<=4,"%s range h%d j%d",label,p.hunger,p.happy);
        CHECK(p.poop>=0&&p.poop<=4&&p.weight>=5&&p.weight<=99&&p.discipline>=0&&p.discipline<=100,"%s range2",label);
        if(p.poop>maxpoop) maxpoop=p.poop;
        if(pet_sleep_time(&p,t)){ night_min++; asleep_min+=p.asleep; }
        int every = care==2?10 : care==1?180 : 0;
        if(every && m%every==0){
            int a=pet_attention(&p);
            if(a&PA_LIGHT) pet_lights(&p);
            if(!p.asleep && !p.lights) pet_lights(&p);
            if(a&PA_SICK) pet_medicine(&p);
            if(a&PA_POOP || (care==2 && p.poop)) pet_clean(&p);
            if(a&PA_FAKE) pet_scold(&p);
            while(p.hunger<(care==2?4:2) && pet_meal(&p)==PR_OK);
            if(p.happy<(care==2?3:1)) pet_play_result(&p,1);
        }
    }
    printf("%-12s seed %u: stage %-5s form %-8s evolutions %d adult-form %d mistakes %d sick %d maxpoop %d asleep at night %d%% age %dd\n",
        label,seed,pet_stage_name(&p),pet_form_name(&p),evolutions,forms_adult,p.mistakes_total,sick,maxpoop,
        night_min?asleep_min*100/night_min:0,p.age_days);
    CHECK(stage_seen[PS_BABY],"%s never hatched",label);
    if(care==2){ CHECK(forms_adult==PF_LUMI,"%s attentive owner should raise a Lumi (got %d)",label,forms_adult);
                 CHECK(p.stage==PS_GONE && p.age_days>=11,"%s should live a full life (stage %d age %d)",label,p.stage,p.age_days); }
    if(care==0) CHECK(p.stage==PS_GONE && p.age_days<6,"%s neglected pet should not survive long (age %d)",label,p.age_days);
    CHECK(night_min==0 || asleep_min*100/night_min>90,"%s sleeps at night",label);
}
int main(void){
    for(unsigned s=1;s<=3;s++){ life(s,2,"attentive"); life(s,1,"casual"); life(s,0,"neglect"); }
    /* offline catch-up: a whole week in one call equals minute-by-minute */
    pet_t a,b; pet_new(&a,1793750400,7); pet_new(&b,1793750400,7);
    pet_catch_up(&a,1793750400+7*86400);
    for(int m=1;m<=7*24*60;m++) pet_catch_up(&b,1793750400+m*60);
    a.events=b.events=0;
    if(memcmp(&a,&b,sizeof a)){ printf("FAIL: catch-up differs\n"); fail++; } else printf("catch-up: identical\n");
    printf(fail?"FAILED %d\n":"ALL OK\n",fail); return fail!=0;
}
