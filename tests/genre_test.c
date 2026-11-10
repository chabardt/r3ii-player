/* Host test for genre.c: unit checks, then (optionally) the per-artist breakdown of a real
 * index file (the device's /usr/data/r3_mix.idx: rel \t artist \t genre \t year).
 *   gcc -I src/ui -o build/genre_test tests/genre_test.c src/ui/genre.c && build/genre_test [r3_mix.idx] */
#include "genre.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
static void check(const char*v,int fam,const char*sub){ char s[96]; int f=genre_classify(v,s,sizeof s);
    if(f!=fam||strcmp(s,sub)){ printf("FAIL %-28s -> %d '%s' (want %d '%s')\n",v,f,s,fam,sub); fails++; } }
static char got[16][96]; static int ngot;
static void collect(void*u,const char*v){ (void)u; if(ngot<16) snprintf(got[ngot++],96,"%s",v); }
static void check_split(const char*tag,int n,const char*a,const char*b){ ngot=0; genre_split(tag,collect,0);
    if(ngot!=n || strcmp(got[0],a) || (n>1&&strcmp(got[1],b))){ printf("FAIL split '%s' -> %d '%s' '%s'\n",tag,ngot,ngot?got[0]:"",ngot>1?got[1]:""); fails++; } }

/* --- per-artist aggregation (same rule as explore.c: main genre by track count, then the most
 * frequent specific subgenre inside it) --- */
#define MAXA 4000
typedef struct { char name[128]; int fam[GF_N]; char sub[48][64]; int subn[48]; int nsub; } art_t;
static art_t A[MAXA]; static int NA;
static art_t* art(const char*n){ for(int i=0;i<NA;i++) if(!strcmp(A[i].name,n)) return &A[i];
    if(NA>=MAXA) return 0; snprintf(A[NA].name,128,"%s",n); return &A[NA++]; }
static art_t*cur; static char unk[400][64]; static int unkn[400], nunk;
static void add(void*u,const char*v){ (void)u; char s[96]; int f=genre_classify(v,s,sizeof s);
    if(f<0){ char k[64]; snprintf(k,64,"%s",v); int i; for(i=0;i<nunk;i++) if(!strcmp(unk[i],k)) break;
        if(i==nunk && nunk<400) snprintf(unk[nunk++],64,"%s",k); if(i<400) unkn[i]++; return; }
    cur->fam[f]++; if(!s[0]) return;
    int i; for(i=0;i<cur->nsub;i++) if(!strcmp(cur->sub[i],s)) break;
    if(i==cur->nsub){ if(i>=48) return; snprintf(cur->sub[i],64,"%s",s); cur->subn[i]=0; cur->nsub++; }
    cur->subn[i]++; }

int main(int ac,char**av){
    check("Heavy Metal",GF_METAL,"Heavy metal"); check("Metal",GF_METAL,""); check("métal",GF_METAL,"");
    check("Punkrock",GF_PUNK,"Punk rock"); check("Pop Punk",GF_PUNK,"Pop punk"); check("Rock",GF_ROCK,"");
    check("Alternative",GF_ROCK,"Alternative rock"); check("Synth Pop",GF_ELECTRO,"Synth pop");
    check("Trip Hop",GF_ELECTRO,"Trip hop"); check("Rap et Hip Hop",GF_HIPHOP,""); check("B.O.",GF_CLASSICAL,"Soundtrack");
    check("Other",GF_NONE,""); check("<Inconnu>",GF_NONE,""); check("Folk-Rock",GF_ROCK,"Folk-rock");
    check("Chanson Française",GF_POP,"Chanson française"); check("(17)Rock",GF_ROCK,""); check("(17)",GF_NONE,"");
    check("Neue Deutsche Härte",GF_METAL,"Industrial metal"); check("R&B",GF_JAZZ,"R&B"); check("Britpop",GF_ROCK,"Britpop");
    check("Viking Metal",GF_METAL,"Viking metal"); check("Electronic",GF_ELECTRO,""); check("Grunge",GF_ROCK,"Grunge");
    check_split("Heavy MetalRock",2,"Heavy Metal","Rock"); check_split("Stoner Rock, Desert Rock",2,"Stoner Rock","Desert Rock");
    check_split("Rock;Pop",2,"Rock","Pop"); check_split("R&B",1,"R&B",0); check_split("Hard Rock / Blues",2,"Hard Rock","Blues");
    printf("unit checks: %s\n",fails?"FAILED":"ok");
    if(ac<2) return fails!=0;
    FILE*f=fopen(av[1],"r"); if(!f){ perror(av[1]); return 1; }
    char l[2048]; int tracks=0;
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\n")]=0; char*t1=strchr(l,'\t'); if(!t1) continue; *t1=0;
        char*t2=strchr(t1+1,'\t'); if(!t2) continue; *t2=0; char*t3=strchr(t2+1,'\t'); if(t3) *t3=0;
        if(strncmp(l,"Music/",6)) continue; tracks++;
        cur=art(t1+1); if(cur) genre_split(t2+1,add,0); }
    fclose(f);
    int famart[GF_N]={0}, none=0; char subs[GF_N][200][64]; int subc[GF_N][200]={{0}}, ns[GF_N]={0};
    for(int i=0;i<NA;i++){ int b=-1; for(int k=0;k<GF_N;k++) if(A[i].fam[k] && (b<0||A[i].fam[k]>A[i].fam[b])) b=k;
        if(b<0){ none++; continue; } famart[b]++;
        int bs=-1; char s[96]; for(int j=0;j<A[i].nsub;j++){ if(genre_classify(A[i].sub[j],s,sizeof s)!=b) continue;
            if(bs<0||A[i].subn[j]>A[i].subn[bs]) bs=j; }
        const char*name= bs>=0?A[i].sub[bs]:"(general)"; int k;
        for(k=0;k<ns[b];k++) if(!strcmp(subs[b][k],name)) break;
        if(k==ns[b] && k<200){ snprintf(subs[b][k],64,"%s",name); ns[b]++; } if(k<200) subc[b][k]++; }
    printf("%d tracks, %d artists, %d without a usable genre\n",tracks,NA,none);
    for(int b=0;b<GF_N;b++){ printf("\n%s: %d artists\n  ",genre_family_name(b),famart[b]);
        for(int r=0;r<14;r++){ int m=-1; for(int k=0;k<ns[b];k++) if(subc[b][k]>0&&(m<0||subc[b][k]>subc[b][m])) m=k;
            if(m<0) break; printf("%s %d, ",subs[b][m],subc[b][m]); subc[b][m]=-subc[b][m]; } printf("\n"); }
    printf("\nunmapped:"); for(int r=0;r<30;r++){ int m=-1; for(int i=0;i<nunk;i++) if(unkn[i]>0&&(m<0||unkn[i]>unkn[m])) m=i;
        if(m<0) break; printf(" %s(%d)",unk[m],unkn[m]); unkn[m]=0; } printf("\n");
    return fails!=0;
}
