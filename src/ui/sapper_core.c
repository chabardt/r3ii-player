/* Sapper logic — see sapper_core.h */
#include "sapper_core.h"
#include <string.h>

static const int DIMS[SP_NLEVELS][3]={ {9,12,12}, {12,16,30}, {16,21,60} };   /* cols, rows, mines */
void sp_level_dims(int level,int*c,int*r,int*m){
    if(level<0||level>=SP_NLEVELS) level=0; *c=DIMS[level][0]; *r=DIMS[level][1]; *m=DIMS[level][2]; }
static int rnd(sapper_t*g,int n){ g->rng=g->rng*1103515245u+12345u; return (int)((g->rng>>16)%(unsigned)n); }
static int in(const sapper_t*g,int x,int y){ return x>=0&&y>=0&&x<g->cols&&y<g->rows; }

void sp_new(sapper_t*g,int level,unsigned seed){
    memset(g,0,sizeof *g); if(level<0||level>=SP_NLEVELS) level=0;
    g->level=level; sp_level_dims(level,&g->cols,&g->rows,&g->mines); g->rng=seed?seed:1; g->boom_x=g->boom_y=-1; }

static void lay(sapper_t*g,int sx,int sy){      /* mines everywhere but the first dig and its neighbours */
    int left=g->mines;
    while(left>0){ int x=rnd(g,g->cols), y=rnd(g,g->rows);
        if(g->mine[y][x] || (x>=sx-1&&x<=sx+1&&y>=sy-1&&y<=sy+1)) continue;
        g->mine[y][x]=1; left--; }
    for(int y=0;y<g->rows;y++) for(int x=0;x<g->cols;x++){ int n=0;
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) if((dx||dy)&&in(g,x+dx,y+dy)&&g->mine[y+dy][x+dx]) n++;
        g->adj[y][x]=(uint8_t)n; }
    g->laid=1;
}
static void check_win(sapper_t*g){
    if(g->over || g->opened != g->cols*g->rows-g->mines) return;
    g->over=g->won=1;
    for(int y=0;y<g->rows;y++) for(int x=0;x<g->cols;x++) if(g->mine[y][x]) g->flag[y][x]=1;   /* show them flagged */
    g->flags=g->mines;
}
int sp_dig(sapper_t*g,int x,int y){
    if(g->over||!in(g,x,y)||g->open[y][x]||g->flag[y][x]) return 0;
    if(!g->laid) lay(g,x,y);
    if(g->mine[y][x]){ g->open[y][x]=1; g->over=1; g->boom_x=x; g->boom_y=y; return 0; }
    /* flood fill from (x,y): empty squares open their neighbours (explicit stack, no recursion) */
    static int16_t st[SP_MAXR*SP_MAXC][2]; int sp=0, n=0;
    st[sp][0]=(int16_t)x; st[sp][1]=(int16_t)y; sp++; g->open[y][x]=1; n++;
    while(sp){ sp--; int cx=st[sp][0], cy=st[sp][1];
        if(g->adj[cy][cx]) continue;
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){ int nx=cx+dx, ny=cy+dy;
            if(!in(g,nx,ny)||g->open[ny][nx]||g->flag[ny][nx]||g->mine[ny][nx]) continue;
            g->open[ny][nx]=1; n++; st[sp][0]=(int16_t)nx; st[sp][1]=(int16_t)ny; sp++; } }
    g->opened+=n; check_win(g); return n;
}
int sp_chord(sapper_t*g,int x,int y){
    if(g->over||!in(g,x,y)||!g->open[y][x]||!g->adj[y][x]) return 0;
    int f=0; for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) if(in(g,x+dx,y+dy)&&g->flag[y+dy][x+dx]) f++;
    if(f!=g->adj[y][x]) return 0;
    int n=0; for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){ int nx=x+dx, ny=y+dy;
        if(!in(g,nx,ny)||g->open[ny][nx]||g->flag[ny][nx]) continue;
        if(g->mine[ny][nx]){ g->open[ny][nx]=1; g->over=1; g->boom_x=nx; g->boom_y=ny; return n; }   /* a wrong flag */
        n+=sp_dig(g,nx,ny); }
    return n;
}
void sp_flag(sapper_t*g,int x,int y){
    if(g->over||!in(g,x,y)||g->open[y][x]) return;
    g->flag[y][x]^=1; g->flags+=g->flag[y][x]?1:-1; }
int sp_score(const sapper_t*g,int seconds){
    if(!g->won) return 0; if(seconds<0) seconds=0;
    return (int)((long)g->mines*g->mines*100/(seconds+5)) + g->mines*10; }
