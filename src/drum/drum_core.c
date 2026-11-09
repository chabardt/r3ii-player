/* drum_core.c — see drum_core.h */
#include "drum_core.h"

/* Drumfolk's own command patterns (DUM low drum, KA high drum, TOK wood block, CHI cymbal) */
const unsigned char DR_PATTERN[CMD_N][4]={
    {DR_DUM,DR_KA, DR_DUM,DR_KA },   /* MARCH   */
    {DR_KA, DR_KA, DR_KA, DR_DUM},   /* STRIKE  */
    {DR_TOK,DR_DUM,DR_TOK,DR_DUM},   /* GUARD   */
    {DR_DUM,DR_DUM,DR_TOK,DR_TOK},   /* RETREAT */
    {DR_CHI,DR_CHI,DR_TOK,DR_DUM},   /* RALLY   */
    {DR_CHI,DR_KA, DR_CHI,DR_KA },   /* SPECIAL: super fever only */
};
const char *const DR_CMD_NAME[CMD_N]={"MARCH","STRIKE","GUARD","RETREAT","RALLY","SPECIAL"};
const char *const DR_DRUM_NAME[DR_NDRUMS]={"DUM","KA","TOK","CHI"};

static int64_t win(int ms){ return (int64_t)ms*DR_RATE/1000; }
static int64_t floordiv(int64_t a,int64_t b){ int64_t q=a/b; if((a%b!=0)&&((a<0)!=(b<0))) q--; return q; }

void rc_init(rc_t*r,int bpm,int64_t t0){
    r->t0=t0; r->beat=(int64_t)DR_RATE*60/bpm; r->nseq=0; r->resp_cmd=CMD_NONE; r->resp_beat=-100;
    r->next_start=-1; r->combo=0; r->fever=0; r->super=0;
    r->ev_judge=J_NONE; r->ev_cmd=CMD_NONE; r->ev_break=0; r->ev_fever_on=r->ev_fever_off=0; r->ev_super_on=r->ev_not_ready=0;
}
int64_t rc_beat_time(const rc_t*r,int64_t b){ return r->t0+b*r->beat; }
int64_t rc_nearest_beat(const rc_t*r,int64_t t){ return floordiv(t-r->t0+r->beat/2,r->beat); }
int rc_match(const unsigned char p[4]){
    for(int c=0;c<CMD_N;c++){ int ok=1; for(int i=0;i<4;i++) if(p[i]!=DR_PATTERN[c][i]) ok=0; if(ok) return c; }
    return CMD_NONE;
}
static void streak_break(rc_t*r){
    if(r->combo>0) r->ev_break=1;
    if(r->fever) r->ev_fever_off=1;
    r->combo=0; r->fever=0; r->super=0; r->next_start=-1;
}
int rc_responding(const rc_t*r,int64_t t,int*beat_in){
    if(r->resp_cmd==CMD_NONE) return CMD_NONE;
    int64_t bt=floordiv(t-r->t0,r->beat);
    if(bt<r->resp_beat || bt>=r->resp_beat+4) return CMD_NONE;
    if(beat_in) *beat_in=(int)(bt-r->resp_beat);
    return r->resp_cmd;
}
int rc_hit(rc_t*r,int drum,int64_t t){
    int64_t b=rc_nearest_beat(r,t), d=t-rc_beat_time(r,b); if(d<0) d=-d;
    if(r->resp_cmd!=CMD_NONE && b>=r->resp_beat && b<r->resp_beat+4){   /* drumming over the squad */
        r->nseq=0; streak_break(r); return r->ev_judge=J_MISS; }
    if(d>win(DR_GOOD_MS)){ r->nseq=0; streak_break(r); return r->ev_judge=J_MISS; }
    int j = d<=win(DR_PERFECT_MS) ? J_PERFECT : J_GOOD;
    if(r->nseq>0 && b==r->seq_beat[r->nseq-1]){ r->nseq=0; streak_break(r); return r->ev_judge=J_MISS; }   /* two on one beat */
    if(r->nseq>0 && b!=r->seq_beat[r->nseq-1]+1){ r->nseq=0; if(r->combo>0) streak_break(r); }       /* skipped a beat */
    if(r->nseq==0 && r->combo>0 && b!=r->next_start) streak_break(r);                                 /* late / early restart */
    r->seq[r->nseq]=(unsigned char)drum; r->seq_beat[r->nseq]=b; r->nseq++;
    if(r->nseq==4){
        int c=rc_match(r->seq); r->nseq=0;
        if(c==CMD_NONE){ streak_break(r); return r->ev_judge=j; }
        if(c==CMD_SPECIAL && !r->super){ r->ev_not_ready=1; streak_break(r); return r->ev_judge=j; }
        r->combo++;
        if(c==CMD_SPECIAL){ r->combo=DR_FEVER_AT; r->super=0; }      /* spent: back to plain fever */
        if(r->combo>=DR_FEVER_AT && !r->fever){ r->fever=1; r->ev_fever_on=1; }
        if(r->combo>=DR_SUPER_AT && !r->super){ r->super=1; r->ev_super_on=1; }
        r->resp_cmd=c; r->resp_beat=b+1; r->next_start=b+5; r->ev_cmd=c;
    }
    return r->ev_judge=j;
}
void rc_tick(rc_t*r,int64_t t){
    /* a half-entered command whose next beat went by */
    if(r->nseq>0 && t>rc_beat_time(r,r->seq_beat[r->nseq-1]+1)+win(DR_GOOD_MS)){
        r->nseq=0; if(r->combo>0) streak_break(r); }
    /* the streak needs the next command to start right after the response */
    if(r->combo>0 && r->next_start>=0 && t>rc_beat_time(r,r->next_start)+win(DR_GOOD_MS)
       && !(r->nseq>0 && r->seq_beat[0]==r->next_start))
        streak_break(r);
}
