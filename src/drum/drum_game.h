/* drum_game — Drumfolk battles: squads, gear, enemies, bosses, missions (pure C, host-tested:
 * tests/drum_game_test.c). The frontend tells it about beats (dg_beat: which command the squad
 * is performing on this beat) and time (dg_step); it moves, fights and reports events.
 *
 * Strategy comes from matching gear to enemies:
 *   damage type x material: armoured foes shrug off pierce (x0.5) but crack under crush (x1.5),
 *   wooden palisades burn (fire x2) and resist pierce; flying foes can only be hit by ranged
 *   classes; elements add burn (fire), slowed attacks (frost) or a chain hit (storm);
 *   shields block passively (half) and fully on GUARD; mirror shields reflect missiles,
 *   thorn shields hurt melee attackers; two-handed classes carry no shield at all.
 * Every mission ends with a boss: big, with several telegraphed attacks (named one or two beats
 * ahead: GUARD to soak, RETREAT out of range to dodge) and an enraged second phase. */
#ifndef DRUM_GAME_H
#define DRUM_GAME_H
#include <stdint.h>

#define DG_MAXU 8                 /* squad size: up to 8 (slots are won by rescues, missions cap how many deploy) */
#define DG_SLOTS0 4               /* soldier slots at the start */
#define DG_MAXE 48
#define DG_MAXP 48
enum { CL_LANCER, CL_WARDEN, CL_SLINGER, CL_MENDER, CL_BREAKER, CL_N };
enum { DT_PIERCE, DT_SLASH, DT_CRUSH };
enum { EL_NONE, EL_FIRE, EL_FROST, EL_STORM };
enum { TR_ARMORED=1, TR_FLYING=2, TR_WOODEN=4, TR_FIREPROOF=8, TR_RANGED=16, TR_STATIC=32,
       TR_FROSTBORN=64,    /* frost does nothing, fire x1.5 */
       TR_HEALER=128,      /* its "attack" heals nearby foes */
       TR_BOMB=256,        /* its missiles burst over the whole squad */
       TR_TOTEM=512,       /* while one stands, foes take x0.7 damage */
       TR_SCOUT=1024,      /* runs for the next fort when it sees the squad: if it gets there, alarm */
       TR_GATE=2048,       /* a fort's gate: blocks the way, shelters the garrison behind it */
       TR_HEAVYSHOT=4096,  /* telegraphs 4 beats ahead (ballista): GUARD in time */
       TR_EXPLODE=8192,    /* bursts when it dies or reaches the squad: kill it from afar */
       TR_DEFLECT=16384,   /* missiles glance off (x0.3): go melee */
       TR_CAGE=32768 };    /* prisoners: breaking it frees a recruit who joins the squad */
enum { SH_REFLECT=1, SH_THORNS=2, SH_HEAVY=4 };
enum { ET_GRUNT, ET_IRONBACK, ET_WINGSPITE, ET_PALISADE, ET_STONESLINGER,
       ET_BRISTLER, ET_HEXCALLER, ET_GOURDTHROWER, ET_TOTEM, ET_BULWARK, ET_FROSTLING,
       /* forts */      ET_SCOUT, ET_GATE, ET_WATCHTOWER, ET_IRONGATE, ET_BALLISTA,
       /* sub-bosses */ ET_CHIEFTAIN, ET_RUSTJAW, ET_HEXMOTHER, ET_CINDERHORN,
       /* bosses */     ET_THORNHIDE, ET_GLOOMWING, ET_GLACIERWYRM, ET_EMBERMAW,
       /* act II */     ET_BOGLURKER, ET_WISP, ET_BLASTCAP, ET_SHIELDBEARER, ET_CROSSBOW, ET_CAGE,
       /* sub-bosses */ ET_CHAINWARDEN, ET_BLASTMASTER,
       /* bosses */     ET_HYDRA, ET_COLOSSUS, ET_STORMROC, ET_HOLLOWKING, ET_N };
/* visual effects reported to the frontend (it empties fx[] each frame) */
enum { FX_HIT, FX_CRIT, FX_HURT, FX_BLOCK, FX_HEAL, FX_SPECIAL };   /* FX_SPECIAL: arg = class */
typedef struct { float x, y; int amt, kind, arg; } dg_fx_t;
#define DG_MAXFX 32
/* boss moves */
enum { MV_MELEE,     /* heavy blow on the front unit */
       MV_SWEEP,     /* hits the whole squad */
       MV_QUAKE,     /* whole squad + shoves it back */
       MV_VOLLEY,    /* n missiles at random units */
       MV_SUMMON,    /* calls a minion (arg = enemy type) */
       MV_DIVE,      /* flyer lands on the front unit and stays grounded arg beats */
       MV_CHARGE,    /* rushes the squad: front unit + shove */
       MV_ROAR,      /* enrage (once, at half health) */
       MV_HEAL };    /* mends itself and nearby foes by n % */

typedef struct { const char*name; int hp, reach, ranged, two_handed; unsigned color; const char*role; } class_t;
typedef struct { const char*name; unsigned classes; int dtype, elem, dmg, range, heal; } weapon_t;
typedef struct { const char*name; unsigned classes; int block, special; } shield_t;   /* block in % */
typedef struct { const char*name; int hp, traits, dmg, range, period; unsigned color; const char*hint; int hw, boss; } etype_t;
typedef struct { const char*name; int kind, windup, dmg, range, n, arg, enraged_only; } bmove_t;
typedef struct { const char*name; int nmv; bmove_t mv[8]; int rest, rest_enraged, elite; const char*lore; } boss_t;
typedef struct { int type; int x; } espawn_t;
typedef struct { const char*name; const char*brief; int length; int ne; espawn_t e[32]; int nreward; int reward[8];
                 int alarm[2], biome;            /* alarm: who comes running when a scout reaches a fort */
                 int cap;                        /* soldiers deployed at most */
                 int rescue, rescue_cls;         /* rescue mission: soldier slots won (first win), class of the prisoners */
                 int req;                        /* mission to clear first (-1: none) */
                 int mx, my; } mission_t;        /* place on the world map (480 x 720 screen) */
enum { BIOME_FOREST, BIOME_DESERT, BIOME_SNOW, BIOME_VOLCANO, BIOME_SWAMP, BIOME_PEAKS, BIOME_N };
/* reward entries: weapon index w as w, shield index s as 100+s */

extern const class_t  DG_CLASS[CL_N];
extern const weapon_t DG_WEAPON[];  extern const int DG_NWEAPON;
extern const shield_t DG_SHIELD[];  extern const int DG_NSHIELD;   /* [0] = no shield */
extern const etype_t  DG_ETYPE[ET_N];
extern const boss_t   DG_BOSS[];    extern const int DG_NBOSS;
extern const mission_t DG_MISSION[]; extern const int DG_NMISSION;
extern const char *const DG_DTYPE_NAME[3];
extern const char *const DG_ELEM_NAME[4];

typedef struct { int cls, weapon, shield; } loadout_t;
/* what the player owns (persisted by the frontend). done/rescued: bit per mission. The squad is a
 * roster of up to `slots` soldiers; a mission deploys the first dg_deploy() of them. */
typedef struct { uint32_t weapons, shields, done, rescued; int slots; loadout_t squad[DG_MAXU]; int nsquad; } armory_t;
void dg_armory_default(armory_t*a);
int  dg_mission_open(const armory_t*a,int m);  /* its required mission is cleared */
int  dg_deploy(const armory_t*a,int m);        /* soldiers that march on mission m */
int  dg_starter_weapon(int cls);
int  dg_weapon_ok(int cls,int w);            /* class may wield it */
int  dg_shield_ok(int cls,int s);
float dg_mult(int dtype,int elem,int traits); /* damage multiplier vs a foe's traits */

typedef struct { int on, cls, weapon, shield; float hp, maxhp, x, flash, swing; int rallied; float off, kb; } unit_t;   /* off: combat step from the formation spot; kb: knocked back by a blow */
typedef struct { int on, type, traits; float x, y, hp, maxhp, burn, flash, bob;
                 int frost, next, windup;              /* beats: countdown, telegraph */
                 int move, enraged, grounded, summoned, spawns; /* bosses: move telegraphed (-1), phase 2, dive; minion; summons made */
                 int fleeing; float fled;              /* scouts */
                 float act;                            /* animation: 1 when a blow lands, decays */
                 float home, kb, walk; } enemy_t;      /* spawn x (bosses keep near it), knockback still to apply, distance walked (legs) */
typedef struct { int on, foe, target, kind, dtype, elem; float t, dur, x0, y0, dmg; } proj_t;   /* foe: thrown by an enemy */

typedef struct {
    const mission_t*m; int mission;
    unit_t u[DG_MAXU]; enemy_t e[DG_MAXE]; proj_t p[DG_MAXP];
    float sx, vx, move_left;      /* squad front x, march speed, distance still to cover */
    int cmd, cmd_beat, fever;     /* command being performed (CMD_* from drum_core) and its beat 0..3 */
    int64_t beat; int guard_until;/* guard holds through this beat */
    int won, lost; float time; uint32_t rng;
    float beat_s;                 /* seconds per beat (0.5 at 120 BPM; songs set their own) */
    /* events since the frontend last cleared them (sounds, effects) */
    int ev_attack, ev_hit, ev_kill, ev_hurt, ev_block, ev_heal, ev_windup, ev_drop;   /* ev_drop: reward code+1 */
    int ev_boss_move;             /* a boss started telegraphing move index+1 */
    int ev_boss_land, ev_quake, ev_roar, ev_summon, ev_boss_down;
    int ev_scout_flee, ev_alarm, ev_gate_down, ev_special;
    int ev_rescue, ev_blast;      /* a cage broken (a recruit joined when there was room), a blastcap burst */
    int rescued;                  /* cages broken this battle */
    dg_fx_t fx[DG_MAXFX]; int nfx;
} dg_t;

void dg_start(dg_t*g,int mission,const armory_t*a,uint32_t seed);
void dg_beat(dg_t*g,int64_t beat,int cmd,int beat_in,int fever);   /* cmd: CMD_NONE when idle; fever 2 = super fever */
void dg_step(dg_t*g,float dt);
int  dg_alive(const dg_t*g);
int  dg_enemies_left(const dg_t*g);
int  dg_boss(const dg_t*g);                 /* index of the living boss in e[], -1 */
const bmove_t* dg_boss_move(const dg_t*g,int k);   /* move being telegraphed by e[k], NULL */
float dg_front(const enemy_t*e);            /* x of a foe's front edge */
float dg_unit_y(const dg_t*g,int i);        /* for drawing: ground line = 0 */
#endif
