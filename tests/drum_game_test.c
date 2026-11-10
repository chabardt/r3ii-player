/* host test for drum_game: data sanity, damage rules, and an autopilot playing every mission.
 * gcc -O1 -Wall -I src/drum -o build/drum_game_test tests/drum_game_test.c src/drum/drum_game.c src/drum/drum_core.c -lm && build/drum_game_test */
#include "drum_game.h"
#include "drum_core.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL line %d: ",__LINE__); printf(__VA_ARGS__); printf("\n"); fail++; } }while(0)

static int in_reach(const dg_t*g){
    for(int i=0;i<DG_MAXU;i++){ const unit_t*u=&g->u[i]; if(!u->on||u->hp<=0||u->weapon<0) continue;
        for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0) continue;
            if((e->traits&TR_FLYING) && !DG_CLASS[u->cls].ranged) continue;
            float dx=dg_front(e)-u->x; if(dx>=-30-2*DG_ETYPE[e->type].hw && dx<=DG_WEAPON[u->weapon].range) return 1; } }
    return 0;
}
static int front_reaches(const dg_t*g){        /* the front-most unit (if melee) has a target */
    int f=-1; float fx=-1e9f; for(int i=0;i<DG_MAXU;i++) if(g->u[i].on&&g->u[i].hp>0&&g->u[i].x>fx){ fx=g->u[i].x; f=i; }
    if(f<0) return 0; const unit_t*u=&g->u[f]; if(DG_CLASS[u->cls].ranged) return 1;
    for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0||(e->traits&TR_FLYING)) continue;
        float dx=dg_front(e)-u->x; if(dx>=-30-2*DG_ETYPE[e->type].hw && dx<=DG_WEAPON[u->weapon].range+10) return 1; }
    for(int k=0;k<DG_MAXE;k++) if(g->e[k].on&&g->e[k].hp>0&&!(g->e[k].traits&TR_FLYING)) return 0;
    return 1;                                     /* only flyers left: nothing to walk up to */
}
static int has(const dg_t*g,int c){ for(int i=0;i<DG_MAXU;i++) if(g->u[i].on&&g->u[i].hp>0&&g->u[i].cls==c) return 1; return 0; }
static float hp_frac(const dg_t*g){ float h=0,m=0; for(int i=0;i<DG_MAXU;i++) if(g->u[i].on&&g->u[i].hp>0){ h+=g->u[i].hp; m+=g->u[i].maxhp; } return m>0?h/m:0; }
/* smart: guard against telegraphed boss blows, rally when hurt; dumb: never guards */
static int choose(const dg_t*g,int smart){
    if(smart){ int k=dg_boss(g); const bmove_t*mv=k>=0?dg_boss_move(g,k):0;
        /* guard only if the blow lands inside the window a GUARD drummed now will cover
         * (response starts in 4 beats, the stance holds 12) — otherwise hit it while it winds up */
        if(mv && mv->kind!=MV_SUMMON && mv->kind!=MV_HEAL && mv->kind!=MV_ROAR
           && g->e[k].windup>=4 && g->e[k].windup<=11 && g->beat+g->e[k].windup>g->guard_until) return CMD_GUARD;
        for(int j=0;j<DG_MAXE;j++){ const enemy_t*e=&g->e[j];          /* a ballista bolt on its way */
            if(e->on&&e->hp>0&&e->windup&&(e->traits&TR_HEAVYSHOT)&&e->next+1>=4&&e->next+1<=11&&g->beat+e->next+1>g->guard_until) return CMD_GUARD; }
        if(hp_frac(g)<0.55f && has(g,CL_MENDER)) return CMD_RALLY; }
    /* strike when the front line reaches something (or the squad is all ranged); when only the
     * back row reaches, alternate strike / march to close in */
    static int alt; alt^=1;
    if(!in_reach(g)) return CMD_MARCH;
    if(front_reaches(g)) return CMD_STRIKE;
    return alt ? CMD_STRIKE : CMD_MARCH;
}
typedef struct { int won, lost, beats, specials; float hp; } result_t;
static result_t play(int mission,const armory_t*a,uint32_t seed,int smart){
    static dg_t g; dg_start(&g,mission,a,seed);
    int64_t beat=0; int cmd=CMD_NONE, combo=0, specials=0; result_t r={0};
    while(!g.won && !g.lost && beat<8000){
        int cyc=(int)(beat%8);
        if(cyc==0) cmd=choose(&g,smart);
        if(cyc==0 && smart && combo>=DR_SUPER_AT && cmd!=CMD_GUARD && in_reach(&g)) cmd=CMD_SPECIAL;   /* spend super fever */
        int fever=combo>=DR_SUPER_AT?2:combo>=DR_FEVER_AT;
        dg_beat(&g,beat,cyc<4?CMD_NONE:cmd,cyc<4?cyc:cyc-4,fever);
        if(cyc==7){ combo++; if(cmd==CMD_SPECIAL){ combo=DR_FEVER_AT; specials++; } }
        for(int s=0;s<30 && !g.won && !g.lost;s++) dg_step(&g,0.5f/30);
        for(int i=0;i<DG_MAXU;i++) if(g.u[i].on){
            if(!(g.u[i].hp>=0 && g.u[i].hp<=g.u[i].maxhp+0.01f) || isnan(g.u[i].x)){ CHECK(0,"unit %d hp %f x %f",i,g.u[i].hp,g.u[i].x); return r; } }
        for(int k=0;k<DG_MAXE;k++) if(g.e[k].on && (isnan(g.e[k].hp)||g.e[k].hp>g.e[k].maxhp+0.01f)){ CHECK(0,"enemy %d hp %f",k,g.e[k].hp); return r; }
        if(!(g.sx>=120 && g.sx<=g.m->length+200)){ CHECK(0,"squad x %f",g.sx); return r; }
        beat++;
    }
    r.won=g.won; r.lost=g.lost; r.beats=(int)beat; r.hp=hp_frac(&g); r.specials=specials;
    if(getenv("DG_DEBUG") && !g.won){
        printf("  [m%d seed %u] %s at beat %lld, squad x %.0f:",mission+1,seed,g.lost?"wiped":"timeout",(long long)beat,g.sx);
        for(int i=0;i<DG_MAXU;i++) if(g.u[i].on) printf(" %s %.0f/%.0f",DG_CLASS[g.u[i].cls].name,g.u[i].hp,g.u[i].maxhp);
        printf("\n   foes left:"); for(int k=0;k<DG_MAXE;k++) if(g.e[k].on&&g.e[k].hp>0) printf(" %s@%.0f(%.0f)",DG_ETYPE[g.e[k].type].name,g.e[k].x,g.e[k].hp);
        printf("\n"); }
    return r;
}
static void loadout(armory_t*a,int n,const int*spec){ a->nsquad=0; for(int i=0;i<n;i++){ if(spec[i*3+1]<=0 && spec[i*3]==0 && spec[i*3+2]==0 && i) break;
    a->squad[i]=(loadout_t){spec[i*3],spec[i*3+1],spec[i*3+2]}; a->nsquad++; } }

int main(void){
    /* data sanity */
    for(int w=0;w<DG_NWEAPON;w++){ CHECK(DG_WEAPON[w].classes!=0 && DG_WEAPON[w].dmg>0,"weapon %d",w); }
    for(int t=0;t<ET_N;t++){ int b=DG_ETYPE[t].boss; CHECK(b<DG_NBOSS,"etype %d boss %d",t,b);
        if(b>=0){ CHECK(DG_BOSS[b].nmv>=3,"boss %d moves",b); int roar=0; for(int i=0;i<DG_BOSS[b].nmv;i++) roar|=DG_BOSS[b].mv[i].kind==MV_ROAR;
            CHECK(DG_BOSS[b].elite || roar,"boss %s has no enrage",DG_BOSS[b].name); } }
    for(int m=0;m<DG_NMISSION;m++){ const mission_t*ms=&DG_MISSION[m]; int boss=0, sub=0;
        for(int i=0;i<ms->ne;i++){ int b=DG_ETYPE[ms->e[i].type].boss; if(b>=0){ if(DG_BOSS[b].elite) sub++; else boss++; }
            if(i) CHECK(ms->e[i].x>ms->e[i-1].x,"mission %d spawns not sorted",m);
            CHECK(ms->e[i].x<ms->length,"mission %d spawn past the banner",m); }
        CHECK(boss==1 && sub==1,"mission %d: %d boss %d sub-boss",m,boss,sub);
        CHECK(DG_ETYPE[ms->e[ms->ne-1].type].boss>=0 && !DG_BOSS[DG_ETYPE[ms->e[ms->ne-1].type].boss].elite,"mission %d doesn't end on its boss",m);
        for(int i=0;i<ms->nreward;i++){ int r=ms->reward[i]; CHECK(r>=0 && (r<DG_NWEAPON || (r>=100 && r-100<DG_NSHIELD)),"mission %d reward %d",m,r); }
        int cages=0; for(int i=0;i<ms->ne;i++) cages+=(DG_ETYPE[ms->e[i].type].traits&TR_CAGE)!=0;
        CHECK(ms->cap>=1 && ms->cap<=DG_MAXU,"mission %d cap",m);
        CHECK(ms->req<m,"mission %d needs a later mission",m);
        CHECK(!ms->rescue || cages>0,"rescue mission %d has no cage",m);
        CHECK(ms->ne<=(int)(sizeof ms->e/sizeof ms->e[0]) && ms->ne<=DG_MAXE,"mission %d spawns",m); }
    CHECK(DG_NWEAPON<=32 && DG_NSHIELD<=32,"gear fits the armory bit masks");
    { int slots=DG_SLOTS0; for(int m=0;m<DG_NMISSION;m++) slots+=DG_MISSION[m].rescue; CHECK(slots==DG_MAXU,"rescues lead to a full squad (%d)",slots); }
    /* damage rules */
    CHECK(dg_mult(DT_PIERCE,EL_NONE,TR_ARMORED)==0.5f && dg_mult(DT_CRUSH,EL_NONE,TR_ARMORED)==1.5f,"armour");
    CHECK(dg_mult(DT_PIERCE,EL_FIRE,TR_WOODEN)==1.0f,"fire javelin vs wood");
    CHECK(dg_mult(DT_SLASH,EL_FIRE,TR_FROSTBORN)==1.5f && dg_mult(DT_SLASH,EL_FIRE,TR_FIREPROOF)==0.5f,"fire vs frostborn / fireproof");
    CHECK(!dg_shield_ok(CL_SLINGER,1) && dg_shield_ok(CL_SLINGER,0) && dg_shield_ok(CL_WARDEN,4) && !dg_shield_ok(CL_LANCER,4),"shield rules");
    CHECK(dg_weapon_ok(CL_LANCER,0) && !dg_weapon_ok(CL_LANCER,5),"weapon rules");

    armory_t a; dg_armory_default(&a);
    /* squads chosen for each mission, gear from earlier rewards */
    /* squads chosen for each mission (as many as it deploys), gear from earlier rewards */
    static const int SQ[12][24]={
        {CL_WARDEN,5,2,  CL_LANCER,0,1,  CL_SLINGER,9,0,  CL_MENDER,16,0},          /* starting armory */
        {CL_WARDEN,7,5,  CL_LANCER,1,3,  CL_SLINGER,10,0, CL_MENDER,16,0},          /* crush + ranged for iron & wings */
        {CL_WARDEN,7,5,  CL_LANCER,2,3,  CL_SLINGER,11,0, CL_MENDER,17,0},          /* crush for the bulwark, fire vs frostborn */
        {CL_WARDEN,7,6,  CL_LANCER,19,5, CL_SLINGER,12,0, CL_MENDER,17,0},          /* frost vs the fireproof beast */
        /* act II: wisps are fireproof flyers (frost / plain bows), shieldbearers want melee crush */
        {CL_WARDEN,7,5,  CL_LANCER,19,3, CL_SLINGER,11,0, CL_MENDER,17,0},
        {CL_WARDEN,7,5,  CL_LANCER,22,3, CL_SLINGER,11,0, CL_MENDER,17,0, CL_BREAKER,14,0},
        {CL_WARDEN,23,5, CL_LANCER,22,3, CL_SLINGER,24,0, CL_MENDER,18,0, CL_BREAKER,21,0},
        {CL_WARDEN,23,6, CL_LANCER,22,3, CL_SLINGER,11,0, CL_MENDER,18,0, CL_BREAKER,14,0, CL_BREAKER,21,0},
        {CL_WARDEN,23,7, CL_LANCER,22,5, CL_SLINGER,20,0, CL_MENDER,26,0, CL_BREAKER,25,0, CL_SLINGER,11,0},
        {CL_WARDEN,23,7, CL_LANCER,27,8, CL_SLINGER,24,0, CL_MENDER,26,0, CL_BREAKER,25,0, CL_BREAKER,14,0},
        {CL_WARDEN,23,7, CL_LANCER,19,8, CL_SLINGER,12,0, CL_MENDER,26,0, CL_BREAKER,25,0, CL_WARDEN,7,4, CL_SLINGER,20,0},
        {CL_WARDEN,23,7, CL_LANCER,27,8, CL_SLINGER,24,0, CL_MENDER,29,0, CL_BREAKER,28,0, CL_WARDEN,7,4, CL_SLINGER,20,0, CL_MENDER,26,0},
    };
    CHECK(DG_NMISSION==12,"missions: %d",DG_NMISSION);
    for(int m=0;m<DG_NMISSION;m++){ armory_t s=a; loadout(&s,DG_MISSION[m].cap,SQ[m]); s.slots=DG_MAXU; int wins=0, beats=0; float hp=0; int sp=0;
        for(uint32_t seed=1;seed<=12;seed++){ result_t r=play(m,&s,seed,1); wins+=r.won; beats+=r.beats; hp+=r.hp; sp+=r.specials; }
        printf("mission %d %-16s smart: %d/12 won, %.1f min avg, %.0f%% hp left, %.1f specials\n",m+1,DG_MISSION[m].name,wins,beats/12*0.5/60,hp/12*100,sp/12.0);
        CHECK(wins>=8,"mission %d should be winnable with a fitting squad (%d/12)",m+1,wins);
        CHECK(beats/12*0.5/60<9,"mission %d takes too long",m+1); }
    /* strategy matters */
    { armory_t s=a; const int melee[12]={CL_WARDEN,7,3, CL_BREAKER,14,0, CL_WARDEN,5,2, CL_MENDER,16,0};
      loadout(&s,4,melee); s.slots=4; int wins=0; for(uint32_t seed=1;seed<=4;seed++) wins+=play(1,&s,seed,1).won;
      printf("iron hollow, all-melee squad: %d/4 won\n",wins); CHECK(wins==0,"no ranged unit should not clear the flyers"); }
    { int ws=0, wd=0; for(int m=2;m<4;m++){ armory_t s=a; loadout(&s,4,SQ[m]); s.slots=4;
        for(uint32_t seed=1;seed<=6;seed++){ ws+=play(m,&s,seed,1).won; wd+=play(m,&s,seed,0).won; } }
      printf("missions 3-4: smart %d/12, never-guard %d/12\n",ws,wd); CHECK(ws>wd,"guarding against bosses should pay off"); }

    /* rescue: breaking a cage frees a recruit who joins (room in the squad) */
    { armory_t s=a; loadout(&s,4,SQ[4]); static dg_t g; dg_start(&g,4,&s,3); int k=-1;
      for(int j=0;j<DG_MAXE;j++) if(g.e[j].on&&(g.e[j].traits&TR_CAGE)){ k=j; break; }
      CHECK(k>=0,"mission 5 has a cage");
      if(k>=0){ int before=dg_alive(&g); g.e[k].hp=1; g.sx=g.e[k].x-60;
          for(int i=0;i<DG_MAXU;i++) if(g.u[i].on) g.u[i].x=g.sx-10*i;
          g.e[k].burn=2; for(int i=0;i<60;i++) dg_step(&g,1/30.0f);
          CHECK(g.rescued==1 && dg_alive(&g)==before+1,"recruit joins: rescued %d, %d -> %d",g.rescued,before,dg_alive(&g)); } }
    /* blastcap: bursting at the front line hurts the squad, shot from afar it doesn't */
    { CHECK(DG_ETYPE[ET_BLASTCAP].traits&TR_EXPLODE,"blastcap"); CHECK(dg_mult(DT_PIERCE,EL_NONE,DG_ETYPE[ET_SHIELDBEARER].traits)==0.5f,"shieldbearer armour"); }
    if(fail){ printf("%d failure(s)\n",fail); return 1; }
    printf("drum_game: all tests passed\n"); return 0;
}
