/* host test for drum_core (rhythm rules).
 * gcc -O1 -Wall -I src/drum -o build/drum_core_test tests/drum_core_test.c src/drum/drum_core.c && build/drum_core_test */
#include "drum_core.h"
#include <stdio.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL line %d: ",__LINE__); printf(__VA_ARGS__); printf("\n"); fail++; } }while(0)
#define MS(x) ((int64_t)(x)*DR_RATE/1000)

static rc_t r;
static int64_t T(int64_t b,int ms){ return rc_beat_time(&r,b)+MS(ms); }
static void clear(void){ r.ev_judge=J_NONE; r.ev_cmd=CMD_NONE; r.ev_break=0; r.ev_fever_on=r.ev_fever_off=0; }
/* play command c starting on beat b (offset ms on every hit), ticking time along */
static int play(int c,int64_t b,int ms){ int last=J_NONE;
    for(int i=0;i<4;i++){ rc_tick(&r,T(b+i,ms)); last=rc_hit(&r,DR_PATTERN[c][i],T(b+i,ms)); }
    return last; }

int main(void){
    rc_init(&r,120,1000);
    CHECK(r.beat==24000,"beat %lld",(long long)r.beat);
    CHECK(rc_nearest_beat(&r,T(3,-200))==3 && rc_nearest_beat(&r,T(3,240))==3 && rc_nearest_beat(&r,T(3,260))==4,"nearest beat");

    /* judgement windows */
    clear(); CHECK(rc_hit(&r,DR_DUM,T(0,20))==J_PERFECT,"perfect");
    rc_init(&r,120,1000); clear(); CHECK(rc_hit(&r,DR_DUM,T(0,-90))==J_GOOD,"good early");
    rc_init(&r,120,1000); clear(); CHECK(rc_hit(&r,DR_DUM,T(0,180))==J_MISS,"miss");

    /* a command and its response */
    rc_init(&r,120,1000); clear();
    play(CMD_MARCH,8,0);
    CHECK(r.ev_cmd==CMD_MARCH && r.combo==1,"march recognised (cmd %d combo %d)",r.ev_cmd,r.combo);
    int bi=-1; CHECK(rc_responding(&r,T(12,10),&bi)==CMD_MARCH && bi==0,"responding on beat 12");
    CHECK(rc_responding(&r,T(15,400),&bi)==CMD_MARCH && bi==3,"responding on beat 15");
    CHECK(rc_responding(&r,T(16,10),&bi)==CMD_NONE,"response over");

    /* streak: next command right after the response, up to fever */
    clear(); play(CMD_STRIKE,16,30); CHECK(r.combo==2,"combo 2 (%d)",r.combo);
    clear(); play(CMD_GUARD,24,-40); CHECK(r.combo==3 && !r.fever,"combo 3");
    clear(); play(CMD_MARCH,32,0); CHECK(r.combo==4 && r.fever && r.ev_fever_on,"fever at 4");
    clear(); play(CMD_RALLY,40,60); CHECK(r.ev_cmd==CMD_RALLY && r.combo==5 && r.fever,"rally keeps fever");

    /* drumming during the response breaks the streak */
    clear(); rc_tick(&r,T(45,0)); CHECK(rc_hit(&r,DR_KA,T(45,0))==J_MISS && r.ev_break && r.ev_fever_off && r.combo==0,"hit over response");

    /* late restart: streak lost, but the new command still works */
    rc_init(&r,120,1000); clear();
    play(CMD_MARCH,0,0); play(CMD_MARCH,4+4,0); CHECK(r.combo==2,"two in a row");
    clear(); rc_tick(&r,T(16,200)); CHECK(r.ev_break && r.combo==0,"timeout after response (combo %d)",r.combo);
    clear(); play(CMD_MARCH,18,0); CHECK(r.ev_cmd==CMD_MARCH && r.combo==1,"fresh start after timeout");

    /* unknown pattern, skipped beat, double hit */
    rc_init(&r,120,1000); clear();
    rc_hit(&r,DR_DUM,T(0,0)); rc_hit(&r,DR_DUM,T(1,0)); rc_hit(&r,DR_DUM,T(2,0)); rc_hit(&r,DR_DUM,T(3,0));
    CHECK(r.ev_cmd==CMD_NONE && r.nseq==0,"DUM DUM DUM DUM is nothing");
    rc_hit(&r,DR_DUM,T(5,0)); rc_hit(&r,DR_KA,T(7,0)); CHECK(r.nseq==1,"skipped beat restarts the sequence");
    rc_init(&r,120,1000); rc_hit(&r,DR_DUM,T(0,0)); CHECK(rc_hit(&r,DR_KA,T(0,40))==J_MISS && r.nseq==0,"two hits on one beat");
    /* half command abandoned */
    rc_init(&r,120,1000); rc_hit(&r,DR_DUM,T(0,0)); rc_hit(&r,DR_KA,T(1,0)); rc_tick(&r,T(2,300)); CHECK(r.nseq==0,"stale sequence dropped");
    /* negative beats (input before t0) don't crash the maths */
    rc_init(&r,120,48000); CHECK(rc_nearest_beat(&r,10)==-2 || rc_nearest_beat(&r,10)==-1,"before t0");

    if(fail){ printf("%d failure(s)\n",fail); return 1; }
    printf("drum_core: all tests passed\n"); return 0;
}
