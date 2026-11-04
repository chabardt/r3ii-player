/* Brickstorm logic — see brick_core.h. */
#include "brick_core.h"
#include <math.h>
#include <string.h>

static unsigned rng;
static int rnd(int n){ rng=rng*1103515245u+12345u; return (int)((rng>>16)%(unsigned)n); }

float bk_base_speed(const bk_t*g){ float s=330.0f+25.0f*(g->level-1); return s>520?520:s; }

/* level layouts: a few shapes, specials sprinkled in, tougher bricks from level 2 */
static void build_level(bk_t*g){
    memset(g->br,0,sizeof g->br); g->left=0;
    int shape=(g->level-1)%5;
    for(int r=0;r<BK_NR;r++) for(int c=0;c<BK_NC;c++){
        int on=1;
        switch(shape){
        case 0: on = r>=1 && r<=6; break;                                   /* wall */
        case 1: on = (r+c)%2==0; break;                                     /* checker */
        case 2: on = c>=r && c<BK_NC-r && r<6; break;                      /* inverted pyramid */
        case 3: on = !(c==2||c==7) && r<7; break;                           /* columns with gaps */
        default: on = rnd(100)<70; break;                                   /* random */
        }
        if(!on) continue;
        bk_brick_t*b=&g->br[r][c];
        b->hp = (g->level>=2 && rnd(100)<12+3*g->level) ? 2 : 1;
        b->col = (int8_t)(r%7);
        int s=rnd(100);
        b->type = s<4?BT_MULTI : s<7?BT_BOMB : s<9?BT_FAST : s<11?BT_SLOW : s<13?BT_WIDE : s<15?BT_TINY : BT_NORMAL;
        if(b->type!=BT_NORMAL) b->hp=1;
        g->left++;
    }
    if(!g->left){ g->br[0][0].hp=1; g->left=1; }
    g->dirty_bricks=1;
}
static void serve(bk_t*g){
    memset(g->b,0,sizeof g->b);
    g->b[0].on=1; g->b[0].stuck=1; g->speed=1.0f;
    { float c=g->px+g->pw/2.0f; g->pw=BK_PW; bk_set_paddle(g,c); }   /* paddle back to normal */
    g->b[0].x=g->px+g->pw/2.0f; g->b[0].y=BK_PY-BK_R;
}
void bk_new_game(bk_t*g,unsigned seed){
    memset(g,0,sizeof *g); rng=seed?seed:1;
    g->level=1; g->pw=BK_PW; g->px=(BK_W-g->pw)/2.0f;
    build_level(g); serve(g);
}
void bk_set_paddle(bk_t*g,float cx){
    float x=cx-g->pw/2.0f; if(x<0)x=0; if(x>BK_W-g->pw)x=BK_W-g->pw; g->px=x;
}
int bk_balls(const bk_t*g){ int n=0; for(int i=0;i<BK_MAXB;i++) n+=g->b[i].on; return n; }
int bk_any_stuck(const bk_t*g){ for(int i=0;i<BK_MAXB;i++) if(g->b[i].on&&g->b[i].stuck) return 1; return 0; }
void bk_launch(bk_t*g){
    float sp=bk_base_speed(g)*g->speed;
    for(int i=0;i<BK_MAXB;i++) if(g->b[i].on && g->b[i].stuck){
        float a=(rnd(41)-20)*(float)M_PI/180.0f;           /* +-20 deg off vertical */
        g->b[i].stuck=0; g->b[i].vx=sp*sinf(a); g->b[i].vy=-sp*cosf(a); }
}
static void set_speed(bk_ball_t*b,float sp){
    float v=sqrtf(b->vx*b->vx+b->vy*b->vy); if(v<1) return;
    b->vx*=sp/v; b->vy*=sp/v;
}
/* keep the ball from going (nearly) horizontal: it would take forever to come back */
static void min_vertical(bk_ball_t*b){
    float v=sqrtf(b->vx*b->vx+b->vy*b->vy); if(v<1) return;
    float minvy=v*0.28f;
    if(fabsf(b->vy)<minvy){ b->vy = b->vy<0?-minvy:minvy;
        float vx2=v*v-b->vy*b->vy; b->vx = (b->vx<0?-1:1)*sqrtf(vx2>0?vx2:0); }
}
static void spawn_multi(bk_t*g,const bk_ball_t*src){
    for(int k=0;k<2;k++){
        int i=0; while(i<BK_MAXB && g->b[i].on) i++; if(i==BK_MAXB) return;
        bk_ball_t*n=&g->b[i]; *n=*src; n->bomb=0; n->stuck=0;
        float a=(k?1:-1)*0.45f, c=cosf(a), s=sinf(a);   /* +-26 deg */
        n->vx=src->vx*c-src->vy*s; n->vy=src->vx*s+src->vy*c;
        if(n->vy>0) n->vy=-n->vy;                         /* send them up */
        min_vertical(n);
    }
}
static int kill_brick(bk_t*g,int r,int c,int force){
    bk_brick_t*b=&g->br[r][c]; if(b->hp<=0) return 0;
    g->quiet=0;
    if(!force && b->hp>1){ b->hp--; g->score+=5*g->level; g->dirty_bricks=1; return 0; }
    int type=b->type; b->hp=0; g->left--; g->dirty_bricks=1;
    g->score += (type==BT_NORMAL?10:30)*g->level;
    return type+1;   /* 1..5 = destroyed brick type+1 */
}
static void power(bk_t*g,int type,bk_ball_t*ball){
    if(type==BT_NORMAL) return;
    g->toast=type;
    if(type==BT_MULTI) spawn_multi(g,ball);
    else if(type==BT_BOMB) ball->bomb=1;
    else if(type==BT_WIDE||type==BT_TINY){   /* resize around the current centre */
        float c=g->px+g->pw/2.0f; g->pw *= type==BT_WIDE?1.4f:0.7f;
        if(g->pw>160) g->pw=160;
        if(g->pw<56) g->pw=56;
        bk_set_paddle(g,c); }
    else if(type==BT_FAST||type==BT_SLOW){
        g->speed *= type==BT_FAST?1.3f:0.75f;
        if(g->speed>1.9f) g->speed=1.9f;
        if(g->speed<0.55f) g->speed=0.55f;
        float sp=bk_base_speed(g)*g->speed;
        for(int i=0;i<BK_MAXB;i++) if(g->b[i].on && !g->b[i].stuck) set_speed(&g->b[i],sp);
    }
}
static void explode(bk_t*g,int r0,int c0,bk_ball_t*ball){
    for(int r=r0-1;r<=r0+1;r++) for(int c=c0-1;c<=c0+1;c++){
        if(r<0||r>=BK_NR||c<0||c>=BK_NC) continue;
        int t=kill_brick(g,r,c,1); if(t>1) power(g,t-1,ball);
    }
}
/* one small step for one ball; returns 0 if it fell out */
static int ball_step(bk_t*g,bk_ball_t*b,float dt){
    b->x+=b->vx*dt; b->y+=b->vy*dt;
    if(b->x<BK_R){ b->x=BK_R; b->vx=fabsf(b->vx); }
    if(b->x>BK_W-BK_R){ b->x=BK_W-BK_R; b->vx=-fabsf(b->vx); }
    if(b->y<BK_R){ b->y=BK_R; b->vy=fabsf(b->vy); }
    if(b->y-BK_R>BK_H) return 0;
    /* paddle: bounce angle depends on where it hits (centre = straight up, edges = 60 deg) */
    if(b->vy>0 && b->y+BK_R>=BK_PY && b->y+BK_R<=BK_PY+BK_PH+10 &&
       b->x>=g->px-BK_R && b->x<=g->px+g->pw+BK_R){
        float off=(b->x-(g->px+g->pw/2.0f))/(g->pw/2.0f); if(off<-1)off=-1; if(off>1)off=1;
        /* + a little random spread, and never dead vertical: avoids endless straight loops */
        float spread = g->quiet>15 ? (rnd(41)-20)*0.017f : (rnd(9)-4)*0.02f;   /* explore if stuck on the last bricks */
        float a=off*1.05f + spread, sp=sqrtf(b->vx*b->vx+b->vy*b->vy);
        if(fabsf(a)<0.09f) a = a<0?-0.09f:0.09f;
        b->vx=sp*sinf(a); b->vy=-sp*cosf(a); b->y=BK_PY-BK_R;
    }
    /* bricks: only cells near the ball; resolve one hit per step */
    int c0=(int)((b->x-BK_X0)/48.0f), r0=(int)((b->y-BK_Y0)/BK_DY);
    for(int r=r0-1;r<=r0+1;r++) for(int c=c0-1;c<=c0+1;c++){
        if(r<0||r>=BK_NR||c<0||c>=BK_NC||g->br[r][c].hp<=0) continue;
        float bx=bk_brick_x(c), by=bk_brick_y(r);
        float cx=b->x<bx?bx:(b->x>bx+BK_BW?bx+BK_BW:b->x);
        float cy=b->y<by?by:(b->y>by+BK_BH?by+BK_BH:b->y);
        float dx=b->x-cx, dy=b->y-cy;
        if(dx*dx+dy*dy>BK_R*BK_R) continue;
        /* reflect on the axis of least penetration and push out */
        float ox=(BK_R+BK_BW/2.0f)-fabsf(bx+BK_BW/2.0f-b->x);
        float oy=(BK_R+BK_BH/2.0f)-fabsf(by+BK_BH/2.0f-b->y);
        if(ox<oy){ if(b->x<bx+BK_BW/2.0f){ b->vx=-fabsf(b->vx); b->x-=ox; } else { b->vx=fabsf(b->vx); b->x+=ox; } }
        else     { if(b->y<by+BK_BH/2.0f){ b->vy=-fabsf(b->vy); b->y-=oy; } else { b->vy=fabsf(b->vy); b->y+=oy; } }
        if(b->bomb){ b->bomb=0; explode(g,r,c,b); }
        else { int t=kill_brick(g,r,c,0); if(t>1) power(g,t-1,b); }
        min_vertical(b);
        return 1;
    }
    return 1;
}
void bk_step(bk_t*g,float dt){
    if(g->over) return;
    if(dt>0.05f) dt=0.05f;
    g->quiet+=dt;
    /* sub-step so a ball never moves more than ~3 px at once (no tunnelling) */
    float vmax=bk_base_speed(g)*g->speed*1.05f;
    int n=(int)(vmax*dt/3.0f)+1; float h=dt/n;
    for(int s=0;s<n;s++){
        for(int i=0;i<BK_MAXB;i++){ bk_ball_t*b=&g->b[i]; if(!b->on) continue;
            if(b->stuck){ b->x=g->px+g->pw/2.0f; b->y=BK_PY-BK_R; continue; }
            if(!ball_step(g,b,h)) b->on=0; }
        if(g->left<=0){   /* level clear: next layout, fresh serve */
            g->level++; build_level(g); serve(g); g->toast=100; return; }
    }
    if(!bk_balls(g)) g->over=1;
}
