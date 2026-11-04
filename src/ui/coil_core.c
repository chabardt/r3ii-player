/* Coil logic — see coil_core.h */
#include "coil_core.h"
#include <string.h>

static int rnd(coil_t*g,int n){ g->rng=g->rng*1103515245u+12345u; return (int)((g->rng>>16)%(unsigned)n); }
int coil_at(const coil_t*g,int x,int y){
    for(int i=0;i<g->len;i++){ cell_t c=coil_seg(g,i); if(c.x==x&&c.y==y) return 1; } return 0; }
static int free_cell(const coil_t*g,int x,int y){
    if(coil_at(g,x,y)) return 0;
    if(g->fruit.x==x&&g->fruit.y==y) return 0;
    if(g->gold_ticks>0&&g->gold.x==x&&g->gold.y==y) return 0;
    if(g->berry_on&&g->berry.x==x&&g->berry.y==y) return 0;
    return 1;
}
static int place(coil_t*g,cell_t*c){            /* random free cell; 0 if the board is full */
    for(int t=0;t<400;t++){ int x=rnd(g,CW), y=rnd(g,CH); if(free_cell(g,x,y)){ c->x=(int8_t)x; c->y=(int8_t)y; return 1; } }
    for(int y=0;y<CH;y++) for(int x=0;x<CW;x++) if(free_cell(g,x,y)){ c->x=(int8_t)x; c->y=(int8_t)y; return 1; }
    return 0;
}
void coil_new(coil_t*g,unsigned seed){
    memset(g,0,sizeof *g); g->rng=seed?seed:1; g->level=1; g->dir=DIR_RIGHT;
    g->len=4; g->head=3; for(int i=0;i<4;i++){ g->body[i].x=(int8_t)(CW/2-3+i); g->body[i].y=CH/2; }
    place(g,&g->fruit);
}
int coil_tick_ms(const coil_t*g){ int t=170-(g->level-1)*12; return t<70?70:t; }
void coil_turn(coil_t*g,int d){
    if(g->over || g->nq>=2) return;
    int last = g->nq ? g->next_dir[g->nq-1] : g->dir;
    if(d==last || (d+2)%4==last) return;        /* same way or straight back: ignore */
    g->next_dir[g->nq++]=d;
}
void coil_turn_rel(coil_t*g,int right){
    int last = g->nq ? g->next_dir[g->nq-1] : g->dir;
    coil_turn(g,(last+(right?1:3))%4);
}
void coil_step(coil_t*g){
    if(g->over) return;
    g->ate=F_NONE; g->ticks++;
    if(g->nq){ g->dir=g->next_dir[0]; g->next_dir[0]=g->next_dir[1]; g->nq--; }
    cell_t h=coil_seg(g,0); static const int DX[4]={0,1,0,-1}, DY[4]={-1,0,1,0};
    cell_t n={ (int8_t)(h.x+DX[g->dir]), (int8_t)(h.y+DY[g->dir]) };
    if(n.x<0||n.x>=CW||n.y<0||n.y>=CH){ g->over=1; return; }
    /* moving into the tail cell is fine when the tail moves away this step */
    int tail_moves = g->grow==0;
    for(int i=0;i<g->len-(tail_moves?1:0);i++){ cell_t c=coil_seg(g,i); if(c.x==n.x&&c.y==n.y){ g->over=1; return; } }
    g->head=(g->head+1)%CMAX; g->body[g->head]=n;
    if(g->grow>0){ g->grow--; g->len++; }
    if(n.x==g->fruit.x&&n.y==g->fruit.y){
        g->grow+=1; g->eaten++; g->score+=10*g->level; g->ate=F_FRUIT;
        if(g->eaten%5==0) g->level++;
        if(!place(g,&g->fruit)){ g->over=1; return; }
        if(g->gold_ticks<=0 && rnd(g,100)<22 && place(g,&g->gold)) g->gold_ticks=45;
        if(!g->berry_on && g->len>=14 && rnd(g,100)<25 && place(g,&g->berry)) g->berry_on=1;
    }
    if(g->gold_ticks>0){
        if(n.x==g->gold.x&&n.y==g->gold.y){ g->grow+=2; g->score+=50*g->level; g->gold_ticks=0; g->ate=F_GOLD; }
        else g->gold_ticks--; }
    if(g->berry_on && n.x==g->berry.x&&n.y==g->berry.y){
        g->berry_on=0; g->ate=F_BERRY; g->score+=5*g->level;
        int cut=3; if(g->len-cut<4) cut=g->len-4; if(cut>0) g->len-=cut; }
}
