/* Moko life simulation — see pet_core.h. One step = one real minute. */
#include "pet_core.h"
#include <string.h>

#define PET_MAGIC 0x4D4F4B31u   /* "MOK1" */
static int rnd(pet_t*p,int n){ p->rng=p->rng*1103515245u+12345u; return (int)((p->rng>>16)%(unsigned)n); }

/* stage lengths in minutes, and how often needs drop */
static const int STAGE_LEN[]={ 5, 60, 24*60, 2*24*60, 6*24*60, 3*24*60, 0 };
static const int HUNGER_EVERY[]={ 0, 35, 60, 75, 90, 80, 0 };
static const int HAPPY_EVERY[] ={ 0, 40, 70, 85, 100, 90, 0 };
static const int POOP_EVERY[]  ={ 0, 70, 150, 180, 200, 220, 0 };
#define CALL_LIMIT 15            /* minutes a call may be ignored before it is a mistake */

void pet_new(pet_t*p,time_t now,unsigned seed){
    memset(p,0,sizeof *p); p->magic=PET_MAGIC; p->rng=seed?seed:1;
    p->born=p->last=(int64_t)now; p->stage=PS_EGG; p->form=PF_PLAIN;
    p->hunger=3; p->happy=3; p->weight=5; p->discipline=0; p->lights=1; p->attn_min=-1;
}
const char* pet_stage_name(const pet_t*p){
    static const char*n[]={"egg","baby","child","teen","adult","elder","gone"}; return n[p->stage]; }
const char* pet_form_name(const pet_t*p){
    if(p->stage==PS_EGG) return "Egg";
    if(p->stage<=PS_CHILD) return p->stage==PS_BABY?"Mokopip":"Mokolet";
    static const char*n[]={"Moko","Lumi","Bramble","Grub"}; return n[p->form]; }

int pet_sleep_time(const pet_t*p,time_t now){
    if(p->stage==PS_EGG||p->stage==PS_GONE) return 0;
    struct tm tm; time_t t=now; localtime_r(&t,&tm);
    int h=tm.tm_hour, start = p->stage==PS_BABY?20 : p->stage==PS_ELDER?21 : 22, end = p->stage==PS_ELDER?7:8;
    return h>=start || h<end;
}
int pet_attention(const pet_t*p){
    if(p->stage==PS_EGG||p->stage==PS_GONE) return 0;
    int a=0;
    if(!p->asleep){ if(p->hunger==0) a|=PA_HUNGRY; if(p->happy==0) a|=PA_SAD; if(p->fake_min>0) a|=PA_FAKE; }
    if(p->poop>=3) a|=PA_POOP;
    if(p->sick) a|=PA_SICK;
    if(p->asleep && p->lights) a|=PA_LIGHT;
    return a;
}
static void evolve(pet_t*p){
    int m=p->mistakes;
    p->stage++; p->stage_min=0; p->events|=1;
    if(p->stage==PS_BABY){ p->events|=2; p->weight=5; }
    if(p->stage==PS_TEEN || p->stage==PS_ADULT)           /* the look follows the care */
        p->form = m<=1?PF_LUMI : m<=4?PF_BRAMBLE : PF_GRUB;
    p->mistakes=0;
    if(p->stage==PS_GONE) p->events|=4;
}
static void minute(pet_t*p,time_t t){
    if(p->stage==PS_GONE) return;
    p->stage_min++;
    if(p->stage==PS_EGG){ if(p->stage_min>=STAGE_LEN[PS_EGG]) evolve(p); return; }
    int st=p->stage;
    /* sleep follows the clock; it wakes on its own in the morning */
    int night=pet_sleep_time(p,t);
    if(night && !p->asleep){ p->asleep=1; }
    if(!night && p->asleep){ p->asleep=0; p->lights=1; }
    /* needs (slower while asleep) */
    int slow = p->asleep?3:1;
    if(p->stage_min % (HUNGER_EVERY[st]*slow) == 0 && p->hunger>0) p->hunger--;
    if(p->stage_min % (HAPPY_EVERY[st]*slow) == 0 && p->happy>0) p->happy--;
    if(!p->asleep && p->stage_min % POOP_EVERY[st] == 0 && p->poop<4) p->poop++;
    if(p->snack_min>0 && --p->snack_min==0) p->snacks_hour=0;
    /* getting sick: dirty home, too many snacks, or now and then */
    if(!p->sick){
        int risk = (p->poop>=3?30:0) + (p->snacks_hour>=4?40:0) + 1;     /* per 10 000 per minute */
        if(rnd(p,10000)<risk){ p->sick=1; p->sick_min=0; p->events|=8; }
    } else p->sick_min++;
    /* misbehaviour: now and then it calls for nothing — scold it to build discipline */
    if(p->fake_min>0) p->fake_min--;
    else if(!p->asleep && st>=PS_CHILD && st<=PS_TEEN && p->discipline<100 && rnd(p,1000)<3) p->fake_min=15;
    /* ignored calls become care mistakes */
    int a=pet_attention(p) & ~PA_FAKE;
    if(a){ if(p->attn_min<0){ p->attn_min=0; p->attn_counted=0; }
        else if(++p->attn_min>=CALL_LIMIT && (p->attn_min-CALL_LIMIT)%60==0){ p->mistakes++; p->mistakes_total++; } }   /* +1 per hour still ignored */
    else p->attn_min=-1;
    /* long neglect while sick and starving ends its life early */
    if(p->sick && p->hunger==0) p->neglect_min++; else if(p->neglect_min>0) p->neglect_min--;
    if(p->neglect_min>=24*60){ p->stage=PS_GONE; p->events|=4; return; }
    if(STAGE_LEN[st] && p->stage_min>=STAGE_LEN[st]) evolve(p);
    p->age_days=(int)((t-p->born)/86400);
}
void pet_catch_up(pet_t*p,time_t now){
    if(p->magic!=PET_MAGIC) return;
    int64_t n=((int64_t)now-p->last)/60;
    if(n<=0) return;
    if(n>14*24*60){ p->last=(int64_t)now-14*24*60*60; n=14*24*60; }   /* cap: two weeks away */
    for(int64_t i=0;i<n;i++){ p->last+=60; minute(p,(time_t)p->last); }
}
static int alive_awake(pet_t*p){ if(p->stage==PS_GONE) return PR_GONE; if(p->stage==PS_EGG) return PR_NOTNEEDED;
    if(p->asleep) return PR_ASLEEP; return PR_OK; }
int pet_meal(pet_t*p){ int r=alive_awake(p); if(r) return r;
    if(p->hunger>=4) return PR_REFUSED;
    p->hunger++; p->weight++; return PR_OK; }
int pet_snack(pet_t*p){ int r=alive_awake(p); if(r) return r;
    if(p->happy<4) p->happy++; p->weight+=2; if(p->weight>99)p->weight=99;
    p->snacks_hour++; if(!p->snack_min) p->snack_min=60; return PR_OK; }
int pet_play_result(pet_t*p,int won){ int r=alive_awake(p); if(r) return r;
    if(won && p->happy<4) p->happy++;
    if(p->weight>5) p->weight--; return PR_OK; }
int pet_clean(pet_t*p){ if(p->stage==PS_GONE) return PR_GONE;
    if(!p->poop) return PR_NOTNEEDED; p->poop=0; return PR_OK; }
int pet_medicine(pet_t*p){ if(p->stage==PS_GONE) return PR_GONE;
    if(!p->sick){ if(p->stage!=PS_EGG){ p->mistakes++; p->mistakes_total++; } return PR_NOTNEEDED; }
    p->sick=0; p->sick_min=0; return PR_OK; }
int pet_lights(pet_t*p){ if(p->stage==PS_GONE) return PR_GONE; p->lights=!p->lights; return PR_OK; }
int pet_scold(pet_t*p){ int r=alive_awake(p); if(r) return r;
    if(p->fake_min>0){ p->fake_min=0; p->discipline+=25; if(p->discipline>100)p->discipline=100; return PR_OK; }
    if(p->happy>0) p->happy--; return PR_NOTNEEDED; }
