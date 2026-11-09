/* drum_game.c — see drum_game.h */
#include "drum_game.h"
#include "drum_core.h"
#include <string.h>
#include <math.h>

#define L (1u<<CL_LANCER)
#define W (1u<<CL_WARDEN)
#define S (1u<<CL_SLINGER)
#define M (1u<<CL_MENDER)
#define B (1u<<CL_BREAKER)

const class_t DG_CLASS[CL_N]={
    /* name      hp reach ranged 2h  color     role */
    {"LANCER",   45,  0,  1,  0, 0xE8731C, "THROWN SPEARS, MID RANGE, SHIELD"},
    {"WARDEN",   85, 60,  0,  0, 0x5A86B8, "TANK, COVERS THE SQUAD ON GUARD"},
    {"SLINGER",  30,  0,  1,  1, 0x6FA84A, "LONG RANGE, HITS FLYERS, NO SHIELD"},
    {"MENDER",   35, 50,  0,  1, 0xE8DCC0, "HEALS ON RALLY AND GUARD"},
    {"BREAKER",  65, 75,  0,  1, 0xC8443A, "HEAVY MELEE, CHARGES IN FEVER"},
};
const char *const DG_DTYPE_NAME[3]={"PIERCE","SLASH","CRUSH"};
const char *const DG_ELEM_NAME[4]={"","FIRE","FROST","STORM"};

const weapon_t DG_WEAPON[]={
    /*  name             classes dtype      elem      dmg range heal */
    {"ASH JAVELIN",      L, DT_PIERCE, EL_NONE,   9, 230, 0},   /* 0  start */
    {"BRONZE JAVELIN",   L, DT_PIERCE, EL_NONE,  13, 240, 0},   /* 1  m1 */
    {"EMBER JAVELIN",    L, DT_PIERCE, EL_FIRE,  10, 230, 0},   /* 2  m1 */
    {"RIME JAVELIN",     L, DT_PIERCE, EL_FROST, 10, 230, 0},   /* 3  m2 */
    {"THUNDER JAVELIN",  L, DT_PIERCE, EL_STORM, 11, 240, 0},   /* 4  m3 */
    {"OAK CLUB",         W, DT_CRUSH,  EL_NONE,   9,  60, 0},   /* 5  start */
    {"SHORT SWORD",      W, DT_SLASH,  EL_NONE,  10,  60, 0},   /* 6  start */
    {"IRON MACE",        W, DT_CRUSH,  EL_NONE,  14,  60, 0},   /* 7  m1 */
    {"FLAME BRAND",      W, DT_SLASH,  EL_FIRE,  12,  60, 0},   /* 8  m2 */
    {"HUNTING BOW",      S, DT_PIERCE, EL_NONE,   7, 330, 0},   /* 9  start */
    {"STONE SLING",      S, DT_CRUSH,  EL_NONE,   7, 290, 0},   /* 10 start */
    {"LONGBOW",          S, DT_PIERCE, EL_NONE,  10, 410, 0},   /* 11 m2 */
    {"FROST BOW",        S, DT_PIERCE, EL_FROST,  8, 340, 0},   /* 12 m2 */
    {"WOOD AXE",         B, DT_SLASH,  EL_NONE,  17,  75, 0},   /* 13 start */
    {"WAR HAMMER",       B, DT_CRUSH,  EL_NONE,  20,  70, 0},   /* 14 m2 */
    {"STORM MAUL",       B, DT_CRUSH,  EL_STORM, 17,  75, 0},   /* 15 m3 */
    {"TWIG STAFF",       M, DT_CRUSH,  EL_NONE,   4,  50, 7},   /* 16 start */
    {"REED FLUTE",       M, DT_CRUSH,  EL_NONE,   3,  50, 10},  /* 17 m2 */
    {"SUN STAFF",        M, DT_CRUSH,  EL_FIRE,   6,  50, 12},  /* 18 m4 */
    {"GLACIER PIKE",     L, DT_PIERCE, EL_FROST, 12, 240, 0},   /* 19 m3 */
    {"SKY BOW",          S, DT_PIERCE, EL_STORM,  9, 380, 0},   /* 20 m3 */
    {"MAGMA HAMMER",     B, DT_CRUSH,  EL_FIRE,  19,  70, 0},   /* 21 m2 */
};
const int DG_NWEAPON=(int)(sizeof DG_WEAPON/sizeof DG_WEAPON[0]);
const shield_t DG_SHIELD[]={
    {"NO SHIELD",     L|W|S|M|B,  0, 0},          /* 0 */
    {"WICKER SHIELD", L|W,       15, 0},          /* 1 start */
    {"HIDE SHIELD",   L|W,       25, 0},          /* 2 start */
    {"BRONZE SHIELD", L|W,       35, 0},          /* 3 m1 */
    {"TOWER SHIELD",  W,         50, SH_HEAVY},   /* 4 m4: march slower */
    {"MIRROR SHIELD", L|W,       25, SH_REFLECT}, /* 5 m2: guard reflects missiles */
    {"THORN SHIELD",  L|W,       20, SH_THORNS},  /* 6 m3: melee attackers get hurt */
};
const int DG_NSHIELD=(int)(sizeof DG_SHIELD/sizeof DG_SHIELD[0]);

const etype_t DG_ETYPE[ET_N]={
    /* name            hp  traits                          dmg range per color     hint                                   hw boss */
    {"GRUNT",          60, 0,                               10,  70, 4, 0x8A5A3A, "PLAIN BRUTE",                          0, -1},
    {"IRONBACK",      110, TR_ARMORED,                      16,  70, 5, 0x7A8088, "ARMOURED: CRUSH X1.5, PIERCE X0.5",    0, -1},
    {"WINGSPITE",      35, TR_FLYING|TR_RANGED,              7, 260, 3, 0x8E5AB8, "FLYING: ONLY RANGED CAN HIT IT",       0, -1},
    {"PALISADE",      120, TR_WOODEN|TR_STATIC,              0,   0, 0, 0x9A7040, "WOOD: FIRE X2, PIERCE X0.5, BLOCKS",   0, -1},
    {"STONESLINGER",   45, TR_RANGED,                        9, 300, 4, 0xA88A5A, "ROCKS: GUARD, MIRRORS REFLECT",        0, -1},
    {"BRISTLER",       30, 0,                                5,  60, 2, 0xB8703A, "FAST: HITS EVERY OTHER BEAT",          0, -1},
    {"HEXCALLER",      50, TR_HEALER,                        0, 380, 4, 0x6A4A9A, "HEALS ITS ALLIES: KILL IT FIRST",      0, -1},
    {"GOURDTHROWER",   40, TR_RANGED|TR_BOMB,                6, 280, 5, 0x9AA03A, "BOMBS HIT THE WHOLE SQUAD",            0, -1},
    {"SPIRIT TOTEM",   90, TR_WOODEN|TR_STATIC|TR_TOTEM,     0,   0, 0, 0x4A8A7A, "FOES TAKE X0.7 WHILE IT STANDS",       0, -1},
    {"BULWARK",       170, TR_ARMORED,                      12,  70, 5, 0x5A6470, "HEAVY ARMOUR: BRING CRUSH",            0, -1},
    {"FROSTLING",      55, TR_FROSTBORN,                    11,  70, 4, 0x8AC8E8, "FROSTBORN: FIRE X1.5, FROST NOTHING",  0, -1},
    {"SCOUT",          25, TR_SCOUT,                         4,  60, 3, 0xB09060, "RUNS TO RAISE THE ALARM: STOP IT",     0, -1},
    {"WOODEN GATE",   170, TR_WOODEN|TR_STATIC|TR_GATE,      0,   0, 0, 0x8A6030, "BLOCKS THE WAY: BURN OR CRUSH IT",    10, -1},
    {"WATCHTOWER",    140, TR_WOODEN|TR_STATIC|TR_RANGED,    7, 330, 4, 0x7A5A34, "ARCHER TOWER: WOOD, SHOOTS FAR",      16, -1},
    {"IRON GATE",     260, TR_ARMORED|TR_STATIC|TR_GATE,     0,   0, 0, 0x5A6068, "FORTRESS GATE: ONLY CRUSH DENTS IT",  14, -1},
    {"BALLISTA",      150, TR_ARMORED|TR_STATIC|TR_RANGED|TR_HEAVYSHOT, 14, 480, 12, 0x6A6A70, "HEAVY BOLT: GUARD ON THE SIGNAL", 18, -1},
    {"GRUNT CHIEFTAIN",190, 0,                              18,  90, 3, 0xA04A2A, "SUB-BOSS: CALLS GRUNTS, SLAMS",       30,  0},
    {"RUSTJAW",       250, TR_ARMORED,                      22,  90, 3, 0x8A5A3A, "SUB-BOSS: ARMOURED, RAMS",            40,  1},
    {"HEX MOTHER",    210, 0,                               10, 350, 3, 0x7A3A9A, "SUB-BOSS: HEALS, CALLS WINGS",        30,  2},
    {"CINDERHORN",    270, TR_FIREPROOF,                    24, 100, 3, 0xC0502A, "SUB-BOSS: FIREPROOF, STAMPEDES",      40,  3},
    {"THORNHIDE",     340, 0,                               26, 140, 2, 0x7A4A2A, "BOSS: GORES, STOMPS, THORN VOLLEYS", 110,  4},
    {"GLOOMWING",     460, TR_FLYING,                       20, 400, 2, 0x4A3A6A, "BOSS: FLIES; LANDS AFTER A DIVE",    100,  5},
    {"GLACIER WYRM",  580, TR_FROSTBORN,                    28, 150, 2, 0x9AD0F0, "BOSS: FROSTBORN, ICE BREATH",        130,  6},
    {"EMBERMAW",      660, TR_FIREPROOF,                    30, 150, 2, 0xD04A20, "BOSS: FIREPROOF, INFERNO",           130,  7},
};

const boss_t DG_BOSS[]={
    {"GRUNT CHIEFTAIN", 3, {
        {"CLEAVE",       MV_MELEE,  1, 18,  90, 0, 0, 0},
        {"WAR CRY",      MV_SUMMON, 1,  0, 600, 0, ET_GRUNT, 0},
        {"GROUND SLAM",  MV_QUAKE,  2,  8, 130, 40, 0, 0}}, 3, 2, 1, "LEADS THE BRUTES"},
    {"RUSTJAW", 3, {
        {"CRUNCH",       MV_MELEE,  1, 22,  90, 0, 0, 0},
        {"SHELL SPIN",   MV_SWEEP,  2, 10, 120, 0, 0, 0},
        {"RAM",          MV_CHARGE, 2, 16, 240, 50, 0, 0}}, 3, 2, 1, "AN OLD IRONBACK"},
    {"HEX MOTHER", 4, {
        {"HEX BOLTS",    MV_VOLLEY, 1,  7, 360, 3, 0, 0},
        {"CALL WINGS",   MV_SUMMON, 1,  0, 600, 0, ET_WINGSPITE, 0},
        {"DARK MEND",    MV_HEAL,   1,  0, 600, 12, 0, 0},
        {"CURSE WAVE",   MV_SWEEP,  2,  9, 260, 0, 0, 0}}, 3, 2, 1, "QUEEN OF THE MARSH"},
    {"CINDERHORN", 3, {
        {"HORN GORE",    MV_MELEE,  1, 24, 100, 0, 0, 0},
        {"EMBER VOLLEY", MV_VOLLEY, 1,  8, 330, 3, 0, 0},
        {"STAMPEDE",     MV_CHARGE, 2, 18, 260, 60, 0, 0}}, 3, 2, 1, "HERALD OF THE DEN"},
    {"THORNHIDE", 5, {
        {"GORE",         MV_MELEE,  1, 26, 140, 0, 0, 0},
        {"STOMP",        MV_QUAKE,  2, 12, 220, 60, 0, 0},
        {"BRISTLE VOLLEY",MV_VOLLEY,2,  7, 430, 5, 0, 0},
        {"ROAR",         MV_ROAR,   1,  0, 999, 0, 0, 0},
        {"RAMPAGE",      MV_CHARGE, 2, 22, 340, 80, 0, 1}}, 3, 2, 0, "TITAN BOAR OF THE RIDGE"},
    {"GLOOMWING", 6, {
        {"DIVE",         MV_DIVE,   2, 20, 420, 0, 4, 0},
        {"GALE",         MV_QUAKE,  2,  8, 420, 70, 0, 0},
        {"SPORE RAIN",   MV_VOLLEY, 2,  6, 430, 6, 0, 0},
        {"CALL THE BROOD",MV_SUMMON,1,  0, 650, 0, ET_WINGSPITE, 0},
        {"SHRIEK",       MV_ROAR,   1,  0, 999, 0, 0, 0},
        {"DIVE",         MV_DIVE,   1, 24, 420, 0, 4, 1}}, 3, 2, 0, "MOTH-RAPTOR OF THE HOLLOW"},
    {"GLACIER WYRM", 7, {
        {"FROST BITE",   MV_MELEE,  1, 28, 160, 0, 0, 0},
        {"ICE BREATH",   MV_SWEEP,  2, 16, 320, 0, 0, 0},
        {"ICICLE RAIN",  MV_VOLLEY, 2,  8, 460, 5, 0, 0},
        {"TAIL CRUSH",   MV_QUAKE,  2, 12, 230, 60, 0, 0},
        {"CALL FROSTLINGS",MV_SUMMON,1, 0, 650, 0, ET_FROSTLING, 0},
        {"ROAR",         MV_ROAR,   1,  0, 999, 0, 0, 0},
        {"AVALANCHE",    MV_SWEEP,  2, 26, 520, 0, 0, 1}}, 3, 2, 0, "SLEEPER OF THE PASS"},
    {"EMBERMAW", 8, {
        {"BITE",         MV_MELEE,  1, 30, 160, 0, 0, 0},
        {"FLAME BREATH", MV_SWEEP,  2, 18, 320, 0, 0, 0},
        {"TAIL QUAKE",   MV_QUAKE,  2, 12, 230, 60, 0, 0},
        {"EMBER SPIT",   MV_VOLLEY, 1,  9, 440, 4, 0, 0},
        {"CALL THE PACK",MV_SUMMON, 1,  0, 650, 0, ET_GRUNT, 0},
        {"ROAR",         MV_ROAR,   1,  0, 999, 0, 0, 0},
        {"INFERNO",      MV_SWEEP,  2, 28, 620, 0, 0, 1},
        {"MAGMA CHARGE", MV_CHARGE, 2, 24, 360, 80, 0, 1}}, 3, 2, 0, "THE FIREPROOF BEAST"},
};
const int DG_NBOSS=(int)(sizeof DG_BOSS/sizeof DG_BOSS[0]);

const mission_t DG_MISSION[]={
    /* scouts -> open ground -> outpost -> march -> camp / fortress -> march -> boss */
    {"THORNWOOD", "SCOUTS, AN OUTPOST, THE TITAN BOAR", 4000, 17,
        {{ET_SCOUT,450},{ET_SCOUT,520},{ET_GRUNT,760},{ET_BRISTLER,860},
         {ET_WATCHTOWER,1240},{ET_GATE,1300},{ET_GRUNT,1360},{ET_STONESLINGER,1420},
         {ET_BRISTLER,1800},{ET_GRUNT,1880},
         {ET_PALISADE,2300},{ET_WATCHTOWER,2340},{ET_GRUNT,2400},{ET_CHIEFTAIN,2520},
         {ET_STONESLINGER,2900},{ET_GRUNT,3000},{ET_THORNHIDE,3600}},
        4, {1, 7, 100+3, 2}, {ET_GRUNT,ET_BRISTLER}, BIOME_FOREST},
    {"SUNSCAR DUNES", "BEETLES, A DESERT FORTRESS, WINGS", 4400, 19,
        {{ET_SCOUT,450},{ET_WINGSPITE,700},{ET_IRONBACK,850},
         {ET_WATCHTOWER,1150},{ET_GATE,1200},{ET_GOURDTHROWER,1280},{ET_TOTEM,1330},
         {ET_SCOUT,1650},{ET_WINGSPITE,1800},{ET_IRONBACK,1950},
         {ET_BALLISTA,2350},{ET_IRONGATE,2450},{ET_BULWARK,2560},{ET_STONESLINGER,2620},{ET_RUSTJAW,3000},   /* first fortress: one ballista */
         {ET_WINGSPITE,3250},{ET_GOURDTHROWER,3350},{ET_IRONBACK,3450},{ET_GLOOMWING,3950}},
        7, {11, 14, 100+5, 12, 3, 17, 8}, {ET_BRISTLER,ET_GRUNT}, BIOME_DESERT},
    {"RIME PASS", "FROSTBORN, THE HEX MOTHER'S HOLD", 4600, 20,
        {{ET_SCOUT,450},{ET_SCOUT,500},{ET_FROSTLING,750},{ET_HEXCALLER,900},
         {ET_WATCHTOWER,1250},{ET_GATE,1300},{ET_FROSTLING,1360},{ET_WINGSPITE,1400},
         {ET_BRISTLER,1750},{ET_FROSTLING,1850},{ET_TOTEM,1950},
         {ET_BALLISTA,2400},{ET_IRONGATE,2500},{ET_BULWARK,2580},{ET_BALLISTA,2600},{ET_HEXCALLER,2660},{ET_HEXMOTHER,3050},
         {ET_FROSTLING,3400},{ET_WINGSPITE,3500},{ET_GLACIERWYRM,4050}},
        6, {4, 15, 100+6, 19, 20, 21}, {ET_FROSTLING,ET_FROSTLING}, BIOME_SNOW},
    {"EMBERMAW'S DEN", "THE BLACK FORTRESS, THE BEAST", 4800, 21,
        {{ET_SCOUT,450},{ET_GOURDTHROWER,700},{ET_GRUNT,800},
         {ET_WATCHTOWER,1150},{ET_GATE,1200},{ET_IRONBACK,1260},{ET_HEXCALLER,1320},
         {ET_SCOUT,1650},{ET_WINGSPITE,1750},{ET_TOTEM,1850},
         {ET_BALLISTA,2300},{ET_WATCHTOWER,2340},{ET_IRONGATE,2450},{ET_BALLISTA,2520},{ET_BULWARK,2560},{ET_STONESLINGER,2620},
         {ET_CINDERHORN,3100},
         {ET_GRUNT,3450},{ET_WINGSPITE,3550},{ET_GOURDTHROWER,3650},{ET_EMBERMAW,4250}},
        2, {100+4, 18}, {ET_GRUNT,ET_BRISTLER}, BIOME_VOLCANO},
};
const int DG_NMISSION=(int)(sizeof DG_MISSION/sizeof DG_MISSION[0]);

void dg_armory_default(armory_t*a){
    memset(a,0,sizeof *a);
    const int w[]={0,5,6,9,10,13,16}; for(unsigned i=0;i<sizeof w/sizeof w[0];i++) a->weapons|=1u<<w[i];
    a->shields=(1u<<0)|(1u<<1)|(1u<<2);
    a->nsquad=4;
    a->squad[0]=(loadout_t){CL_WARDEN,5,2};
    a->squad[1]=(loadout_t){CL_LANCER,0,1};
    a->squad[2]=(loadout_t){CL_SLINGER,9,0};
    a->squad[3]=(loadout_t){CL_MENDER,16,0};
}
int dg_weapon_ok(int cls,int w){ return w>=0 && w<DG_NWEAPON && (DG_WEAPON[w].classes>>cls&1); }
int dg_shield_ok(int cls,int s){ if(s<0||s>=DG_NSHIELD) return 0; if(DG_CLASS[cls].two_handed) return s==0; return (DG_SHIELD[s].classes>>cls&1)!=0; }
float dg_mult(int dtype,int elem,int traits){
    float m=1;
    if(traits&TR_ARMORED) m*= dtype==DT_PIERCE?0.5f : dtype==DT_SLASH?0.75f : 1.5f;
    if(traits&TR_WOODEN)  m*= dtype==DT_PIERCE?0.5f : dtype==DT_SLASH?0.9f  : 1.2f;
    if(elem==EL_FIRE){ if(traits&TR_WOODEN) m*=2; if(traits&TR_FIREPROOF) m*=0.5f; if(traits&TR_FROSTBORN) m*=1.5f; }
    return m;
}
float dg_front(const enemy_t*e){ return e->x-(float)DG_ETYPE[e->type].hw; }
int dg_boss(const dg_t*g){
    int best=-1;
    for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0||DG_ETYPE[e->type].boss<0) continue;
        if(best<0 || fabsf(e->x-g->sx)<fabsf(g->e[best].x-g->sx)) best=k; }
    return best;
}
const bmove_t* dg_boss_move(const dg_t*g,int k){
    const enemy_t*e=&g->e[k]; int b=DG_ETYPE[e->type].boss;
    return (b>=0 && e->move>=0) ? &DG_BOSS[b].mv[e->move] : 0;
}

static uint32_t rnd(dg_t*g){ g->rng=g->rng*1664525u+1013904223u; return g->rng>>8; }
static const int RANK[CL_N]={2,0,4,3,1};      /* front to back: warden, breaker, lancer, mender, slinger */
#define SPACING 36.0f
#define BEAT_S (g->beat_s)                     /* the song's tempo; 0.5 s at 120 BPM */
#define FOE_DMG 0.5f                          /* tuning knobs (tests/drum_game_test.c) */
#define BOSS_HP 0.75f
#define BOSS_DMG 0.75f
#define HEAL 1.5f

static void formation(dg_t*g,float*tx){
    int order[DG_MAXU],n=0;
    for(int i=0;i<DG_MAXU;i++) if(g->u[i].on && g->u[i].hp>0) order[n++]=i;
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(RANK[g->u[order[j]].cls]<RANK[g->u[order[i]].cls]){ int t=order[i]; order[i]=order[j]; order[j]=t; }
    for(int k=0;k<n;k++) tx[order[k]]=g->sx-SPACING*k+g->u[order[k]].off;
}
static void enemy_init(dg_t*g,int i,int type,float x){
    enemy_t*e=&g->e[i]; const etype_t*t=&DG_ETYPE[type];
    memset(e,0,sizeof *e);
    e->on=1; e->type=type; e->traits=t->traits; e->x=x; e->maxhp=e->hp=(float)t->hp*(t->boss>=0?BOSS_HP:1); e->move=-1;
    e->next=t->period? t->period+(i%t->period) : 0;
    e->y=(t->traits&TR_FLYING) ? (t->boss>=0?-150.0f:-120.0f) : 0.0f; e->bob=(float)i; e->home=x;
}
void dg_start(dg_t*g,int mission,const armory_t*a,uint32_t seed){
    memset(g,0,sizeof *g);
    g->mission=mission; g->m=&DG_MISSION[mission]; g->rng=seed?seed:1; g->cmd=CMD_NONE; g->guard_until=-1000;
    g->sx=160; g->beat_s=0.5f;
    for(int i=0;i<a->nsquad && i<DG_MAXU;i++){ unit_t*u=&g->u[i]; const loadout_t*l=&a->squad[i];
        u->on=1; u->cls=l->cls; u->weapon=dg_weapon_ok(l->cls,l->weapon)?l->weapon:-1;
        u->shield=dg_shield_ok(l->cls,l->shield)?l->shield:0;
        u->maxhp=u->hp=(float)DG_CLASS[l->cls].hp; }
    float tx[DG_MAXU]={0}; formation(g,tx); for(int i=0;i<DG_MAXU;i++) g->u[i].x=tx[i];
    for(int i=0;i<g->m->ne;i++) enemy_init(g,i,g->m->e[i].type,(float)g->m->e[i].x);
}
int dg_alive(const dg_t*g){ int n=0; for(int i=0;i<DG_MAXU;i++) if(g->u[i].on && g->u[i].hp>0) n++; return n; }
int dg_enemies_left(const dg_t*g){ int n=0; for(int i=0;i<DG_MAXE;i++) if(g->e[i].on && g->e[i].hp>0) n++; return n; }
float dg_unit_y(const dg_t*g,int i){
    const unit_t*u=&g->u[i]; float y=0;
    if(g->move_left!=0) y=-fabsf(sinf(g->time*12.566f+(float)i))*6;   /* trot */
    if(u->swing>0) y-=u->swing*7;
    return y;
}

/* ---- damage ---- */
static void drop(dg_t*g){
    if(rnd(g)%100>=30) return;
    const mission_t*m=g->m; g->ev_drop=m->reward[rnd(g)%m->nreward]+1;
}
static int totem_up(const dg_t*g){ for(int k=0;k<DG_MAXE;k++) if(g->e[k].on&&g->e[k].hp>0&&(g->e[k].traits&TR_TOTEM)) return 1; return 0; }
static void fx(dg_t*g,float x,float y,float amt,int kind,int arg){
    if(g->nfx<DG_MAXFX) g->fx[g->nfx++]=(dg_fx_t){x,y,(int)(amt+0.5f),kind,arg}; }
static float sup(const dg_t*g){ return g->fever>=2?1.4f : g->fever?1.25f : 1; }   /* fever / super fever */
static void enemy_hurt(dg_t*g,int k,float dmg,int elem,int chain,int crit){
    enemy_t*e=&g->e[k]; if(!e->on || e->hp<=0) return;
    if(!(e->traits&TR_TOTEM) && totem_up(g)) dmg*=0.7f;
    e->hp-=dmg; e->flash=0.25f; g->ev_hit++;
    fx(g,dg_front(e)+8,e->y-(DG_ETYPE[e->type].boss>=0?70:30),dmg,crit?FX_CRIT:FX_HIT,elem);
    if(!(e->traits&(TR_STATIC|TR_GATE))){ float p = DG_ETYPE[e->type].boss>=0 ? fminf(dmg*0.25f,10) : fminf(dmg*1.4f,40); e->kb+=p; }   /* knocked back */
    if(elem==EL_FIRE && !(e->traits&TR_FIREPROOF)) e->burn=3.0f;
    if(elem==EL_FROST && !(e->traits&TR_FROSTBORN)) e->frost=2;
    if(elem==EL_STORM && chain){ int best=-1; float bd=140;
        for(int j=0;j<DG_MAXE;j++){ enemy_t*o=&g->e[j]; if(j==k||!o->on||o->hp<=0) continue; float d=fabsf(o->x-e->x); if(d<bd){ bd=d; best=j; } }
        if(best>=0) enemy_hurt(g,best,dmg*0.5f,EL_NONE,0,0); }
    if(e->hp<=0){ e->hp=0; g->ev_kill++; if(DG_ETYPE[e->type].boss>=0) g->ev_boss_down++; if(e->traits&TR_GATE) g->ev_gate_down++;
        if(!e->summoned && !(e->traits&(TR_GATE|TR_STATIC))) drop(g); }
}
static int guarding(const dg_t*g){ return g->beat<=g->guard_until; }
static int warden_up(const dg_t*g){ for(int i=0;i<DG_MAXU;i++) if(g->u[i].on && g->u[i].hp>0 && g->u[i].cls==CL_WARDEN) return 1; return 0; }
static float unit_hurt(dg_t*g,int i,float dmg){
    unit_t*u=&g->u[i]; if(!u->on || u->hp<=0) return 0;
    float blk=DG_SHIELD[u->shield].block/100.0f, b;
    if(guarding(g)){ b=blk*2+0.15f; if(u->cls!=CL_WARDEN && warden_up(g)) b+=0.2f; if(b>0.85f) b=0.85f; }
    else b=blk*0.5f;
    float took=dmg*(1-b); u->hp-=took;   /* dmg already scaled by the caller */ u->flash=0.3f; g->ev_hurt++; if(b>=0.5f) g->ev_block++;
    u->kb+=fminf(took*2.5f,60); if(u->kb>80) u->kb=80;             /* every blow pushes: a little, or a lot */
    fx(g,u->x,-30,took,b>=0.5f?FX_BLOCK:FX_HURT,0);
    if(u->hp<0) u->hp=0;
    return took;
}
static int front_unit(const dg_t*g){ int best=-1; float bx=-1e9f;
    for(int i=0;i<DG_MAXU;i++) if(g->u[i].on && g->u[i].hp>0 && g->u[i].x>bx){ bx=g->u[i].x; best=i; } return best; }
static int random_unit(dg_t*g){ int c[DG_MAXU],n=0; for(int i=0;i<DG_MAXU;i++) if(g->u[i].on && g->u[i].hp>0) c[n++]=i; return n? c[rnd(g)%n] : -1; }
static void shoot(dg_t*g,int foe,int target,float x0,float y0,float dmg,int dtype,int elem,int kind,float dist){
    for(int k=0;k<DG_MAXP;k++) if(!g->p[k].on){ proj_t*p=&g->p[k];
        p->on=1; p->foe=foe; p->target=target; p->x0=x0; p->y0=y0; p->dmg=dmg; p->dtype=dtype; p->elem=elem; p->kind=kind;
        p->t=0; p->dur=0.18f+fabsf(dist)/1400.0f; return; }
}
static void shove(dg_t*g,float d){ g->sx-=d; if(g->sx<120) g->sx=120; g->move_left=0; g->ev_quake++; }

/* ---- the squad's actions on a response beat ---- */
static int target_for(const dg_t*g,const unit_t*u){
    const weapon_t*w=&DG_WEAPON[u->weapon]; int ranged=DG_CLASS[u->cls].ranged, best=-1; float bd=1e9f;
    for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0) continue;
        if((e->traits&TR_FLYING) && !ranged) continue;
        float dx=dg_front(e)-u->x; if(dx<-30-2*DG_ETYPE[e->type].hw || dx>w->range) continue;
        /* healers and totems first when in reach: that's what a sensible squad does */
        float pri = dx - ((e->traits&(TR_HEALER|TR_TOTEM))?400:0) - (e->fleeing?600:0);
        if(pri<bd){ bd=pri; best=k; } }
    return best;
}
/* Engaging: at the start of a STRIKE, melee steps from its formation spot to striking distance of
 * the nearest ground foe (never through it), ranged units take ~70% of their weapon's range (back a
 * little when a foe is close, never past the melee line). They regroup when the fight is over. */
static void engage(dg_t*g){
    float tx[DG_MAXU]={0}; float save[DG_MAXU];
    for(int i=0;i<DG_MAXU;i++){ save[i]=g->u[i].off; g->u[i].off=0; }
    formation(g,tx);                                       /* plain formation spots */
    float melee_front=-1e9f;
    for(int i=0;i<DG_MAXU;i++){ unit_t*u=&g->u[i]; if(!u->on||u->hp<=0||u->weapon<0||DG_CLASS[u->cls].ranged||u->cls==CL_MENDER) continue;   /* the healer stays back */
        int best=-1; float bd=1e9f;
        for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0||(e->traits&TR_FLYING)) continue;
            float d=dg_front(e)-tx[i]; if(d>-40 && d<220 && d<bd){ bd=d; best=k; } }
        float off=0;
        if(best>=0){ float reach=(float)DG_WEAPON[u->weapon].range, want=dg_front(&g->e[best])-reach*0.6f;
            off=want-tx[i]; if(off<0) off=0; if(off>100) off=100;
            float lim=dg_front(&g->e[best])-18-tx[i]; if(off>lim) off=lim>0?lim:0; }
        u->off=off; if(tx[i]+off>melee_front) melee_front=tx[i]+off; }
    for(int i=0;i<DG_MAXU;i++){ unit_t*u=&g->u[i]; if(!u->on||u->hp<=0||u->weapon<0||!DG_CLASS[u->cls].ranged) continue;
        int best=-1; float bd=1e9f;
        for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0) continue;
            float d=dg_front(e)-tx[i]; if(d>-40 && d<bd){ bd=d; best=k; } }
        float off=0;
        if(best>=0 && bd<DG_WEAPON[u->weapon].range*1.3f){ float ideal=DG_WEAPON[u->weapon].range*0.7f;
            off=bd-ideal; if(off<-60) off=-60; if(off>40) off=40;
            if(melee_front>-1e8f && tx[i]+off>melee_front-24) off=melee_front-24-tx[i]; }
        u->off=off; }
    (void)save;
}
static void strike(dg_t*g){
    for(int i=0;i<DG_MAXU;i++){ unit_t*u=&g->u[i]; if(!u->on||u->hp<=0||u->weapon<0) continue;
        int k=target_for(g,u); if(k<0) continue;
        const weapon_t*w=&DG_WEAPON[u->weapon]; enemy_t*e=&g->e[k];
        float m=dg_mult(w->dtype,w->elem,e->traits), dmg=w->dmg*m*sup(g)*(u->rallied?1.35f:1);
        u->swing=1; g->ev_attack++;
        if(DG_CLASS[u->cls].ranged) shoot(g,0,k,u->x,-34,dmg,w->dtype,w->elem,u->cls==CL_SLINGER?(w->dtype==DT_CRUSH?2:1):0,dg_front(e)-u->x);
        else enemy_hurt(g,k,dmg,w->elem,1,m>=1.4f);
    }
}
static void heal_all(dg_t*g,float scale){
    for(int i=0;i<DG_MAXU;i++){ unit_t*m=&g->u[i]; if(!m->on||m->hp<=0||m->cls!=CL_MENDER||m->weapon<0) continue;
        float h=DG_WEAPON[m->weapon].heal*scale*HEAL*(g->fever?1.3f:1);
        for(int j=0;j<DG_MAXU;j++){ unit_t*u=&g->u[j]; if(!u->on||u->hp<=0) continue; float before=u->hp; u->hp+=h; if(u->hp>u->maxhp) u->hp=u->maxhp;
            if(u->hp-before>=1) fx(g,u->x,0,u->hp-before,FX_HEAL,0); }
        g->ev_heal++; m->swing=1; }
}
static float block_limit(const dg_t*g){       /* the squad can't walk through a standing ground foe */
    float lim=1e9f;
    for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(!e->on||e->hp<=0||(e->traits&(TR_FLYING|TR_SCOUT))) continue;
        float f=dg_front(e); if(e->x>g->sx-10 && f-46<lim) lim=f-46; }
    return lim;
}
static int heavy(const dg_t*g){ for(int i=0;i<DG_MAXU;i++) if(g->u[i].on&&g->u[i].hp>0&&(DG_SHIELD[g->u[i].shield].special&SH_HEAVY)) return 1; return 0; }
static int has_class(const dg_t*g,int c){ for(int i=0;i<DG_MAXU;i++) if(g->u[i].on&&g->u[i].hp>0&&g->u[i].cls==c) return 1; return 0; }

/* ---- foes ---- */
static void mend_foes(dg_t*g,float x,float pct,float cap){
    for(int j=0;j<DG_MAXE;j++){ enemy_t*o=&g->e[j]; if(!o->on||o->hp<=0||fabsf(o->x-x)>400) continue;
        float h=o->maxhp*pct; if(h>cap) h=cap; o->hp+=h; if(o->hp>o->maxhp) o->hp=o->maxhp; }
}
static void summon(dg_t*g,enemy_t*by,int type){
    int alive=0; for(int j=0;j<DG_MAXE;j++) if(g->e[j].on&&g->e[j].hp>0&&g->e[j].summoned) alive++;
    int cap = DG_BOSS[DG_ETYPE[by->type].boss].elite ? 4 : 6;       /* a lifetime supply, not a stalemate */
    if(alive>=3 || by->spawns>=cap) return;
    by->spawns++;
    for(int j=0;j<DG_MAXE;j++) if(!g->e[j].on || (g->e[j].hp<=0 && g->e[j].flash<=0 && g->e[j].burn<=0)){
        float x=dg_front(by)-60; if(x<g->sx+140) x=g->sx+140;
        enemy_init(g,j,type,x); g->e[j].summoned=1; g->e[j].next=2; g->ev_summon++; return; }
}
static void boss_land(dg_t*g,int k,const bmove_t*mv){
    enemy_t*e=&g->e[k]; int f=front_unit(g); if(f<0) return;
    const float D=mv->dmg*BOSS_DMG;
    float dist=dg_front(e)-g->u[f].x;
    int reach = mv->kind==MV_SUMMON || mv->kind==MV_ROAR || mv->kind==MV_HEAL || (dist>-200 && dist<=mv->range);
    e->act=1; g->ev_boss_land++;
    if(!reach) return;                                         /* retreated out of range: dodged */
    switch(mv->kind){
    case MV_MELEE:  unit_hurt(g,f,D); if(DG_SHIELD[g->u[f].shield].special&SH_THORNS) enemy_hurt(g,k,6,EL_NONE,0,0); break;
    case MV_SWEEP:  for(int i=0;i<DG_MAXU;i++) unit_hurt(g,i,D); break;
    case MV_QUAKE:  for(int i=0;i<DG_MAXU;i++) unit_hurt(g,i,D); shove(g,(float)mv->n); break;
    case MV_VOLLEY: for(int n=0;n<mv->n;n++){ int v=random_unit(g); if(v>=0) shoot(g,1,v,e->x,e->y-60,D,DT_PIERCE,EL_NONE,3,dist+n*30); } break;
    case MV_SUMMON: summon(g,e,mv->arg); break;
    case MV_HEAL:   mend_foes(g,e->x,mv->n/100.0f,60); break;
    case MV_DIVE:   e->x=g->u[f].x+45+DG_ETYPE[e->type].hw; e->traits&=~TR_FLYING; e->y=0; e->grounded=mv->arg+1;
                    unit_hurt(g,f,D); break;
    case MV_CHARGE: unit_hurt(g,f,D); shove(g,(float)mv->n); break;     /* a lunge: the boss keeps its ground */
    case MV_ROAR:   e->enraged=1; g->ev_roar++; break;
    }
}
static void boss_beat(dg_t*g,int k){
    enemy_t*e=&g->e[k]; const etype_t*t=&DG_ETYPE[e->type]; const boss_t*b=&DG_BOSS[t->boss];
    if(e->grounded>0 && --e->grounded==0){ e->traits|=TR_FLYING; e->y=-150; }
    if(e->move>=0){ if(--e->windup<=0){ boss_land(g,k,&b->mv[e->move]); e->move=-1; e->next=e->enraged?b->rest_enraged:b->rest; } return; }
    if(e->frost>0){ e->frost--; return; }
    int f=front_unit(g); if(f<0) return;
    float dist=dg_front(e)-g->u[f].x; if(dist>450) return;    /* hasn't seen the squad yet */
    int pick=-1;
    if(!e->enraged && e->hp<e->maxhp*0.5f) for(int i=0;i<b->nmv;i++) if(b->mv[i].kind==MV_ROAR) pick=i;
    if(pick<0){ if(e->next>0){ e->next--; return; }
        int cand[8],n=0;
        for(int i=0;i<b->nmv;i++){ const bmove_t*mv=&b->mv[i];
            if(mv->kind==MV_ROAR || (mv->enraged_only && !e->enraged)) continue;
            if(mv->kind==MV_DIVE && e->grounded) continue;
            if(mv->kind!=MV_SUMMON && mv->kind!=MV_HEAL && dist>mv->range) continue;
            if(mv->kind==MV_HEAL && e->hp>e->maxhp*0.8f) continue;
        if(mv->kind==MV_SUMMON && e->spawns>=(b->elite?4:6)) continue;
            cand[n++]=i; if(mv->enraged_only) cand[n++]=i;            /* phase-2 moves come up more often */
            if(n>=7) break; }
        if(!n) return;
        pick=cand[rnd(g)%n]; }
    /* telegraphs: big moves 11 beats, so a GUARD or RETREAT drummed after seeing one is always in time
     * (worst case: it starts just after the player committed to another command); blows 4 beats */
    const bmove_t*mv=&b->mv[pick];
    e->move=pick; e->windup = mv->kind==MV_ROAR ? 2 : mv->windup<=1 ? 4 : 11; g->ev_boss_move=pick+1; g->ev_windup++;
}
static void enemies_beat(dg_t*g){
    for(int k=0;k<DG_MAXE;k++){ enemy_t*e=&g->e[k]; const etype_t*t=&DG_ETYPE[e->type];
        if(!e->on||e->hp<=0) continue;
        if(t->boss>=0){ boss_beat(g,k); continue; }
        if(!t->period) continue;
        if(e->frost>0){ e->frost--; continue; }             /* frozen: the countdown waits */
        int f=front_unit(g); if(f<0) return;
        float dx=dg_front(e)-g->u[f].x; int in_range = dx>-40 && dx<=t->range;
        if(e->traits&TR_SCOUT){ if(!e->fleeing && dx<220){ e->fleeing=1; g->ev_scout_flee++; } if(e->fleeing) continue; }
        int tele=(t->traits&TR_HEAVYSHOT)?11:1;                 /* beats of warning: a ballista visibly reloads */
        if(e->next>tele){ e->next--; continue; }
        if(!e->windup){ if(in_range){ e->windup=1; g->ev_windup++; e->next--; } continue; }
        if(e->next>0){ e->next--; continue; }
        e->windup=0; e->next=t->period;
        if(!in_range) continue;
        e->act=1;
        if(t->traits&TR_HEAVYSHOT) shoot(g,1,f,e->x,e->y-90,t->dmg*FOE_DMG,DT_PIERCE,EL_NONE,5,dx);
        else if(t->traits&TR_HEALER) mend_foes(g,e->x,0.15f,25);
        else if(t->traits&TR_RANGED){ int v=random_unit(g); if(v>=0) shoot(g,1,v,e->x,e->y-20,t->dmg*FOE_DMG,DT_CRUSH,EL_NONE,(t->traits&TR_BOMB)?4:3,dx); }
        else { unit_hurt(g,f,t->dmg*FOE_DMG);
            if(DG_SHIELD[g->u[f].shield].special&SH_THORNS) enemy_hurt(g,k,6,EL_NONE,0,0); }
    }
}

/* a scout running for the next fort; if it gets there the garrison is reinforced */
static void spawn_at(dg_t*g,int type,float x){
    for(int j=0;j<DG_MAXE;j++) if(!g->e[j].on || (g->e[j].hp<=0 && g->e[j].flash<=0 && g->e[j].burn<=0)){
        enemy_init(g,j,type,x); g->e[j].summoned=1; g->e[j].next=3; return; } }
static void scout_run(dg_t*g,int k,float dt){
    enemy_t*e=&g->e[k]; float d=75*dt; e->x+=d; e->fled+=d;   /* catchable by ranged units */
    int gate=-1; for(int j=0;j<DG_MAXE;j++){ enemy_t*o=&g->e[j]; if(o->on&&o->hp>0&&(o->traits&TR_GATE)&&o->x>e->x-30&&(gate<0||o->x<g->e[gate].x)) gate=j; }
    if((gate>=0 && e->x>=g->e[gate].x-20) || (gate<0 && e->fled>900)){
        float at = gate>=0 ? g->e[gate].x : e->x;
        e->on=0; e->hp=0; g->ev_alarm++;
        spawn_at(g,g->m->alarm[0],at+70); spawn_at(g,g->m->alarm[1],at+120); }
}

/* SPECIAL (super fever only): every soldier's signature move at once */
static void special(dg_t*g){
    int f=front_unit(g); if(f<0) return;
    float front=g->u[f].x; g->ev_special++;
    for(int i=0;i<DG_MAXU;i++){ unit_t*u=&g->u[i]; if(!u->on||u->hp<=0||u->weapon<0) continue;
        const weapon_t*w=&DG_WEAPON[u->weapon]; u->swing=1; fx(g,u->x,-40,0,FX_SPECIAL,u->cls);
        switch(u->cls){
        case CL_WARDEN:      /* SHIELD BASH: the foes at the front are hit, stunned 3 beats and thrown back; the wall holds */
            for(int k=0;k<DG_MAXE;k++){ enemy_t*e=&g->e[k]; float d=dg_front(e)-front;
                if(!e->on||e->hp<=0||(e->traits&TR_FLYING)||d<-60||d>170) continue;
                enemy_hurt(g,k,(w->dmg*2+10)*dg_mult(DT_CRUSH,EL_NONE,e->traits)*1.4f,EL_NONE,0,1);
                if(!(e->traits&(TR_STATIC|TR_GATE))){ if(e->frost<3) e->frost=3; e->kb+=DG_ETYPE[e->type].boss>=0?14:50; } }
            if(g->guard_until<(int)g->beat+8) g->guard_until=(int)g->beat+8;
            break;
        case CL_LANCER:      /* SPEAR RAIN: six javelins over everything ahead, flyers too */
            for(int n=0;n<6;n++){ int c[DG_MAXE],m=0;
                for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; float d=dg_front(e)-u->x; if(e->on&&e->hp>0&&d>-40&&d<520) c[m++]=k; }
                if(!m) break; int k=c[rnd(g)%m]; const enemy_t*e=&g->e[k];
                shoot(g,0,k,u->x,-70-n*6,w->dmg*1.3f*dg_mult(w->dtype,w->elem,e->traits)*1.4f,w->dtype,w->elem,0,dg_front(e)-u->x+n*40); }
            break;
        case CL_SLINGER: {   /* PIERCING SHOT: the toughest foe in sight, through any armour */
            int best=-1; for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; float d=dg_front(e)-u->x;
                if(e->on&&e->hp>0&&d>-40&&d<700&&(best<0||e->hp>g->e[best].hp)) best=k; }
            if(best>=0){ const enemy_t*e=&g->e[best]; float m=dg_mult(w->dtype,w->elem,e->traits); if(m<1) m=1;
                shoot(g,0,best,u->x,-40,w->dmg*5*m*1.4f,w->dtype,w->elem,6,dg_front(e)-u->x); }
            break; }
        case CL_BREAKER:     /* EARTHQUAKE: every ground foe near the front is crushed and shaken back */
            for(int k=0;k<DG_MAXE;k++){ enemy_t*e=&g->e[k]; float d=dg_front(e)-front;
                if(!e->on||e->hp<=0||(e->traits&TR_FLYING)||d<-60||d>320) continue;
                enemy_hurt(g,k,w->dmg*1.6f*dg_mult(DT_CRUSH,w->elem,e->traits)*1.4f,w->elem,0,1);
                if(!(e->traits&(TR_STATIC|TR_GATE))) e->kb+=DG_ETYPE[e->type].boss>=0?10:40; }
            break;
        case CL_MENDER:      /* BLESSING: half of everyone's health back, a guard and a rally */
            for(int j=0;j<DG_MAXU;j++){ unit_t*o=&g->u[j]; if(!o->on||o->hp<=0) continue; float b=o->hp;
                o->hp+=o->maxhp*0.5f; if(o->hp>o->maxhp) o->hp=o->maxhp; o->rallied=1; if(o->hp-b>=1) fx(g,o->x,0,o->hp-b,FX_HEAL,0); }
            if(g->guard_until<(int)g->beat+8) g->guard_until=(int)g->beat+8;
            g->ev_heal++; break;
        } }
}
void dg_beat(dg_t*g,int64_t beat,int cmd,int beat_in,int fever){
    if(g->won||g->lost) return;
    g->beat=beat; g->cmd=cmd; g->cmd_beat=beat_in; g->fever=fever;
    float span=4*BEAT_S;
    switch(cmd){
    case CMD_MARCH:
        if(beat_in==0){ float d=(fever?175.0f:125.0f)*(heavy(g)?0.75f:1); g->move_left=d; g->vx=d/span; }
        break;
    case CMD_RETREAT:
        if(beat_in==0) for(int i=0;i<DG_MAXU;i++) g->u[i].off=0;   /* fall back together */
        if(beat_in==0){ g->move_left=-110; g->vx=-110/(span*0.6f); }   /* quick: dodges a telegraphed blow */
        break;
    case CMD_STRIKE:
        if(beat_in==0) engage(g);
        if(beat_in==0 && fever && has_class(g,CL_BREAKER)){ g->move_left=30; g->vx=30/(BEAT_S*0.5f); }   /* breaker charge */
        if(beat_in>=1 || fever) strike(g);
        if(beat_in==3) for(int i=0;i<DG_MAXU;i++) g->u[i].rallied=0;
        break;
    case CMD_GUARD:
        if(beat_in==0){ g->guard_until=(int)beat+12;  /* a stance: holds through the next command and its response */
            for(int i=0;i<DG_MAXU;i++) g->u[i].off=0; }   /* close ranks: the shield wall */
        if(beat_in==2) heal_all(g,0.4f);
        break;
    case CMD_SPECIAL:
        if(beat_in==0) engage(g);
        if(beat_in==1) special(g);
        if(beat_in==3){ strike(g); for(int i=0;i<DG_MAXU;i++) g->u[i].rallied=0; }
        break;
    case CMD_RALLY:
        if(beat_in==1){ heal_all(g,1); for(int i=0;i<DG_MAXU;i++) if(g->u[i].on) g->u[i].rallied=1; }
        if(beat_in==3 && fever) heal_all(g,0.5f);
        break;
    default: break;
    }
    enemies_beat(g);
    if(dg_alive(g)==0) g->lost=1;
}

/* foes walk up: melee to contact (queueing behind each other), ranged and healers to ~75% of their
 * range; the garrison waits behind its standing gate; bosses stalk the squad but keep near their lair */
static float foe_speed(int type){
    switch(type){ case ET_GRUNT: return 38; case ET_IRONBACK: return 24; case ET_STONESLINGER: return 30; case ET_BRISTLER: return 60;
    case ET_HEXCALLER: return 26; case ET_GOURDTHROWER: return 28; case ET_BULWARK: return 18; case ET_FROSTLING: return 34; default: return 0; } }
static void foes_move(dg_t*g,float dt){
    int f=front_unit(g); if(f<0) return;
    float ux=g->u[f].x, base=fmaxf(g->sx+48,ux+28);
    for(int k=0;k<DG_MAXE;k++){ enemy_t*e=&g->e[k]; const etype_t*t=&DG_ETYPE[e->type];
        if(!e->on||e->hp<=0||e->frost>0||e->fleeing||(e->traits&(TR_STATIC|TR_GATE|TR_FLYING|TR_SCOUT))) continue;
        float fr=dg_front(e); if(fr-g->sx>480) continue;            /* not seen the squad yet */
        int sheltered=0, ahead=0;
        for(int j=0;j<DG_MAXE;j++){ const enemy_t*o=&g->e[j]; if(j==k||!o->on||o->hp<=0) continue;
            if((o->traits&TR_GATE) && o->x<e->x && o->x>g->sx) sheltered=1;
            if(!(o->traits&(TR_STATIC|TR_GATE|TR_FLYING)) && o->x<e->x && e->x-o->x<220 && DG_ETYPE[o->type].boss<0) ahead++; }
        if(sheltered) continue;
        float want, v;
        if(t->boss>=0){ if(e->move>=0||e->act>0.3f) continue; v=e->enraged?32:22; want=fmaxf(base+40,e->home-320); }
        else { v=foe_speed(e->type); if(v<=0||e->act>0.3f) continue;
            want = (t->traits&(TR_RANGED|TR_HEALER)) ? fmaxf(base,ux+t->range*0.75f) : base+30*ahead; }
        if(fr>want+1){ float d=fminf(fr-want,v*dt); e->x-=d; e->walk+=d; }
    }
}
void dg_step(dg_t*g,float dt){
    if(g->won||g->lost) return;
    g->time+=dt;
    if(g->move_left!=0){ float d=g->vx*dt;
        if(fabsf(d)>=fabsf(g->move_left)){ d=g->move_left; g->move_left=0; } else g->move_left-=d;
        g->sx+=d; }
    float lim=block_limit(g); if(g->sx>lim){ g->sx=lim; if(g->move_left>0) g->move_left=0; }
    if(g->sx<120) { g->sx=120; if(g->move_left<0) g->move_left=0; }
    if(g->sx>g->m->length){ g->sx=(float)g->m->length; g->move_left=0; }   /* the banner: wait there for the last foes */
    float tx[DG_MAXU]={0}; formation(g,tx);
    int calm=1; for(int k=0;k<DG_MAXE;k++){ const enemy_t*e=&g->e[k]; if(e->on&&e->hp>0&&fabsf(dg_front(e)-g->sx)<450) calm=0; }
    if(calm) for(int i=0;i<DG_MAXU;i++){ float*o=&g->u[i].off; *o-=*o*fminf(1,dt*3); if(fabsf(*o)<0.5f) *o=0; }   /* back into formation */
    for(int i=0;i<DG_MAXU;i++){ unit_t*u=&g->u[i]; if(!u->on) continue;
        if(u->hp>0 && calm){ u->hp+=4*dt; if(u->hp>u->maxhp) u->hp=u->maxhp; }   /* catching breath between fights */
        if(u->hp>0) u->x+= (tx[i]-u->kb-u->x)*fminf(1,dt*16);
        u->kb-=u->kb*fminf(1,dt*3); if(u->kb<0.5f) u->kb=0;   /* staggered back, then steps in again */
        if(u->flash>0) u->flash-=dt; if(u->swing>0) u->swing-=dt*6; if(u->swing<0) u->swing=0; }
    foes_move(g,dt);
    for(int k=0;k<DG_MAXE;k++){ enemy_t*e=&g->e[k]; if(!e->on) continue;
        if(e->fleeing && e->hp>0) scout_run(g,k,dt);
        if(e->hp>0 && (e->traits&TR_FLYING) && DG_ETYPE[e->type].boss<0){      /* flyers hound the squad */
            float want=g->sx+170, d=want-e->x; if(fabsf(d)>60) e->x+= (d>0?1:-1)*fminf(fabsf(d),70*dt); }
        if(e->kb!=0){ float m=e->kb*fminf(1,dt*12); e->x+=m; e->kb-=m; if(fabsf(e->kb)<0.3f) e->kb=0; }
        if(e->flash>0) e->flash-=dt; if(e->act>0) e->act-=dt*4;
        e->bob+=dt;
        if(e->hp>0 && e->burn>0){ e->burn-=dt; e->hp-=3*dt*((e->traits&TR_WOODEN)?2:1);
            if(e->hp<=0){ e->hp=0; g->ev_kill++; if(DG_ETYPE[e->type].boss>=0) g->ev_boss_down++; if(!e->summoned) drop(g); } } }
    for(int k=0;k<DG_MAXP;k++){ proj_t*p=&g->p[k]; if(!p->on) continue;
        p->t+=dt; if(p->t<p->dur) continue;
        p->on=0;
        if(!p->foe){ const enemy_t*e=&g->e[p->target]; enemy_hurt(g,p->target,p->dmg,p->elem,1,p->kind==6||dg_mult(p->dtype,p->elem,e->traits)>=1.4f); continue; }
        unit_t*u=&g->u[p->target];
        if(u->on && u->hp>0 && guarding(g) && (DG_SHIELD[u->shield].special&SH_REFLECT)){   /* mirror: send it back */
            int best=-1; float bd=1e9f; for(int j=0;j<DG_MAXE;j++){ enemy_t*e=&g->e[j]; if(!e->on||e->hp<=0) continue; float d=fabsf(e->x-u->x); if(d<bd){ bd=d; best=j; } }
            g->ev_block++;
            if(best>=0) shoot(g,0,best,u->x,-30,p->dmg*1.5f,DT_CRUSH,EL_NONE,3,bd);
            continue; }
        if(p->kind==4) for(int i=0;i<DG_MAXU;i++) unit_hurt(g,i,p->dmg);   /* gourd bomb: everyone */
        else unit_hurt(g,p->target,p->dmg);
    }
    if(dg_alive(g)==0) g->lost=1;
    else if(dg_enemies_left(g)==0 && g->sx>=g->m->length-40) g->won=1;
}
