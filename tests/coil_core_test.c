/* host test for coil_core: invariants + a greedy autopilot that should eat a lot.
 * gcc -O1 -Wall -I src/ui -o build/coil_test tests/coil_core_test.c src/ui/coil_core.c && build/coil_test */
#include "coil_core.h"
#include <stdio.h>
#include <stdlib.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL: " __VA_ARGS__); printf("\n"); fail++; return; } }while(0)
static int safe(const coil_t*g,int d){ static const int DX[4]={0,1,0,-1},DY[4]={-1,0,1,0};
    cell_t h=coil_seg(g,0); int x=h.x+DX[d], y=h.y+DY[d];
    if(x<0||x>=CW||y<0||y>=CH) return 0;
    for(int i=0;i<g->len-1;i++){ cell_t c=coil_seg(g,i); if(c.x==x&&c.y==y) return 0; } return 1; }
static void run(unsigned seed){
    coil_t g; coil_new(&g,seed); int golds=0,berries=0,maxlen=0;
    for(int t=0;t<20000 && !g.over;t++){
        /* greedy: head toward the fruit (gold first), never straight into a wall/body */
        cell_t h=coil_seg(&g,0), f=g.gold_ticks>0?g.gold:g.fruit;
        int want[4]={ f.y<h.y, f.x>h.x, f.y>h.y, f.x<h.x }, pick=-1;
        for(int d=0;d<4;d++) if(want[d]&&safe(&g,d)&&(d+2)%4!=g.dir){ pick=d; break; }
        if(pick<0) for(int d=0;d<4;d++) if(safe(&g,d)&&(d+2)%4!=g.dir){ pick=d; break; }
        if(pick>=0) coil_turn(&g,pick);
        coil_step(&g);
        if(g.ate==F_GOLD) golds++; if(g.ate==F_BERRY) berries++;
        if(g.len>maxlen) maxlen=g.len;
        CHECK(g.len>=4 && g.len<=CMAX,"seed %u len %d",seed,g.len);
        for(int i=0;i<g.len;i++){ cell_t c=coil_seg(&g,i); CHECK(c.x>=0&&c.x<CW&&c.y>=0&&c.y<CH,"seed %u body out",seed);
            for(int j=i+1;j<g.len;j++){ cell_t d=coil_seg(&g,j); CHECK(!(c.x==d.x&&c.y==d.y) || g.over,"seed %u self overlap",seed); } }
        CHECK(!coil_at(&g,g.fruit.x,g.fruit.y) || g.over,"seed %u fruit under snake",seed);
        CHECK(coil_tick_ms(&g)>=70,"speed floor");
    }
    printf("seed %u: eaten %d score %d level %d maxlen %d gold %d berry %d over %d\n",seed,g.eaten,g.score,g.level,maxlen,golds,berries,g.over);
    CHECK(g.eaten>=5,"seed %u autopilot ate only %d",seed,g.eaten);   /* greedy bot: just a sanity bar */
}
int main(void){
    /* reversing is ignored, turning works, walls kill */
    coil_t g; coil_new(&g,1); coil_turn(&g,DIR_LEFT); coil_step(&g);
    if(g.over || g.dir!=DIR_RIGHT){ printf("FAIL: reverse not ignored\n"); fail++; }
    coil_new(&g,1); for(int i=0;i<40 && !g.over;i++) coil_step(&g);
    if(!g.over){ printf("FAIL: wall did not end the game\n"); fail++; }
    coil_new(&g,1); coil_turn_rel(&g,1); coil_step(&g);
    if(g.dir!=DIR_DOWN){ printf("FAIL: relative right turn\n"); fail++; }
    for(unsigned s=1;s<=8;s++) run(s);
    printf(fail?"FAILED %d\n":"ALL OK\n",fail); return fail!=0;
}
