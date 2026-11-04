/* host test for brick_core: autopilot paddle, invariants, progression.
 * gcc -O1 -Wall -I src/ui -o build/brick_test tests/brick_core_test.c src/ui/brick_core.c -lm && build/brick_test */
#include "brick_core.h"
#include <stdio.h>
#include <math.h>
static int fail;
#define CHECK(c,...) do{ if(!(c)){ printf("FAIL: " __VA_ARGS__); printf("\n"); fail++; return; } }while(0)
static void run(unsigned seed,int perfect,float minutes){
    bk_t g; bk_new_game(&g,seed); bk_launch(&g);
    float t=0, dt=1/60.0f; int maxballs=0, toasts[8]={0}, lvl_t=0; float since_progress=0; int last_score=0;
    while(t<minutes*60 && !g.over){
        /* autopilot: follow the lowest descending ball; imperfect = slow + offset */
        float tx=g.px+g.pw/2, best=-1;
        for(int i=0;i<BK_MAXB;i++) if(g.b[i].on && g.b[i].vy>0 && g.b[i].y>best){ best=g.b[i].y; tx=g.b[i].x; }
        if(perfect) bk_set_paddle(&g,tx+((int)(t*7)%40-20));
        else { float cx=g.px+g.pw/2, d=tx-cx+25; cx+= d>0?fminf(d,2.2f):fmaxf(d,-2.2f); bk_set_paddle(&g,cx); }
        if(bk_any_stuck(&g)) bk_launch(&g);
        bk_step(&g,dt); t+=dt;
        if(g.toast){ toasts[g.toast==100?7:g.toast]++; g.toast=0; }
        int nb=bk_balls(&g); if(nb>maxballs) maxballs=nb;
        CHECK(g.pw>=56 && g.pw<=160 && g.px>=0 && g.px+g.pw<=BK_W+0.01f,"seed %u paddle %f %f",seed,g.px,g.pw);
        for(int i=0;i<BK_MAXB;i++) if(g.b[i].on){ bk_ball_t*b=&g.b[i];
            CHECK(!isnan(b->x)&&!isnan(b->vx),"seed %u nan",seed);
            CHECK(b->x>=BK_R-0.01f && b->x<=BK_W-BK_R+0.01f,"seed %u ball x out %f",seed,b->x);
            CHECK(b->y>=BK_R-0.01f,"seed %u ball above top %f",seed,b->y);
            float v=sqrtf(b->vx*b->vx+b->vy*b->vy);
            CHECK(b->stuck || (v>150 && v<1100),"seed %u speed %f",seed,v); }
        if(g.score!=last_score){ last_score=g.score; since_progress=0; } else since_progress+=dt;
        if(since_progress>=90){ printf("left=%d bricks:",g.left); for(int r=0;r<BK_NR;r++)for(int c=0;c<BK_NC;c++) if(g.br[r][c].hp>0) printf(" r%dc%d",r,c); printf("\n"); for(int i=0;i<BK_MAXB;i++) if(g.b[i].on) printf(" ball %.0f,%.0f v %.0f,%.0f\n",g.b[i].x,g.b[i].y,g.b[i].vx,g.b[i].vy); }
        CHECK(since_progress<90,"seed %u no score for 90 s (ball looping?) level %d",seed,g.level);
        (void)lvl_t;
    }
    printf("seed %u %s: %.0fs level %d score %d over=%d maxballs %d powers M%d X%d F%d S%d W%d N%d clears %d pw %.0f\n",
        seed,perfect?"perfect":"sloppy ",t,g.level,g.score,g.over,maxballs,toasts[1],toasts[2],toasts[3],toasts[4],toasts[5],toasts[6],toasts[7],g.pw);
    if(!perfect) CHECK(g.over,"seed %u sloppy player never lost",seed);
}
int main(void){
    for(unsigned s=1;s<=6;s++) run(s,1,20);
    for(unsigned s=1;s<=6;s++) run(s,0,20);
    printf(fail?"FAILED %d\n":"ALL OK\n",fail); return fail!=0;
}
