/* host test for sapper_core: safe first dig, flood fill, chords, wins, losses, score.
 * gcc -O1 -Wall -I src/ui -o build/sapper_test tests/sapper_core_test.c src/ui/sapper_core.c && build/sapper_test */
#include "sapper_core.h"
#include <stdio.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL: " __VA_ARGS__); printf("\n"); fail++; } }while(0)

/* solver that cheats (reads the mine map) to play a game to a win through the public API */
static int play_win(sapper_t*g){
    for(int guard=0;guard<10000 && !g->over;guard++){
        int moved=0;
        for(int y=0;y<g->rows && !g->over;y++) for(int x=0;x<g->cols && !g->over;x++){
            if(g->mine[y][x] && !g->flag[y][x]){ sp_flag(g,x,y); moved=1; }
            else if(!g->mine[y][x] && !g->open[y][x]){ sp_dig(g,x,y); moved=1; } }
        if(!moved) break; }
    return g->won;
}
int main(void){
    for(int lv=0;lv<SP_NLEVELS;lv++) for(unsigned s=1;s<=40;s++){
        sapper_t g; sp_new(&g,lv,s);
        int cx=g.cols/2, cy=g.rows/2; int n=sp_dig(&g,cx,cy);
        int mines=0; for(int y=0;y<g.rows;y++) for(int x=0;x<g.cols;x++) mines+=g.mine[y][x];
        CHECK(mines==g.mines,"lv %d seed %u: %d mines laid, want %d",lv,s,mines,g.mines);
        CHECK(!g.over && n>=9,"lv %d seed %u: first dig must open an area (opened %d)",lv,s,n);
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) CHECK(!g.mine[cy+dy][cx+dx],"lv %d seed %u: mine next to the first dig",lv,s);
        /* every open square's number matches the map; flood fill opened all neighbours of zeros */
        int opened=0;
        for(int y=0;y<g.rows;y++) for(int x=0;x<g.cols;x++){ if(!g.open[y][x]) continue; opened++;
            int a=0; for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){ int nx=x+dx, ny=y+dy;
                if((dx||dy)&&nx>=0&&ny>=0&&nx<g.cols&&ny<g.rows){ a+=g.mine[ny][nx];
                    if(g.adj[y][x]==0) CHECK(g.open[ny][nx],"lv %d seed %u: zero at %d,%d left a neighbour closed",lv,s,x,y); } }
            CHECK(a==g.adj[y][x],"adjacency"); }
        CHECK(opened==g.opened,"opened counter %d vs %d",g.opened,opened);
        CHECK(play_win(&g) && g.flags==g.mines,"lv %d seed %u: winnable to the end",lv,s);
        CHECK(sp_score(&g,60)>0,"score on win");
    }
    /* chord: flags placed around a number open the rest; a wrong flag loses */
    { sapper_t g; sp_new(&g,SP_EASY,7); sp_dig(&g,4,6);
      int done=0;
      for(int y=0;y<g.rows && !done;y++) for(int x=0;x<g.cols && !done;x++){
          if(!g.open[y][x]||!g.adj[y][x]) continue;
          int closed_safe=0; for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){ int nx=x+dx, ny=y+dy;
              if(nx<0||ny<0||nx>=g.cols||ny>=g.rows) continue;
              if(g.mine[ny][nx]) sp_flag(&g,nx,ny); else if(!g.open[ny][nx]) closed_safe++; }
          if(!closed_safe) continue;
          int before=g.opened; sp_chord(&g,x,y);
          CHECK(g.opened>before && !g.over,"chord opens the safe neighbours"); done=1; }
      CHECK(done,"found a square to chord"); }
    { sapper_t g; sp_new(&g,SP_EASY,9); sp_dig(&g,4,6);
      int done=0;
      for(int y=0;y<g.rows && !done;y++) for(int x=0;x<g.cols && !done;x++){
          if(!g.open[y][x]||!g.adj[y][x]) continue;
          /* flag adj-count closed SAFE squares instead of the mines */
          int need=g.adj[y][x], have=0, safe=0;
          for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){ int nx=x+dx, ny=y+dy;
              if(nx<0||ny<0||nx>=g.cols||ny>=g.rows||g.open[ny][nx]) continue; if(!g.mine[ny][nx]) safe++; else have++; }
          if(safe<need||have==0) continue;
          for(int dy=-1;dy<=1 && need;dy++) for(int dx=-1;dx<=1 && need;dx++){ int nx=x+dx, ny=y+dy;
              if(nx<0||ny<0||nx>=g.cols||ny>=g.rows||g.open[ny][nx]||g.mine[ny][nx]) continue; sp_flag(&g,nx,ny); need--; }
          sp_chord(&g,x,y); CHECK(g.over && !g.won && g.boom_x>=0,"chord with a wrong flag loses"); done=1; }
      CHECK(done,"found a square for the wrong-flag test"); }
    /* digging a mine loses; flags can't be dug; nothing works after the end */
    { sapper_t g; sp_new(&g,SP_MEDIUM,3); sp_dig(&g,0,0);
      int mx=-1,my=-1; for(int y=0;y<g.rows&&mx<0;y++) for(int x=0;x<g.cols;x++) if(g.mine[y][x]){ mx=x; my=y; break; }
      sp_flag(&g,mx,my); CHECK(sp_dig(&g,mx,my)==0 && !g.over,"flagged square can't be dug");
      sp_flag(&g,mx,my); sp_dig(&g,mx,my); CHECK(g.over && !g.won && g.boom_x==mx && g.boom_y==my,"digging a mine loses");
      int o=g.opened; sp_dig(&g,0,g.rows-1); CHECK(g.opened==o,"no digging after the end"); }
    /* harder and faster score more */
    { sapper_t e,h; e.won=h.won=1; e.mines=12; h.mines=60;
      CHECK(sp_score(&h,300)>sp_score(&e,30),"hard in 5 min beats easy in 30 s");
      CHECK(sp_score(&e,20)>sp_score(&e,60),"faster scores more"); }
    printf(fail?"FAILED %d\n":"ALL OK\n",fail); return fail!=0;
}
