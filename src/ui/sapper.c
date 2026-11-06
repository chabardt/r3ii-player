/* sapper.c — Sapper screen (LVGL) on top of sapper_core.c.
 * Tap = dig (or flag, in FLAG mode), hold = flag, tap a number with all its flags = open around it.
 * Keys: play toggles DIG / FLAG. Controller: d-pad moves a cursor, A digs, B flags, + new game.
 * The clock starts on the first dig. Wins go into a top-5 table (bigger boards, faster = more). */
#include "sapper.h"
#include "sapper_core.h"
#include "hiscore.h"
#include "pad.h"
#include "r3_tick.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BY 54
#define SAVE "/usr/data/r3_sapper.sav"
#define LEVEL_FILE "/usr/data/r3_sapper_level"
#define MAGIC 0x53505231u   /* "SPR1" */
static sapper_t G; static int s_flagmode, s_cx, s_cy, s_cursor; static uint32_t s_elapsed, s_last;
static hiscore_t HS={ "/usr/data/r3_sapper" };
static lv_obj_t *s_scr,*s_board,*s_title,*s_left,*s_clock,*s_mode,*s_mode_lbl,*s_over,*s_over_lbl;
static lv_timer_t *s_tmr; static void (*s_exit)(void);
static int s_cs, s_ox;                     /* cell size, board x offset */
static int s_press_x=-1, s_press_y=-1, s_held, s_rank=-1, s_score;
static const char *LEVEL_NAMES[SP_NLEVELS]={"Easy","Medium","Hard"};
static const uint32_t NUM_COL[9]={0,0x6C8CD5,0x7BB662,0xE04848,0xA77BE0,0xD08A3A,0x3AB0B0,0xD0D0D0,0x9A9A9A};

static int level_load(void){ int l=0; FILE*f=fopen(LEVEL_FILE,"r"); if(f){ if(fscanf(f,"%d",&l)!=1) l=0; fclose(f); } return l<0||l>=SP_NLEVELS?0:l; }
static void level_save(int l){ FILE*f=fopen(LEVEL_FILE,"w"); if(f){ fprintf(f,"%d\n",l); fclose(f); } }

/* ---- drawing ---- */
static void rect(lv_draw_ctx_t*dc,int x1,int y1,int x2,int y2,uint32_t c,int r){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color=lv_color_hex(c); d.radius=r;
    lv_area_t a={x1,y1,x2,y2}; lv_draw_rect(dc,&d,&a); }
static void text(lv_draw_ctx_t*dc,int x,int y,int w,int h,const char*t,uint32_t c,const lv_font_t*f){
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d); d.color=lv_color_hex(c); d.font=f; d.align=LV_TEXT_ALIGN_CENTER;
    int fh=lv_font_get_line_height(f); lv_area_t a={x,y+(h-fh)/2,x+w-1,y+(h-fh)/2+fh}; lv_draw_label(dc,&d,&a,t,NULL); }
static void draw_flag(lv_draw_ctx_t*dc,int x,int y,int cs){
    int px=x+cs*2/5, top=y+cs/5, bot=y+cs*4/5, ph=cs*2/5, pw=cs*2/5, pole=cs/16>1?cs/16:1;
    rect(dc,px,top,px+pole,bot,0xF4E4D0,0);                                       /* pole */
    for(int i=0;i<ph;i++){ int w=pw*(i<ph/2?i:ph-1-i)*2/ph;                       /* pennant: triangle pointing right */
        if(w>0) rect(dc,px+pole+1,top+i,px+pole+w,top+i,0xE8731C,0); }
    rect(dc,x+cs/4,bot,x+cs*3/4,bot+1+cs/24,0xF4E4D0,1); }                       /* base */
static void draw_mine(lv_draw_ctx_t*dc,int x,int y,int cs){
    int c=cs/2, r=cs/4;
    rect(dc,x+c-r-cs/10,y+c-1,x+c+r+cs/10,y+c+1,0x0A0A0A,0); rect(dc,x+c-1,y+c-r-cs/10,x+c+1,y+c+r+cs/10,0x0A0A0A,0);
    rect(dc,x+c-r,y+c-r,x+c+r,y+c+r,0x0A0A0A,LV_RADIUS_CIRCLE);
    rect(dc,x+c-r/2,y+c-r/2,x+c-r/2+cs/12+1,y+c-r/2+cs/12+1,0xF4E4D0,LV_RADIUS_CIRCLE); }
static void board_draw_cb(lv_event_t*e){
    lv_draw_ctx_t*dc=lv_event_get_draw_ctx(e); lv_area_t a; lv_obj_get_coords(lv_event_get_target(e),&a);
    int cs=s_cs; const lv_font_t*f= cs>=48?&lv_font_montserrat_28:cs>=36?&lv_font_montserrat_20:&lv_font_montserrat_16;
    for(int y=0;y<G.rows;y++) for(int x=0;x<G.cols;x++){
        int px=a.x1+x*cs, py=a.y1+y*cs;
        if(G.open[y][x]){
            int boom = G.boom_x==x && G.boom_y==y;
            rect(dc,px,py,px+cs-1,py+cs-1,boom?0xB8321E:0x15110D,0);
            if(G.mine[y][x]) draw_mine(dc,px,py,cs);
            else if(G.adj[y][x]){ char t[2]={(char)('0'+G.adj[y][x]),0}; text(dc,px,py,cs,cs,t,NUM_COL[G.adj[y][x]],f); }
        } else {
            int lost = G.over && !G.won;
            rect(dc,px+1,py+1,px+cs-2,py+cs-2,0x3A2C1F,cs/8);
            rect(dc,px+2,py+2,px+cs-3,py+cs-4,0x2A2017,cs/8);                     /* raised tile */
            if(G.flag[y][x]){ draw_flag(dc,px,py,cs);
                if(lost && !G.mine[y][x]) text(dc,px,py,cs,cs,"x",0xE04848,f); }    /* wrong flag */
            else if(lost && G.mine[y][x]) draw_mine(dc,px,py,cs);
        } }
    if(s_cursor && !G.over){ int px=a.x1+s_cx*cs, py=a.y1+s_cy*cs;
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_opa=LV_OPA_TRANSP; d.border_color=lv_color_hex(0xE8731C);
        d.border_width=3; d.radius=4; lv_area_t r={px,py,px+cs-1,py+cs-1}; lv_draw_rect(dc,&d,&r); }
}
/* ---- HUD ---- */
static void hud(void){
    char b[32]; snprintf(b,sizeof b,"%d",G.mines-G.flags); lv_label_set_text(s_left,b);
    unsigned s=s_elapsed/1000; snprintf(b,sizeof b,"%u:%02u",s/60,s%60); lv_label_set_text(s_clock,b);
    lv_label_set_text(s_mode_lbl,s_flagmode?"FLAG":"DIG");
    lv_obj_set_style_bg_color(s_mode,lv_color_hex(s_flagmode?0xE8731C:0x1A1510),0);
    lv_obj_set_style_text_color(s_mode_lbl,lv_color_hex(s_flagmode?0x0A0A0A:0xE8731C),0);
    lv_label_set_text(s_title, !G.over?"SAPPER":G.won?"CLEARED!":"BOOM!"); }
static void redraw(void){ hud(); lv_obj_invalidate(s_board); }

/* ---- game flow ---- */
static void finish(void){
    if(G.won){ s_score=sp_score(&G,(int)(s_elapsed/1000)); s_rank=hs_add(&HS,s_score); }
    else { s_score=0; s_rank=-1; }
    unlink(SAVE); redraw(); }
static void layout(void){
    int cw=480/G.cols, ch=(720-BY-6)/G.rows; s_cs=cw<ch?cw:ch; s_ox=(480-s_cs*G.cols)/2;
    lv_obj_set_size(s_board,s_cs*G.cols,s_cs*G.rows); lv_obj_set_pos(s_board,s_ox,BY+2); }
static void new_game(int level){
    sp_new(&G,level,(unsigned)time(0)^r3_tick_ms()); level_save(level);
    s_elapsed=0; s_cx=G.cols/2; s_cy=G.rows/2; s_rank=-1; s_score=0;
    lv_obj_add_flag(s_over,LV_OBJ_FLAG_HIDDEN); layout(); redraw(); }
static void level_btn_cb(lv_event_t*e){ new_game((int)(long)lv_event_get_user_data(e)); }
/* end panel / chooser: scores (after a game) + a button per board size */
static void show_chooser(void){
    static char b[640]; int n=0;
    if(G.over){
        unsigned s=s_elapsed/1000;
        if(G.won) n+=snprintf(b+n,sizeof b-n,"%sCLEARED in %u:%02u\n%d points\n\n",s_rank==0?"NEW BEST  ":"",s/60,s%60,s_score);
        else n+=snprintf(b+n,sizeof b-n,"BOOM - %d of %d cleared\n\n",G.opened,G.cols*G.rows-G.mines);
        n+=snprintf(b+n,sizeof b-n,"HIGH SCORES\n");
        for(int i=0;i<HS.n && n<(int)sizeof b;i++)
            n+=snprintf(b+n,sizeof b-n,i==s_rank?"#E8731C %d.  %6d   %s#\n":"%d.  %6d   %s\n",i+1,HS.s[i],HS.d[i]);
        if(!HS.n && n<(int)sizeof b) n+=snprintf(b+n,sizeof b-n,"-\n");
        if(n<(int)sizeof b) snprintf(b+n,sizeof b-n,"\nnew game:");
    } else snprintf(b,sizeof b,"SAPPER\nclear the field without\ndigging up a mine\n\ntap: dig   hold: flag\ntap a number to open around it\n\nchoose a field:");
    lv_label_set_text(s_over_lbl,b); lv_obj_clear_flag(s_over,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s_over); }
static void act(int x,int y,int flag){            /* dig / flag / chord at a square */
    if(G.over) return;
    int was_laid=G.laid;
    if(flag){ if(!G.open[y][x]) sp_flag(&G,x,y); }
    else if(G.open[y][x]) sp_chord(&G,x,y);
    else sp_dig(&G,x,y);
    if(!was_laid && G.laid) s_last=r3_tick_ms();
    if(G.over) finish(); else redraw(); }
static int cell_at(lv_point_t p,int*x,int*y){ lv_area_t a; lv_obj_get_coords(s_board,&a);
    *x=(p.x-a.x1)/s_cs; *y=(p.y-a.y1)/s_cs; return p.x>=a.x1 && p.y>=a.y1 && *x<G.cols && *y<G.rows; }
static void board_input_cb(lv_event_t*e){
    lv_event_code_t code=lv_event_get_code(e); lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p); int x,y;
    if(G.over){ if(code==LV_EVENT_SHORT_CLICKED) show_chooser(); return; }
    if(code==LV_EVENT_PRESSED){ s_held=0; s_cursor=0; if(cell_at(p,&x,&y)){ s_press_x=x; s_press_y=y; } else s_press_x=-1; return; }
    if(code==LV_EVENT_LONG_PRESSED){ s_held=1; if(s_press_x>=0 && cell_at(p,&x,&y) && x==s_press_x && y==s_press_y) act(x,y,1); return; }
    if(code==LV_EVENT_SHORT_CLICKED && !s_held && s_press_x>=0 && cell_at(p,&x,&y) && x==s_press_x && y==s_press_y)
        act(x,y,s_flagmode && !G.open[y][x]); }
static void mode_cb(lv_event_t*e){ (void)e; s_flagmode=!s_flagmode; hud(); }
static void tick_cb(lv_timer_t*t){ (void)t;
    uint32_t now=r3_tick_ms();
    if(G.laid && !G.over){ uint32_t before=s_elapsed/1000; s_elapsed+=now-s_last;
        if(s_elapsed/1000!=before) hud(); }
    s_last=now; }

int sapper_key(int code){
    if(!s_scr) return 0;
    if(code==164){ if(G.over) show_chooser(); else { s_flagmode=!s_flagmode; hud(); } return 1; }
    return 0; }                      /* prev / next keep driving the music */
int sapper_pad(int a){
    if(!s_scr) return 0;
    if(!lv_obj_has_flag(s_over,LV_OBJ_FLAG_HIDDEN)){             /* chooser open: A = last size */
        if(a==PA_A||a==PA_PLUS){ new_game(level_load()); return 1; } return a!=PA_HOME; }
    if(G.over){ if(a==PA_A||a==PA_PLUS){ show_chooser(); return 1; } return a!=PA_HOME; }
    if(a==PA_PLUS){ show_chooser(); return 1; }
    s_cursor=1;
    if(a==PA_UP&&s_cy>0) s_cy--; else if(a==PA_DOWN&&s_cy<G.rows-1) s_cy++;
    else if(a==PA_LEFT&&s_cx>0) s_cx--; else if(a==PA_RIGHT&&s_cx<G.cols-1) s_cx++;
    else if(a==PA_A){ act(s_cx,s_cy,0); return 1; } else if(a==PA_B){ act(s_cx,s_cy,1); return 1; }
    else return a!=PA_HOME;
    lv_obj_invalidate(s_board); return 1; }
int sapper_running(void){ return s_scr && G.laid && !G.over; }

/* ---- save / resume ---- */
typedef struct { uint32_t magic; sapper_t g; uint32_t elapsed; int flagmode; } sp_save_t;
void sapper_save(void){
    if(!s_scr) return; if(!G.laid || G.over){ unlink(SAVE); return; }
    sp_save_t s; memset(&s,0,sizeof s); s.magic=MAGIC; s.g=G; s.elapsed=s_elapsed; s.flagmode=s_flagmode;
    FILE*f=fopen(SAVE,"wb"); if(f){ fwrite(&s,sizeof s,1,f); fclose(f); } }
static int load_save(sp_save_t*s){ FILE*f=fopen(SAVE,"rb"); if(!f) return 0;
    int ok=fread(s,sizeof *s,1,f)==1 && s->magic==MAGIC && s->g.level>=0 && s->g.level<SP_NLEVELS; fclose(f); return ok; }
int sapper_has_save(int*cleared){ static sp_save_t s; if(!load_save(&s)) return 0; if(cleared) *cleared=s.g.opened; return 1; }

static void back_cb(lv_event_t*e){ (void)e; if(s_exit) s_exit(); }
static void del_cb(lv_event_t*e){ (void)e; sapper_save(); if(s_tmr){ lv_timer_del(s_tmr); s_tmr=0; } s_scr=0; }
static lv_obj_t* label(lv_obj_t*p,const lv_font_t*f,uint32_t col){ lv_obj_t*l=lv_label_create(p);
    lv_obj_set_style_text_font(l,f,0); lv_obj_set_style_text_color(l,lv_color_hex(col),0); lv_label_set_text(l,""); return l; }
static lv_obj_t* btn(lv_obj_t*p,int w,int h,uint32_t border){ lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,w,h);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x1A1510),0); lv_obj_set_style_radius(b,14,0); lv_obj_set_style_shadow_width(b,0,0);
    lv_obj_set_style_border_width(b,2,0); lv_obj_set_style_border_color(b,lv_color_hex(border),0); return b; }

lv_obj_t* sapper_create(void (*on_exit)(void),int resume){
    s_exit=on_exit; hs_load(&HS); s_flagmode=0; s_cursor=0;
    lv_obj_t*scr=lv_obj_create(NULL); s_scr=scr;
    lv_obj_set_style_bg_color(scr,lv_color_hex(0x0A0A0A),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr,del_cb,LV_EVENT_DELETE,NULL);
    lv_obj_t*bk=btn(scr,46,40,0xE8731C); lv_obj_set_pos(bk,8,7); lv_obj_add_event_cb(bk,back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT); lv_obj_set_style_text_color(bl,lv_color_hex(0xE8731C),0); lv_obj_center(bl);
    s_title=label(scr,&lv_font_montserrat_20,0xE8731C); lv_obj_set_pos(s_title,64,16);
    lv_obj_t*ml=label(scr,&lv_font_montserrat_12,0x8A6A4A); lv_label_set_text(ml,"MINES"); lv_obj_set_pos(ml,206,8);
    s_left=label(scr,&lv_font_montserrat_20,0xF4E4D0); lv_obj_set_pos(s_left,206,22);
    lv_obj_t*tl=label(scr,&lv_font_montserrat_12,0x8A6A4A); lv_label_set_text(tl,"TIME"); lv_obj_set_pos(tl,282,8);
    s_clock=label(scr,&lv_font_montserrat_20,0xF4E4D0); lv_obj_set_pos(s_clock,282,22);
    s_mode=btn(scr,86,40,0xE8731C); lv_obj_set_pos(s_mode,480-86-8,7); lv_obj_add_event_cb(s_mode,mode_cb,LV_EVENT_CLICKED,NULL);
    s_mode_lbl=label(s_mode,&lv_font_montserrat_16,0xE8731C); lv_obj_center(s_mode_lbl);
    s_board=lv_obj_create(scr);
    lv_obj_set_style_bg_color(s_board,lv_color_hex(0x0F0C09),0); lv_obj_set_style_radius(s_board,0,0);
    lv_obj_set_style_border_width(s_board,0,0); lv_obj_set_style_pad_all(s_board,0,0);
    lv_obj_clear_flag(s_board,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_board,board_draw_cb,LV_EVENT_DRAW_MAIN_END,NULL);
    lv_obj_add_event_cb(s_board,board_input_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(s_board,board_input_cb,LV_EVENT_LONG_PRESSED,NULL);
    lv_obj_add_event_cb(s_board,board_input_cb,LV_EVENT_SHORT_CLICKED,NULL);
    /* chooser / end panel */
    s_over=lv_obj_create(scr); lv_obj_set_size(s_over,420,LV_SIZE_CONTENT); lv_obj_align(s_over,LV_ALIGN_CENTER,0,20);
    lv_obj_set_style_bg_color(s_over,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s_over,LV_OPA_90,0);
    lv_obj_set_style_border_color(s_over,lv_color_hex(0xE8731C),0); lv_obj_set_style_border_width(s_over,2,0);
    lv_obj_set_style_radius(s_over,18,0); lv_obj_set_style_pad_all(s_over,16,0); lv_obj_set_style_pad_row(s_over,10,0);
    lv_obj_set_flex_flow(s_over,LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(s_over,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_over,LV_OBJ_FLAG_SCROLLABLE);
    s_over_lbl=label(s_over,&lv_font_montserrat_16,0xF4E4D0); lv_label_set_recolor(s_over_lbl,true);
    lv_obj_set_style_text_align(s_over_lbl,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_style_text_line_space(s_over_lbl,4,0);
    for(long i=0;i<SP_NLEVELS;i++){ int c,r,m; sp_level_dims((int)i,&c,&r,&m);
        lv_obj_t*b=btn(s_over,LV_PCT(100),58,0xE8731C); lv_obj_add_event_cb(b,level_btn_cb,LV_EVENT_CLICKED,(void*)i);
        lv_obj_t*l=label(b,&lv_font_montserrat_20,0xF4E4D0); char t[48];
        snprintf(t,sizeof t,"%s   #8A6A4A %dx%d, %d mines#",LEVEL_NAMES[i],c,r,m); lv_label_set_recolor(l,true);
        lv_label_set_text(l,t); lv_obj_center(l); }
    lv_obj_add_flag(s_over,LV_OBJ_FLAG_HIDDEN);
    sp_save_t sv;
    if(resume && load_save(&sv)){ G=sv.g; s_elapsed=sv.elapsed; s_flagmode=sv.flagmode; s_cx=G.cols/2; s_cy=G.rows/2; layout(); redraw(); }
    else { new_game(level_load()); show_chooser(); }
    unlink(SAVE);
    s_last=r3_tick_ms(); s_tmr=lv_timer_create(tick_cb,100,NULL);
    return scr;
}
