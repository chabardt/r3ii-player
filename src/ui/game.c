/* game.c — "Blockfall", an original falling-block puzzle for the R3 (480x720, touch).
 *
 * Pieces are generated at random (3..5 connected cells, weighted towards 4) instead of
 * a fixed set, each with its own colour. Full rows clear; a row made of a single colour
 * scores double. Speed rises every 10 rows.
 *
 * Touch on the board: tap = rotate, drag sideways = move (one cell per 32 px).
 * Keys: next = drop, prev = turn, play = pause.
 * Top-5 high scores (with dates) in /usr/data/r3_blockfall, shown at game over. */
#include "game.h"
#include "r3_tick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLS 10
#define ROWS 18
#define CELL 32
#define BX 8
#define BY 62
#define BEST_FILE "/usr/data/r3_blockfall"

static const uint32_t PAL[]={0xE8731C,0xF2B33D,0x4FB0A5,0x6C8CD5,0xC75B8F,0x9BC15B,0xE04848};
#define NPAL (int)(sizeof PAL/sizeof PAL[0])

typedef struct { int n; int x[5],y[5]; int col; } piece_t;

static unsigned char board[ROWS][COLS];   /* 0 = empty, else palette index+1 */
static piece_t cur, nxt; static int px,py;
static int score,best,rows_done,level,over,paused,piece_no;
static uint32_t last_fall;
static lv_obj_t *g_scr, *g_board, *g_next, *g_score, *g_info, *g_over, *g_over_lbl, *g_pause;
#define NHS 5
static int hs[NHS], nhs; static char hsd[NHS][12];   /* high-score table, best first */
static lv_timer_t *g_tmr;
static void (*g_exit)(void);

/* ---- pieces ---- */
static void normalize(piece_t*p){
    int mx=99,my=99; for(int i=0;i<p->n;i++){ if(p->x[i]<mx)mx=p->x[i]; if(p->y[i]<my)my=p->y[i]; }
    for(int i=0;i<p->n;i++){ p->x[i]-=mx; p->y[i]-=my; }
}
static int has(const piece_t*p,int x,int y){ for(int i=0;i<p->n;i++) if(p->x[i]==x&&p->y[i]==y) return 1; return 0; }
static void gen(piece_t*p){
    int r=rand()%10; p->n = r<2?3 : r<8?4 : 5;
    p->x[0]=0; p->y[0]=0; int k=1;
    while(k<p->n){   /* grow: attach a new cell next to a random existing one */
        int b=rand()%k, d=rand()%4, x=p->x[b]+(d==0)-(d==1), y=p->y[b]+(d==2)-(d==3);
        if(!has(p,x,y)){ p->x[k]=x; p->y[k]=y; k++; } }
    normalize(p); p->col=rand()%NPAL;
}
static int width(const piece_t*p){ int w=0; for(int i=0;i<p->n;i++) if(p->x[i]+1>w)w=p->x[i]+1; return w; }
static int fits(const piece_t*p,int ox,int oy){
    for(int i=0;i<p->n;i++){ int x=ox+p->x[i], y=oy+p->y[i];
        if(x<0||x>=COLS||y>=ROWS) return 0;
        if(y>=0 && board[y][x]) return 0; }
    return 1;
}
static void rotate(void){
    piece_t r=cur; for(int i=0;i<r.n;i++){ int x=r.x[i]; r.x[i]=-r.y[i]; r.y[i]=x; } normalize(&r);
    static const int kick[]={0,-1,1,-2,2};
    for(int k=0;k<5;k++) if(fits(&r,px+kick[k],py)){ cur=r; px+=kick[k]; return; }
}

/* ---- game flow ---- */
static void hs_load(void){
    nhs=0; FILE*f=fopen(BEST_FILE,"r"); if(!f) return; char l[64];
    while(nhs<NHS && fgets(l,sizeof l,f)){ int v; char d[12]="-";
        int k=sscanf(l,"%d %11s",&v,d); if(k<1) continue;      /* old file: a lone best score */
        hs[nhs]=v; snprintf(hsd[nhs],12,"%s",d); nhs++; }
    fclose(f); best = nhs?hs[0]:0;
}
static void hs_save(void){ FILE*f=fopen(BEST_FILE,"w"); if(!f) return;
    for(int i=0;i<nhs;i++) fprintf(f,"%d %s\n",hs[i],hsd[i]); fclose(f); }
/* game over: enter the score in the table, show the table with this run highlighted */
static void game_end(void){
    over=1; int rank=-1;
    if(score>0){
        for(rank=0;rank<nhs && hs[rank]>=score;rank++);
        if(rank<NHS){
            for(int i=(nhs<NHS?nhs:NHS-1);i>rank;i--){ hs[i]=hs[i-1]; memcpy(hsd[i],hsd[i-1],12); }
            hs[rank]=score; time_t t=time(0); struct tm tm; localtime_r(&t,&tm);
            strftime(hsd[rank],12,"%d/%m/%y",&tm); if(nhs<NHS) nhs++; hs_save();
        } else rank=-1;
    }
    best = nhs?hs[0]:0;
    char b[512]; int n=snprintf(b,sizeof b,"GAME OVER\n%s%d\n\nHIGH SCORES\n",rank==0?"NEW BEST  ":"",score);
    for(int i=0;i<nhs;i++)
        n+=snprintf(b+n,sizeof b-n,i==rank?"#E8731C %d.  %6d   %s#\n":"%d.  %6d   %s\n",i+1,hs[i],hsd[i]);
    if(!nhs) n+=snprintf(b+n,sizeof b-n,"-\n");
    snprintf(b+n,sizeof b-n,"\ntap to play again");
    lv_label_set_text(g_over_lbl,b);
    lv_obj_clear_flag(g_over,LV_OBJ_FLAG_HIDDEN);
}
static void update_labels(void){
    char b[96];
    snprintf(b,sizeof b,"%d",score); lv_label_set_text(g_score,b);
    snprintf(b,sizeof b,"ROWS %d\nLEVEL %d\nBEST %d",rows_done,level,best); lv_label_set_text(g_info,b);
}
static void spawn(void){
    cur=nxt; gen(&nxt); px=(COLS-width(&cur))/2; py=-1; piece_no++;
    if(!fits(&cur,px,py+1)) game_end();
    lv_obj_invalidate(g_next);
}
static void clear_rows(void){
    int cleared=0, mono=0;
    for(int y=ROWS-1;y>=0;y--){
        int full=1, same=1; for(int x=0;x<COLS;x++){ if(!board[y][x]) full=0; if(board[y][x]!=board[y][0]) same=0; }
        if(!full) continue;
        cleared++; if(same) mono++;
        memmove(&board[1][0],&board[0][0],(size_t)y*COLS); memset(board[0],0,COLS); y++;   /* recheck row */
    }
    if(!cleared) return;
    static const int base[]={0,100,300,500,800,1200};
    int pts=base[cleared>5?5:cleared]*level + mono*base[1]*level;   /* single-colour row: double */
    score+=pts; rows_done+=cleared; level=1+rows_done/10;
}
static void lock(void){
    for(int i=0;i<cur.n;i++){ int y=py+cur.y[i], x=px+cur.x[i];
        if(y<0){ over=1; continue; } board[y][x]=(unsigned char)(cur.col+1); }
    clear_rows();
    if(over) game_end();
    else spawn();
    update_labels();
}
static void step_down(void){ if(fits(&cur,px,py+1)) py++; else lock(); }
static void hard_drop(void){ int d=0; while(fits(&cur,px,py+1)){ py++; d++; } score+=d; lock(); }
static void move(int dx){ if(fits(&cur,px+dx,py)) px+=dx; }
static void reset(void){
    memset(board,0,sizeof board); score=0; rows_done=0; level=1; over=0; paused=0; if(g_pause) lv_label_set_text(g_pause,"");
    gen(&nxt); spawn(); last_fall=r3_tick_ms();
    lv_obj_add_flag(g_over,LV_OBJ_FLAG_HIDDEN); update_labels();
}
static int fall_ms(void){ int t=720-(level-1)*60; return t<90?90:t; }

/* ---- drawing ---- */
static void cell_rect(lv_draw_ctx_t*dc,int x0,int y0,int sz,uint32_t col,lv_opa_t opa){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color=lv_color_hex(col); d.bg_opa=opa; d.radius=3;
    d.border_color=lv_color_hex(0x000000); d.border_width=1; d.border_opa=LV_OPA_50;
    lv_area_t a={x0+1,y0+1,x0+sz-2,y0+sz-2}; lv_draw_rect(dc,&d,&a);
}
static void board_draw_cb(lv_event_t*e){
    lv_draw_ctx_t*dc=lv_event_get_draw_ctx(e); lv_obj_t*o=lv_event_get_target(e);
    lv_area_t c; lv_obj_get_coords(o,&c);
    for(int y=0;y<ROWS;y++) for(int x=0;x<COLS;x++) if(board[y][x])
        cell_rect(dc,c.x1+x*CELL,c.y1+y*CELL,CELL,PAL[board[y][x]-1],LV_OPA_COVER);
    if(!over){
        int gy=py; while(fits(&cur,px,gy+1)) gy++;     /* ghost: where it would land */
        for(int i=0;i<cur.n;i++){ int y=gy+cur.y[i]; if(y>=0)
            cell_rect(dc,c.x1+(px+cur.x[i])*CELL,c.y1+y*CELL,CELL,PAL[cur.col],LV_OPA_20); }
        for(int i=0;i<cur.n;i++){ int y=py+cur.y[i]; if(y>=0)
            cell_rect(dc,c.x1+(px+cur.x[i])*CELL,c.y1+y*CELL,CELL,PAL[cur.col],LV_OPA_COVER); }
    }
}
static void next_draw_cb(lv_event_t*e){
    lv_draw_ctx_t*dc=lv_event_get_draw_ctx(e); lv_obj_t*o=lv_event_get_target(e);
    lv_area_t c; lv_obj_get_coords(o,&c); const int s=22;
    int w=width(&nxt), h=0; for(int i=0;i<nxt.n;i++) if(nxt.y[i]+1>h)h=nxt.y[i]+1;
    int ox=c.x1+(lv_area_get_width(&c)-w*s)/2, oy=c.y1+(lv_area_get_height(&c)-h*s)/2;
    for(int i=0;i<nxt.n;i++) cell_rect(dc,ox+nxt.x[i]*s,oy+nxt.y[i]*s,s,PAL[nxt.col],LV_OPA_COVER);
}

/* ---- input ---- */
static int drag_x, drag_y, drag_moved;
static void board_input_cb(lv_event_t*e){
    lv_event_code_t code=lv_event_get_code(e);
    if(over){ if(code==LV_EVENT_CLICKED) reset(); return; }
    if(code==LV_EVENT_PRESSED){ drag_x=drag_y=0; drag_moved=0; return; }
    if(code==LV_EVENT_PRESSING){ lv_point_t v; lv_indev_get_vect(lv_indev_get_act(),&v);
        drag_x+=v.x; drag_y+=v.y;
        while(drag_x>=CELL){ move(+1); drag_x-=CELL; drag_moved=1; }
        while(drag_x<=-CELL){ move(-1); drag_x+=CELL; drag_moved=1; }
        lv_obj_invalidate(g_board); return; }
    if(code==LV_EVENT_RELEASED){
        if(paused){ paused=0; lv_label_set_text(g_pause,""); last_fall=r3_tick_ms(); }   /* tap resumes */
        else if(!drag_moved) rotate();   /* every tap turns at once (fast spins) */
        lv_obj_invalidate(g_board); }
}
/* keys: next = drop, prev = turn, play = pause/resume (or new game after game over) */
int game_key(int code){
    if(!g_scr) return 0;
    if(over){ if(code==164||code==163) reset(); return 1; }
    if(code==164){ paused=!paused; lv_label_set_text(g_pause, paused?"PAUSED":"");
        if(!paused) last_fall=r3_tick_ms(); return 1; }
    if(paused) return 1;
    if(code==163) hard_drop(); else if(code==165) rotate(); else return 0;
    lv_obj_invalidate(g_board); return 1;
}
int game_running(void){ return g_scr && !over; }

static void tick_cb(lv_timer_t*t){ (void)t;
    if(over||paused) return; uint32_t now=r3_tick_ms();
    if(now-last_fall>=(uint32_t)fall_ms()){ last_fall=now; step_down(); lv_obj_invalidate(g_board); }
}
/* ---- save / resume: the running game is kept when you leave (or power off) ---- */
#define SAVE_FILE "/usr/data/r3_blockfall.sav"
#define SAVE_MAGIC 0x42465331u   /* "BFS1" */
typedef struct { uint32_t magic; unsigned char board[ROWS][COLS]; piece_t cur,nxt;
    int px,py,score,rows_done,level,piece_no; } bf_save_t;
void game_save(void){
    if(!g_scr) return;
    if(over){ unlink(SAVE_FILE); return; }
    bf_save_t s; memset(&s,0,sizeof s); s.magic=SAVE_MAGIC; memcpy(s.board,board,sizeof board);
    s.cur=cur; s.nxt=nxt; s.px=px; s.py=py; s.score=score; s.rows_done=rows_done; s.level=level; s.piece_no=piece_no;
    FILE*f=fopen(SAVE_FILE,"wb"); if(f){ fwrite(&s,sizeof s,1,f); fclose(f); }
}
static int load_save(bf_save_t*s){ FILE*f=fopen(SAVE_FILE,"rb"); if(!f) return 0;
    int ok=fread(s,sizeof *s,1,f)==1 && s->magic==SAVE_MAGIC; fclose(f); return ok; }
int game_has_save(int*score_out){ bf_save_t s; if(!load_save(&s)) return 0;
    if(score_out) *score_out=s.score; return 1; }

static void back_cb(lv_event_t*e){ (void)e; if(g_exit) g_exit(); }
static void scr_del_cb(lv_event_t*e){ (void)e; game_save();
    if(g_tmr){ lv_timer_del(g_tmr); g_tmr=0; } g_scr=0; }

lv_obj_t* game_create(void (*on_exit)(void),int resume){
    g_exit=on_exit; srand((unsigned)time(0)^r3_tick_ms());
    hs_load();
    lv_obj_t*scr=lv_obj_create(NULL); g_scr=scr;
    lv_obj_set_style_bg_color(scr,lv_color_hex(0x0A0A0A),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr,scr_del_cb,LV_EVENT_DELETE,NULL);
    /* header */
    lv_obj_t*bk=lv_btn_create(scr); lv_obj_set_size(bk,46,40); lv_obj_set_pos(bk,8,8);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x1A1510),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(0xE8731C),0);
    lv_obj_add_event_cb(bk,back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl,lv_color_hex(0xE8731C),0); lv_obj_center(bl);
    lv_obj_t*t=lv_label_create(scr); lv_label_set_text(t,"BLOCKFALL");
    lv_obj_set_style_text_color(t,lv_color_hex(0xE8731C),0); lv_obj_set_style_text_font(t,&lv_font_montserrat_20,0);
    lv_obj_set_pos(t,66,16);
    /* board */
    g_board=lv_obj_create(scr); lv_obj_set_size(g_board,COLS*CELL,ROWS*CELL); lv_obj_set_pos(g_board,BX,BY);
    lv_obj_set_style_bg_color(g_board,lv_color_hex(0x15110D),0); lv_obj_set_style_radius(g_board,0,0);
    lv_obj_set_style_border_width(g_board,0,0); lv_obj_set_style_pad_all(g_board,0,0);
    lv_obj_set_style_outline_color(g_board,lv_color_hex(0x8A6A4A),0); lv_obj_set_style_outline_width(g_board,2,0);
    lv_obj_clear_flag(g_board,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_board,board_draw_cb,LV_EVENT_DRAW_MAIN_END,NULL);
    lv_obj_add_event_cb(g_board,board_input_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(g_board,board_input_cb,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(g_board,board_input_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(g_board,board_input_cb,LV_EVENT_CLICKED,NULL);
    /* side panel */
    int sx=BX+COLS*CELL+14, sw=480-sx-8;
    lv_obj_t*nl=lv_label_create(scr); lv_label_set_text(nl,"NEXT");
    lv_obj_set_style_text_color(nl,lv_color_hex(0x8A6A4A),0); lv_obj_set_pos(nl,sx,BY);
    g_next=lv_obj_create(scr); lv_obj_set_size(g_next,sw,sw); lv_obj_set_pos(g_next,sx,BY+24);
    lv_obj_set_style_bg_color(g_next,lv_color_hex(0x15110D),0); lv_obj_set_style_radius(g_next,0,0);
    lv_obj_set_style_border_width(g_next,0,0); lv_obj_clear_flag(g_next,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_next,next_draw_cb,LV_EVENT_DRAW_MAIN_END,NULL);
    lv_obj_t*sl=lv_label_create(scr); lv_label_set_text(sl,"SCORE");
    lv_obj_set_style_text_color(sl,lv_color_hex(0x8A6A4A),0); lv_obj_set_pos(sl,sx,BY+sw+44);
    g_score=lv_label_create(scr); lv_obj_set_style_text_font(g_score,&lv_font_montserrat_28,0);
    lv_obj_set_style_text_color(g_score,lv_color_hex(0xF4E4D0),0); lv_obj_set_pos(g_score,sx,BY+sw+66);
    g_info=lv_label_create(scr); lv_obj_set_style_text_color(g_info,lv_color_hex(0x8A6A4A),0);
    lv_obj_set_style_text_line_space(g_info,6,0); lv_obj_set_pos(g_info,sx,BY+sw+120);
    lv_obj_t*help=lv_label_create(scr);
    lv_label_set_text(help,"tap: turn\ndrag: move\n\nkeys:\nnext: drop\nprev: turn\nplay: pause");
    lv_obj_set_style_text_color(help,lv_color_hex(0x5A4632),0); lv_obj_set_style_text_font(help,&lv_font_montserrat_12,0);
    lv_obj_set_pos(help,sx,BY+ROWS*CELL-150);
    g_pause=lv_label_create(scr); lv_label_set_text(g_pause,"");
    lv_obj_set_style_text_font(g_pause,&lv_font_montserrat_28,0);
    lv_obj_set_style_text_color(g_pause,lv_color_hex(0xE8731C),0);
    lv_obj_align(g_pause,LV_ALIGN_TOP_LEFT,BX+COLS*CELL/2-58,BY+ROWS*CELL/2-20);
    /* game over */
    g_over=lv_obj_create(scr); lv_obj_set_size(g_over,COLS*CELL-24,330); lv_obj_set_pos(g_over,BX+12,BY+110);
    lv_obj_set_style_bg_color(g_over,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(g_over,LV_OPA_80,0);
    lv_obj_set_style_border_color(g_over,lv_color_hex(0xE8731C),0); lv_obj_set_style_border_width(g_over,2,0);
    lv_obj_set_style_radius(g_over,0,0); lv_obj_clear_flag(g_over,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    g_over_lbl=lv_label_create(g_over); lv_label_set_text(g_over_lbl,"");
    lv_label_set_recolor(g_over_lbl,true);
    lv_obj_set_style_text_align(g_over_lbl,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_line_space(g_over_lbl,4,0);
    lv_obj_set_style_text_color(g_over_lbl,lv_color_hex(0xF4E4D0),0); lv_obj_center(g_over_lbl);
    reset();
    bf_save_t sv;
    if(resume && load_save(&sv)){   /* continue where we left off, paused until a tap */
        memcpy(board,sv.board,sizeof board); cur=sv.cur; nxt=sv.nxt; px=sv.px; py=sv.py;
        score=sv.score; rows_done=sv.rows_done; level=sv.level; piece_no=sv.piece_no;
        paused=1; lv_label_set_text(g_pause,"PAUSED"); update_labels();
    }
    unlink(SAVE_FILE);              /* the live game owns the state now (saved again on leave) */
    g_tmr=lv_timer_create(tick_cb,20,NULL);
    return scr;
}
