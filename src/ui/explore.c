/* explore.c — see explore.h */
#include "explore.h"
#include "smartmix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#ifndef EX_SD_ROOT
#define EX_SD_ROOT "/usr/data/mnt/sd_0"
#endif
#define GENRES_FILE EX_SD_ROOT "/Genres.txt"
#define MAXART 4000
#define MAXALB 6000         /* ~2.7k albums on a 24k-track card */
#define MAXOV 1500
#define MAXSUB 800
#define HIST 40

typedef struct { char name[128]; int fam, sub, ntrk, plays, yrsum, yrn, nalb, famc[GF_N], sc; } art_t;
typedef struct { char dir[320]; int artist, ntrk, plays; } alb_t;
typedef struct { char name[48]; int fam, nart; } sub_t;
typedef struct { int sub, n, next; } sc_t;                  /* per-artist subgenre counts (linked) */
typedef struct { char*key; int n; int fam[8], sub[8]; } memo_t;   /* tag string -> classified values */
typedef struct { char name[128]; int n; int fam[8], sub[8]; } ov_t;  /* Genres.txt artist line */

static art_t *A; static int NA;
static alb_t *L; static int NL;
static sub_t S[MAXSUB]; static int NS;
static sc_t *SC; static int NSC, CAPSC;
static memo_t *M; static int NM;
static ov_t *OV; static int NOV; static ov_t *MB; static int NMB;
static char (*AL_RAW)[64], (*AL_TO)[64]; static int NAL;   /* "genre:" aliases */
static int g_ver=-1, g_famn[EX_NFAM];
static char g_hist[HIST][128]; static int g_hn, g_hpos;

/* ---- small hash maps (open addressing, index+1, 0 = free) ---- */
static unsigned hstr(const char*s){ unsigned h=2166136261u; while(*s){ h^=(unsigned char)*s++; h*=16777619u; } return h; }
#define HA 8192
#define HL 16384
#define HM 8192
static int *hA, *hL, *hM;

static int sub_id(int fam,const char*name){
    if(!name||!name[0]||fam<0) return -1;
    for(int i=0;i<NS;i++) if(S[i].fam==fam && !strcasecmp(S[i].name,name)) return i;
    if(NS>=MAXSUB) return -1;
    snprintf(S[NS].name,sizeof S[NS].name,"%s",name); S[NS].fam=fam; S[NS].nart=0; return NS++; }

static void lower(const char*in,char*out,int cap){ int j=0; for(;*in&&j<cap-1;in++) out[j++]=(char)tolower((unsigned char)*in); out[j]=0; }
static char* trim(char*s){ while(isspace((unsigned char)*s)) s++; int n=(int)strlen(s); while(n>0&&isspace((unsigned char)s[n-1])) s[--n]=0; return s; }

/* one raw value -> (fam, sub id), with the Genres.txt spelling aliases */
static int classify(const char*v,int*sub){
    char lv[64]; lower(v,lv,sizeof lv);
    for(int i=0;i<NAL;i++) if(!strcmp(lv,AL_RAW[i])){ v=AL_TO[i]; break; }
    char s[96]; int f=genre_classify(v,s,sizeof s); *sub = f>=0 ? sub_id(f,s) : -1; return f; }
typedef struct { int n; int*fam; int*sub; } acc_t;
static void acc_value(void*u,const char*v){ acc_t*a=u; if(a->n>=8) return;
    int s, f=classify(v,&s); if(f<0) return; a->fam[a->n]=f; a->sub[a->n]=s; a->n++; }
static memo_t* memo(const char*tag){
    unsigned k=hstr(tag)&(HM-1);
    while(hM[k]){ memo_t*m=&M[hM[k]-1]; if(!strcmp(m->key,tag)) return m; k=(k+1)&(HM-1); }
    if(NM>=HM/2) return 0;
    memo_t*m=&M[NM]; m->key=strdup(tag); m->n=0; acc_t a={0,m->fam,m->sub}; genre_split(tag,acc_value,&a); m->n=a.n;
    hM[k]=++NM; return m; }

/* family from a Genres.txt "Metal > Viking metal" */
static int fam_by_name(const char*n){
    static const struct { const char*w; int f; } F[]={ {"metal",GF_METAL},{"rock",GF_ROCK},{"punk",GF_PUNK},{"electro",GF_ELECTRO},
        {"electronic",GF_ELECTRO},{"hip-hop",GF_HIPHOP},{"hip hop",GF_HIPHOP},{"rap",GF_HIPHOP},{"jazz",GF_JAZZ},{"blues",GF_JAZZ},
        {"soul",GF_JAZZ},{"jazz / blues / soul",GF_JAZZ},{"pop",GF_POP},{"folk",GF_FOLK},{"world",GF_FOLK},{"folk / world",GF_FOLK},
        {"classical",GF_CLASSICAL},{"soundtrack",GF_CLASSICAL},{"classical / soundtrack",GF_CLASSICAL},{0,0} };
    char l[64]; lower(n,l,sizeof l); for(int i=0;F[i].w;i++) if(!strcmp(l,F[i].w)) return F[i].f; return -1; }
static void ov_parse(ov_t*o,char*rhs){
    o->n=0; char*gt=strchr(rhs,'>');
    if(gt){ *gt=0; int f=fam_by_name(trim(rhs));
        if(f>=0){ o->fam[0]=f; o->sub[0]=sub_id(f,trim(gt+1)); o->n=1; return; } *gt='>'; }
    acc_t a={0,o->fam,o->sub}; genre_split(rhs,acc_value,&a); o->n=a.n; }
/* two passes: "genre:" spelling aliases first, so the artist lines are classified with them */
static void load_genres_txt(void){
    NOV=NMB=NAL=0;
    for(int pass=0;pass<2;pass++){
        FILE*f=fopen(GENRES_FILE,"r"); if(!f) return;
        char l[512]; int mb=0;
        while(fgets(l,sizeof l,f)){ char*s=trim(l); if(!s[0]||s[0]=='#') continue;
            if(!strcasecmp(s,"[musicbrainz]")){ mb=1; continue; }
            char*eq=strstr(s," = "); int el=3; if(!eq){ eq=strchr(s,'='); el=1; } if(!eq) continue;
            *eq=0; char*lhs=trim(s), *rhs=trim(eq+el);
            int alias=!strncasecmp(lhs,"genre:",6);
            if(pass==0){ if(alias && NAL<400){ lower(trim(lhs+6),AL_RAW[NAL],64); snprintf(AL_TO[NAL],64,"%s",rhs); NAL++; } continue; }
            if(alias) continue;
            ov_t*o = mb ? (NMB<MAXOV?&MB[NMB++]:0) : (NOV<MAXOV?&OV[NOV++]:0); if(!o) continue;
            snprintf(o->name,sizeof o->name,"%s",lhs); ov_parse(o,rhs); }
        fclose(f); }
}
static const ov_t* ov_find(const ov_t*t,int n,const char*name){ for(int i=0;i<n;i++) if(!strcasecmp(t[i].name,name)) return &t[i]; return 0; }

/* ---- the pass over the index ---- */
static int art_get(const char*name){
    unsigned k=hstr(name)&(HA-1);
    while(hA[k]){ int i=hA[k]-1; if(!strcmp(A[i].name,name)) return i; k=(k+1)&(HA-1); }
    if(NA>=MAXART) return -1;
    art_t*a=&A[NA]; memset(a,0,sizeof *a); snprintf(a->name,sizeof a->name,"%s",name); a->sub=-1; a->sc=-1;
    hA[k]=++NA; return NA-1; }
static int alb_get(const char*dir,int artist){
    unsigned k=hstr(dir)&(HL-1);
    while(hL[k]){ int i=hL[k]-1; if(!strcmp(L[i].dir,dir)) return i; k=(k+1)&(HL-1); }
    if(NL>=MAXALB) return -1;
    alb_t*b=&L[NL]; snprintf(b->dir,sizeof b->dir,"%s",dir); b->artist=artist; b->ntrk=b->plays=0;
    A[artist].nalb++; hL[k]=++NL; return NL-1; }
static void sc_add(int a,int sub){
    for(int i=A[a].sc;i>=0;i=SC[i].next) if(SC[i].sub==sub){ SC[i].n++; return; }
    if(NSC>=CAPSC){ int nc=CAPSC?CAPSC*2:4096; sc_t*n=realloc(SC,sizeof(sc_t)*nc); if(!n) return; SC=n; CAPSC=nc; }
    SC[NSC].sub=sub; SC[NSC].n=1; SC[NSC].next=A[a].sc; A[a].sc=NSC++; }
static void on_track(void*u,const char*rel,const char*artist,const char*genre,int yr,int plays,int skips,int fav){
    (void)u; (void)skips; (void)fav;
    if(strncmp(rel,"Music/",6) || !artist[0]) return;            /* podcasts (Others/) are not music */
    int a=art_get(artist); if(a<0) return;
    A[a].ntrk++; A[a].plays+=plays; if(yr){ A[a].yrsum+=yr; A[a].yrn++; }
    char dir[320]; if((int)strlen(rel)>=(int)sizeof dir) return;   /* path too long to replay: skip */
    snprintf(dir,sizeof dir,"%s",rel); char*sl=strrchr(dir,'/'); if(sl) *sl=0;
    int b=alb_get(dir,a); if(b>=0){ L[b].ntrk++; L[b].plays+=plays; }
    if(genre[0]){ memo_t*m=memo(genre); if(m) for(int i=0;i<m->n;i++){ A[a].famc[m->fam[i]]++; if(m->sub[i]>=0) sc_add(a,m->sub[i]); } }
}
static void resolve(art_t*a){
    const ov_t*o=ov_find(OV,NOV,a->name);
    if(o && o->n){ a->fam=o->fam[0]; a->sub=-1; for(int i=0;i<o->n;i++) if(o->fam[i]==a->fam && o->sub[i]>=0){ a->sub=o->sub[i]; break; } return; }
    a->fam=-1; for(int f=0;f<GF_N;f++) if(a->famc[f] && (a->fam<0||a->famc[f]>a->famc[a->fam])) a->fam=f;
    a->sub=-1; int best=0;
    if(a->fam>=0) for(int i=a->sc;i>=0;i=SC[i].next) if(S[SC[i].sub].fam==a->fam && SC[i].n>best){ best=SC[i].n; a->sub=SC[i].sub; }
    if(a->fam<0 || a->sub<0){ const ov_t*m=ov_find(MB,NMB,a->name);       /* MusicBrainz: fallback only */
        if(m && m->n){
            if(a->fam<0){ a->fam=m->fam[0]; for(int i=0;i<m->n;i++) if(m->fam[i]==a->fam && m->sub[i]>=0){ a->sub=m->sub[i]; break; } }
            else for(int i=0;i<m->n;i++) if(m->fam[i]==a->fam && m->sub[i]>=0){ a->sub=m->sub[i]; break; } } }
    if(a->fam<0){ a->fam=EX_UNSORTED; a->sub=-1; }
}
static void free_all(void){
    if(M) for(int i=0;i<NM;i++) free(M[i].key);
    free(A); free(L); free(SC); free(M); free(OV); free(MB); free(AL_RAW); free(AL_TO); free(hA); free(hL); free(hM);
    A=0; L=0; SC=0; M=0; OV=MB=0; AL_RAW=AL_TO=0; hA=hL=hM=0; NA=NL=NSC=CAPSC=NM=NOV=NMB=NAL=NS=0; }
int explore_refresh(void){
    int v=smartmix_version(); if(!v) return 0;
    if(v==g_ver && A) return 1;
    free_all();
    A=calloc(MAXART,sizeof *A); L=calloc(MAXALB,sizeof *L); M=calloc(HM/2,sizeof *M);
    OV=calloc(MAXOV,sizeof *OV); MB=calloc(MAXOV,sizeof *MB); AL_RAW=calloc(400,64); AL_TO=calloc(400,64);
    hA=calloc(HA,sizeof(int)); hL=calloc(HL,sizeof(int)); hM=calloc(HM,sizeof(int));
    if(!A||!L||!M||!OV||!MB||!AL_RAW||!AL_TO||!hA||!hL||!hM){ free_all(); return 0; }
    static int seeded; if(!seeded){ srand((unsigned)time(0)); seeded=1; }
    load_genres_txt();
    if(smartmix_each(on_track,0)<0){ free_all(); return 0; }
    memset(g_famn,0,sizeof g_famn);
    for(int i=0;i<NA;i++){ resolve(&A[i]); g_famn[A[i].fam]++; if(A[i].sub>=0) S[A[i].sub].nart++; }
    g_ver=v; return 1;
}
int explore_tags_pending(void){ return !smartmix_tags_done(); }
int explore_artists(void){ return NA; }
int explore_fam_artists(int f){ return f>=0&&f<EX_NFAM ? g_famn[f] : 0; }
const char* explore_fam_name(int f){ return genre_family_name(f); }
int explore_subs(int fam,int*ids,int cap){ int n=0;
    for(int i=0;i<NS && n<cap;i++) if(S[i].fam==fam && S[i].nart>=2) ids[n++]=i;
    for(int i=1;i<n;i++){ int x=ids[i], j=i; while(j>0 && S[ids[j-1]].nart<S[x].nart){ ids[j]=ids[j-1]; j--; } ids[j]=x; }
    return n; }
const char* explore_sub_name(int id){ return id>=0&&id<NS ? S[id].name : ""; }
int explore_sub_artists(int id){ return id>=0&&id<NS ? S[id].nart : 0; }
int explore_sub_fam(int id){ return id>=0&&id<NS ? S[id].fam : -1; }

/* ---- picking ---- */
static int in_hist(const char*n){ for(int i=0;i<g_hn;i++) if(!strcmp(g_hist[i],n)) return 1; return 0; }
void explore_seen(int a){ if(a<0||a>=NA||in_hist(A[a].name)) return;
    snprintf(g_hist[g_hpos],128,"%s",A[a].name); g_hpos=(g_hpos+1)%HIST; if(g_hn<HIST) g_hn++; }
static float rnd(void){ return (float)rand()/(float)RAND_MAX; }
static float novelty(const art_t*a){ float ppt=(float)a->plays/(float)(a->ntrk?a->ntrk:1); return 1.0f/(1.0f+4.0f*ppt); }
int explore_pick(int fam,int sub){
    for(int pass=0;pass<2;pass++){          /* pass 1 ignores the history (everything was seen) */
        double tot=0; for(int i=0;i<NA;i++){ const art_t*a=&A[i];
            if(fam>=0&&a->fam!=fam) continue; if(sub>=0&&a->sub!=sub) continue; if(!pass&&in_hist(a->name)) continue; tot+=novelty(a); }
        if(tot<=0) continue;
        double r=rnd()*tot;
        for(int i=0;i<NA;i++){ const art_t*a=&A[i];
            if(fam>=0&&a->fam!=fam) continue; if(sub>=0&&a->sub!=sub) continue; if(!pass&&in_hist(a->name)) continue;
            r-=novelty(a); if(r<=0) return i; }
    }
    return -1; }
int explore_similar(int x){
    if(x<0||x>=NA) return explore_pick(-1,-1);
    const art_t*s=&A[x]; int yr = s->yrn ? s->yrsum/s->yrn : 0;
    int samefam=0; for(int i=0;i<NA;i++) if(i!=x && A[i].fam==s->fam && !in_hist(A[i].name)){ samefam=1; break; }
    int best=-1; float bs=-1;
    for(int i=0;i<NA;i++){ const art_t*a=&A[i]; if(i==x || in_hist(a->name)) continue;
        if(samefam && a->fam!=s->fam) continue;
        float sc = (s->sub>=0 && a->sub==s->sub) ? 3.0f : 0.0f;
        if(a->fam==s->fam) sc+=1.0f;
        if(yr && a->yrn){ int d=abs(a->yrsum/a->yrn-yr); sc += 1.0f - (d>25?25:d)/25.0f; }
        sc += 0.5f*novelty(a) + 1.2f*rnd();
        if(sc>bs){ bs=sc; best=i; } }
    return best>=0 ? best : explore_pick(s->fam,-1); }
int explore_find(const char*name){ if(!hA||!name) return -1; unsigned k=hstr(name)&(HA-1);
    while(hA[k]){ int i=hA[k]-1; if(!strcmp(A[i].name,name)) return i; k=(k+1)&(HA-1); } return -1; }

const char* explore_artist(int a){ return a>=0&&a<NA ? A[a].name : ""; }
int explore_artist_fam(int a){ return a>=0&&a<NA ? A[a].fam : EX_UNSORTED; }
const char* explore_artist_sub(int a){ return a>=0&&a<NA&&A[a].sub>=0 ? S[A[a].sub].name : ""; }
int explore_artist_albums(int a){ return a>=0&&a<NA ? A[a].nalb : 0; }
int explore_artist_plays(int a){ return a>=0&&a<NA ? A[a].plays : 0; }
int explore_album(int a,const char*not_dir,char*dir,int dcap,char*title,int tcap){
    if(a<0||a>=NA) return 0;
    const char*nd = not_dir && !strncmp(not_dir,EX_SD_ROOT "/",strlen(EX_SD_ROOT)+1) ? not_dir+strlen(EX_SD_ROOT)+1 : not_dir;
    int best=-1; float bs=1e9f, n=0;
    for(int pass=0;pass<2 && best<0;pass++)
        for(int i=0;i<NL;i++){ if(L[i].artist!=a) continue; if(!pass && nd && !strcmp(L[i].dir,nd)) continue;
            n++; float sc=(float)L[i].plays/(float)(L[i].ntrk?L[i].ntrk:1) + 0.3f*rnd();   /* least played, a bit random */
            if(sc<bs){ bs=sc; best=i; } }
    if(best<0) return 0;
    snprintf(dir,dcap,"%s/%s",EX_SD_ROOT,L[best].dir);
    const char*t=strrchr(L[best].dir,'/'); t = t ? t+1 : L[best].dir;
    /* tracks straight in the artist folder: no album folder */
    snprintf(title,tcap,"%s", !strcmp(t,A[a].name) ? "" : t);
    (void)n; return 1; }
int explore_nalbums(void){ return NL; }
const char* explore_album_dir(int i){ return i>=0&&i<NL ? L[i].dir : ""; }
int explore_album_artist(int i){ return i>=0&&i<NL ? L[i].artist : -1; }
int explore_artist_of_dir(const char*dir){
    if(!hL||!dir) return -1;
    const char*r = !strncmp(dir,EX_SD_ROOT "/",strlen(EX_SD_ROOT)+1) ? dir+strlen(EX_SD_ROOT)+1 : dir;
    unsigned k=hstr(r)&(HL-1);
    while(hL[k]){ int i=hL[k]-1; if(!strcmp(L[i].dir,r)) return L[i].artist; k=(k+1)&(HL-1); } return -1; }
