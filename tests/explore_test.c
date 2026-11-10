/* Host test for explore.c on a real index (the device's /usr/data/r3_mix.idx).
 *   gcc -DEX_SD_ROOT='"/tmp/exsd"' -I src/ui -o build/explore_test tests/explore_test.c src/ui/explore.c src/ui/genre.c
 *   build/explore_test r3_mix.idx        (Genres.txt is read from EX_SD_ROOT) */
#include "explore.h"
#include "smartmix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char*idx; static int fails;
int smartmix_version(void){ return 1; }
int smartmix_tags_done(void){ return 1; }
int smartmix_each(smartmix_track_fn fn,void*u){
    FILE*f=fopen(idx,"r"); if(!f) return -1; char l[2048]; int n=0;
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\n")]=0; char*t1=strchr(l,'\t'); if(!t1) continue; *t1=0;
        char*t2=strchr(t1+1,'\t'); if(!t2) continue; *t2=0; char*t3=strchr(t2+1,'\t'); if(!t3) continue; *t3=0;
        fn(u,l,t1+1,t2+1,atoi(t3+1),(n%7)==0?3:0,0,0); n++; }   /* fake play counts on some tracks */
    fclose(f); return n; }
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } }while(0)

int main(int ac,char**av){
    if(ac<2){ printf("usage: explore_test r3_mix.idx\n"); return 1; }
    idx=av[1];
    CHECK(explore_refresh()==1,"refresh");
    printf("%d artists\n",explore_artists());
    int tot=0; for(int f=0;f<EX_NFAM;f++){ int ids[64], n=explore_subs(f,ids,64); tot+=explore_fam_artists(f);
        printf("%-24s %3d  ",explore_fam_name(f),explore_fam_artists(f));
        for(int i=0;i<n&&i<6;i++) printf("%s %d, ",explore_sub_name(ids[i]),explore_sub_artists(ids[i])); printf("\n"); }
    CHECK(tot==explore_artists(),"families cover every artist (%d vs %d)",tot,explore_artists());
    /* overrides from Genres.txt */
    int r=explore_find("Rammstein"); if(r>=0){ printf("Rammstein: %s > %s\n",explore_fam_name(explore_artist_fam(r)),explore_artist_sub(r));
        CHECK(explore_artist_fam(r)==GF_METAL && !strcmp(explore_artist_sub(r),"Industrial metal"),"Rammstein override"); }
    int e=explore_find("Eagles of Death Metal"); if(e>=0) printf("EODM: %s > %s\n",explore_fam_name(explore_artist_fam(e)),explore_artist_sub(e));
    /* pick inside a subgenre stays inside it; no repeats while unseen ones remain */
    int ids[64], n=explore_subs(GF_METAL,ids,64); CHECK(n>0,"metal has subgenres");
    if(n){ int sub=ids[0], k=explore_sub_artists(sub), seen[64]={0};
        for(int i=0;i<k && i<40;i++){ int a=explore_pick(GF_METAL,sub); CHECK(a>=0,"pick");
            CHECK(!strcmp(explore_artist_sub(a),explore_sub_name(sub)),"pick in sub");
            for(int j=0;j<i;j++) CHECK(seen[j]!=a,"repeat %s",explore_artist(a)); seen[i]=a; explore_seen(a); }
        printf("picked %d distinct %s artists\n",k<40?k:40,explore_sub_name(sub)); }
    /* similar + albums */
    int a=explore_pick(GF_METAL,-1); explore_seen(a);
    for(int i=0;i<5;i++){ int b=explore_similar(a); char d[1024],t[256];
        CHECK(b>=0 && b!=a,"similar");
        CHECK(explore_album(b,0,d,sizeof d,t,sizeof t),"album of %s",explore_artist(b));
        char d2[1024],t2[256]; int has2=explore_album(b,d,d2,sizeof d2,t2,sizeof t2);
        if(explore_artist_albums(b)>1) CHECK(has2 && strcmp(d,d2),"another album of %s",explore_artist(b));
        CHECK(explore_artist_of_dir(d)==b,"artist of dir");
        printf("  %s (%s > %s) -> %s [%s], %d albums\n",explore_artist(a),explore_fam_name(explore_artist_fam(a)),explore_artist_sub(a),
               explore_artist(b),t,explore_artist_albums(b));
        explore_seen(b); a=b; }
    printf("%s\n",fails?"FAILED":"ok"); return fails!=0;
}
