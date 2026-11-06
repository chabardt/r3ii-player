#ifndef R3_SAPPER_CORE_H
#define R3_SAPPER_CORE_H
/* Sapper — minesweeper-style logic (no UI). Clear every safe square of a hidden minefield;
 * numbers count the mines around a square. Mines are laid on the first dig, never under it
 * or its neighbours, so the first dig always opens an area. */
#include <stdint.h>
#define SP_MAXC 16
#define SP_MAXR 21
enum { SP_EASY, SP_MEDIUM, SP_HARD, SP_NLEVELS };
typedef struct {
    int level, cols, rows, mines;
    uint8_t mine[SP_MAXR][SP_MAXC], open[SP_MAXR][SP_MAXC], flag[SP_MAXR][SP_MAXC], adj[SP_MAXR][SP_MAXC];
    int laid, opened, flags, over, won, boom_x, boom_y;
    uint32_t rng;
} sapper_t;
void sp_new(sapper_t*g,int level,unsigned seed);
int  sp_dig(sapper_t*g,int x,int y);       /* open a square (flood-fills empty ones); returns squares opened */
int  sp_chord(sapper_t*g,int x,int y);     /* on an open number whose flags are all placed: open the rest around it */
void sp_flag(sapper_t*g,int x,int y);      /* toggle a flag on a closed square */
int  sp_score(const sapper_t*g,int seconds);   /* points for a win: bigger boards and faster times score more */
void sp_level_dims(int level,int*cols,int*rows,int*mines);
#endif
