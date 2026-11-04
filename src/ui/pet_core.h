#ifndef R3_PET_CORE_H
#define R3_PET_CORE_H
/* Moko — an original virtual pet. Pure C life simulation (host-testable), advanced in
 * real time one minute at a time; it keeps living while the player is off. */
#include <stdint.h>
#include <time.h>

enum { PS_EGG, PS_BABY, PS_CHILD, PS_TEEN, PS_ADULT, PS_ELDER, PS_GONE };
enum { PF_PLAIN, PF_LUMI, PF_BRAMBLE, PF_GRUB };          /* look; adults by care quality */
enum { PA_NONE=0, PA_HUNGRY=1, PA_SAD=2, PA_POOP=4, PA_SICK=8, PA_LIGHT=16, PA_FAKE=32 };
enum { PR_OK, PR_REFUSED, PR_NOTNEEDED, PR_ASLEEP, PR_GONE };

typedef struct {
    uint32_t magic;
    int64_t  born, last;          /* unix times: birth, last simulated minute */
    int32_t  stage, form;
    int32_t  hunger, happy;       /* 0..4 */
    int32_t  weight, discipline;  /* grams-ish 5..99, 0..100 */
    int32_t  poop;                /* 0..4 piles */
    int32_t  sick, sick_min;      /* sick flag, minutes sick */
    int32_t  asleep, lights;      /* lights: 1 = on */
    int32_t  mistakes, mistakes_total;
    int32_t  attn_min;            /* minutes the current call has been ignored (-1 = none) */
    int32_t  attn_counted;        /* this call already counted as a mistake */
    int32_t  fake_min;            /* >0: a misbehaviour call is active (minutes left) */
    int32_t  stage_min;           /* minutes in the current stage */
    int32_t  snacks_hour, snack_min;
    int32_t  neglect_min;         /* minutes in a bad state (sick + starving) */
    int32_t  age_days;
    uint32_t rng;
    int32_t  events;              /* bits for the UI: 1 evolved, 2 hatched, 4 gone, 8 got sick */
} pet_t;

void pet_new(pet_t*p, time_t now, unsigned seed);
void pet_catch_up(pet_t*p, time_t now);       /* simulate whole minutes up to now (capped) */
int  pet_attention(const pet_t*p);            /* PA_* bits (what it's calling for) */
int  pet_meal(pet_t*p);
int  pet_snack(pet_t*p);
int  pet_play_result(pet_t*p, int won);       /* after the mini-game */
int  pet_clean(pet_t*p);
int  pet_medicine(pet_t*p);
int  pet_lights(pet_t*p);                     /* toggle */
int  pet_scold(pet_t*p);
const char* pet_stage_name(const pet_t*p);
const char* pet_form_name(const pet_t*p);
int  pet_sleep_time(const pet_t*p, time_t now);   /* night window for its stage */
#endif
