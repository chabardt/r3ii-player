#ifndef R3_HISCORE_H
#define R3_HISCORE_H
/* Top-5 high-score table with dates, one file per game ("score dd/mm/yy" lines). */
#define HS_N 5
typedef struct { const char*file; int n; int s[HS_N]; char d[HS_N][12]; } hiscore_t;
void hs_load(hiscore_t*h);
int  hs_add(hiscore_t*h,int score);    /* insert + save; returns rank 0..4 or -1 */
void hs_format(const hiscore_t*h,int rank,int score,char*buf,int cap);   /* LVGL recolor text */
#endif
