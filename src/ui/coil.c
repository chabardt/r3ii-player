/* coil.c — Coil screen (LVGL) on top of coil_core.c.
 * Swipe on the board to steer (or tap the left / right half to turn that way).
 * Keys: prev / next = turn left / right, play = pause (new game after game over).
 * Controller: stick / d-pad steer, A pause. Top-5 scores, resume or new game. */
#include "coil.h"
#include "coil_core.h"
#include "hiscore.h"
#include "pad.h"
#include "r3_tick.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define BY 54
#define CS 24
#define SAVE "/usr/data/r3_coil.sav"
#define MAGIC 0x434F4931u   /* "COI1" */
static coil_t G; static int paused;
static hiscore_t HS={ "/usr/data/r3_coil" };
static lv_obj_t *s_scr,*s_board,*s_score,*s_level,*s_over,*s_over_lbl,*s_pause,*s_toast;
static lv_timer_t *s_tmr; static uint32_t s_last_step, s_toast_until; static void (*s_exit)(void);

static void rect(lv_draw_ctx_t*dc,int x1,int y1,int x2,int y2,uint32_t c,int r,lv_opa_t o){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color=lv_color_hex(c); d.bg_opa=o; d.radius=r;
    lv_area_t a={x1,y1,x2,y2}; lv_draw_rect(dc,&d,&a); }
static void board_draw_cb(lv_event_t*e){
    lv_draw_ctx_t*dc=lv_event_get_draw_ctx(e); lv_area_t c; lv_obj_get_coords(lv_event_get_target(e),&c);
    int ox=c.x1, oy=c.y1;
    for(int y=0;y<CH;y+=2) for(int x=(y/2)%2;x<CW;x+=2)                     /* faint checker */
        rect(dc,ox+x*CS,oy+y*CS,ox+x*CS+CS-1,oy+y*CS+CS-1,0x17120D,0,LV_OPA_COVER);
    /* food */
    int fx=ox+G.fruit.x*CS, fy=oy+G.fruit.y*CS;
    rect(dc,fx+3,fy+3,fx+CS-4,fy+CS-4,0xE04848,LV_RADIUS_CIRCLE,LV_OPA_COVER);
    rect(dc,fx+CS/2,fy+1,fx+CS/2+5,fy+6,0x9BC15B,3,LV_OPA_COVER);          /* leaf */
    if(G.gold_ticks>0){ int gx=ox+G.gold.x*CS, gy=oy+G.gold.y*CS, pulse=(r3_tick_ms()/120)%2;
        rect(dc,gx-1-pulse,gy-1-pulse,gx+CS+pulse,gy+CS+pulse,0xFFE27A,LV_RADIUS_CIRCLE,LV_OPA_30);
        rect(dc,gx+2,gy+2,gx+CS-3,gy+CS-3,0xF2B33D,LV_RADIUS_CIRCLE,LV_OPA_COVER);
        int w=(CS-4)*G.gold_ticks/45; rect(dc,gx+2,gy+CS-2,gx+2+w,gy+CS,0xFFE27A,1,LV_OPA_COVER); }   /* time left */
    if(G.berry_on){ int bx=ox+G.berry.x*CS, by=oy+G.berry.y*CS;
        rect(dc,bx+4,by+4,bx+CS-5,by+CS-5,0x6C8CD5,LV_RADIUS_CIRCLE,LV_OPA_COVER); }
    /* snake: tail darker, head bright with eyes */
    for(int i=G.len-1;i>=0;i--){ cell_t s=coil_seg(&G,i); int x=ox+s.x*CS, y=oy+s.y*CS;
        int t=G.len>1? i*100/(G.len-1) : 0;    /* 0 head .. 100 tail */
        uint32_t col = i==0 ? 0xF2B33D : (t<50 ? 0xE8731C : 0xB85518);
        rect(dc,x+1,y+1,x+CS-2,y+CS-2,col,i==0?9:6,LV_OPA_COVER); }
    { cell_t h=coil_seg(&G,0); int x=ox+h.x*CS, y=oy+h.y*CS;
      int ex1,ey1,ex2,ey2, d=G.dir;
      if(d==DIR_UP){ ex1=x+6;ey1=y+6;ex2=x+CS-10;ey2=y+6; } else if(d==DIR_DOWN){ ex1=x+6;ey1=y+CS-10;ex2=x+CS-10;ey2=y+CS-10; }
      else if(d==DIR_LEFT){ ex1=x+6;ey1=y+6;ex2=x+6;ey2=y+CS-10; } else { ex1=x+CS-10;ey1=y+6;ex2=x+CS-10;ey2=y+CS-10; }
      rect(dc,ex1,ey1,ex1+4,ey1+4,0x0A0A0A,LV_RADIUS_CIRCLE,LV_OPA_COVER);
      rect(dc,ex2,ey2,ex2+4,ey2+4,0x0A0A0A,LV_RADIUS_CIRCLE,LV_OPA_COVER); }
}
static void hud(void){ char b[32]; snprintf(b,sizeof b,"%d",G.score); lv_label_set_text(s_score,b);
    snprintf(b,sizeof b,"LEVEL %d",G.level); lv_label_set_text(s_level,b); }
static void toast(const char*t){ lv_label_set_text(s_toast,t); lv_obj_clear_flag(s_toast,LV_OBJ_FLAG_HIDDEN); s_toast_until=r3_tick_ms()+1200; }
static void game_over(void){
    static char b[512]; int rank=hs_add(&HS,G.score); hs_format(&HS,rank,G.score,b,sizeof b);
    lv_label_set_text(s_over_lbl,b); lv_obj_clear_flag(s_over,LV_OBJ_FLAG_HIDDEN); unlink(SAVE); }
static void new_game(void){
    coil_new(&G,(unsigned)time(0)^r3_tick_ms()); paused=0;
    lv_label_set_text(s_pause,""); lv_obj_add_flag(s_over,LV_OBJ_FLAG_HIDDEN);
    hud(); lv_obj_invalidate(s_board); s_last_step=r3_tick_ms(); toast("SWIPE TO STEER");
}
static void set_pause(int p){ paused=p; lv_label_set_text(s_pause,p?"PAUSED":""); s_last_step=r3_tick_ms(); }
static void tick_cb(lv_timer_t*t){ (void)t;
    uint32_t now=r3_tick_ms();
    if(s_toast_until && now>s_toast_until){ s_toast_until=0; lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); }
    if(G.over||paused) return;
    if(G.gold_ticks>0) lv_obj_invalidate(s_board);       /* pulsing gold fruit */
    if(now-s_last_step < (uint32_t)coil_tick_ms(&G)) return;
    s_last_step=now;
    int sc=G.score, lv=G.level; coil_step(&G);
    if(G.ate==F_GOLD) toast("GOLDEN! +2"); else if(G.ate==F_BERRY) toast("TRIMMED -3");
    else if(G.level!=lv) toast("FASTER!");
    if(G.score!=sc||G.level!=lv) hud();
    lv_obj_invalidate(s_board);
    if(G.over) game_over();
}
/* touch: swipe = absolute direction, tap = turn left / right half */
static lv_point_t p0;
static void board_input_cb(lv_event_t*e){
    lv_event_code_t code=lv_event_get_code(e); lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    if(G.over){ if(code==LV_EVENT_CLICKED) new_game(); return; }
    if(code==LV_EVENT_PRESSED){ p0=p; return; }
    if(code!=LV_EVENT_RELEASED) return;
    if(paused){ set_pause(0); return; }
    int dx=p.x-p0.x, dy=p.y-p0.y;
    if(dx*dx+dy*dy>30*30){ if(abs(dx)>abs(dy)) coil_turn(&G,dx>0?DIR_RIGHT:DIR_LEFT); else coil_turn(&G,dy>0?DIR_DOWN:DIR_UP); }
    else coil_turn_rel(&G,p.x>=240);
}
int coil_key(int code){
    if(!s_scr) return 0;
    if(G.over){ if(code==164) new_game(); return 1; }
    if(code==164){ set_pause(!paused); return 1; }
    if(paused) return 1;
    if(code==165) coil_turn_rel(&G,0); else if(code==163) coil_turn_rel(&G,1); else return 0;
    return 1;
}
int coil_pad(int a){
    if(!s_scr) return 0;
    if(G.over){ if(a==PA_A||a==PA_PLUS) new_game(); return 1; }
    if(a==PA_A||a==PA_PLUS||a==PA_MINUS){ set_pause(!paused); return 1; }
    if(paused) return 1;
    if(a==PA_UP) coil_turn(&G,DIR_UP); else if(a==PA_DOWN) coil_turn(&G,DIR_DOWN);
    else if(a==PA_LEFT) coil_turn(&G,DIR_LEFT); else if(a==PA_RIGHT) coil_turn(&G,DIR_RIGHT);
    else if(a==PA_L) coil_turn_rel(&G,0); else if(a==PA_R) coil_turn_rel(&G,1);
    else return 0;
    return 1;
}
int coil_running(void){ return s_scr && !G.over; }

/* save / resume */
void coil_save(void){
    if(!s_scr) return; if(G.over){ unlink(SAVE); return; }
    uint32_t m=MAGIC; FILE*f=fopen(SAVE,"wb"); if(!f) return;
    fwrite(&m,sizeof m,1,f); fwrite(&G,sizeof G,1,f); fclose(f); }
static int load_save(coil_t*g){ uint32_t m; FILE*f=fopen(SAVE,"rb"); if(!f) return 0;
    int ok=fread(&m,sizeof m,1,f)==1 && m==MAGIC && fread(g,sizeof *g,1,f)==1; fclose(f); return ok; }
int coil_has_save(int*score){ static coil_t g; if(!load_save(&g)) return 0; if(score) *score=g.score; return 1; }

static void back_cb(lv_event_t*e){ (void)e; if(s_exit) s_exit(); }
static void del_cb(lv_event_t*e){ (void)e; coil_save(); if(s_tmr){ lv_timer_del(s_tmr); s_tmr=0; } s_scr=0; }
static lv_obj_t* label(lv_obj_t*p,const lv_font_t*f,uint32_t col){ lv_obj_t*l=lv_label_create(p);
    lv_obj_set_style_text_font(l,f,0); lv_obj_set_style_text_color(l,lv_color_hex(col),0); lv_label_set_text(l,""); return l; }

lv_obj_t* coil_create(void (*on_exit)(void),int resume){
    s_exit=on_exit; hs_load(&HS);
    lv_obj_t*scr=lv_obj_create(NULL); s_scr=scr;
    lv_obj_set_style_bg_color(scr,lv_color_hex(0x0A0A0A),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr,del_cb,LV_EVENT_DELETE,NULL);
    lv_obj_t*bk=lv_btn_create(scr); lv_obj_set_size(bk,46,40); lv_obj_set_pos(bk,8,7);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x1A1510),0); lv_obj_set_style_radius(bk,14,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(0xE8731C),0);
    lv_obj_add_event_cb(bk,back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT); lv_obj_set_style_text_color(bl,lv_color_hex(0xE8731C),0); lv_obj_center(bl);
    lv_obj_t*t=label(scr,&lv_font_montserrat_20,0xE8731C); lv_label_set_text(t,"COIL"); lv_obj_set_pos(t,64,16);
    s_level=label(scr,&lv_font_montserrat_14,0x8A6A4A); lv_obj_align(s_level,LV_ALIGN_TOP_RIGHT,-110,20);
    s_score=label(scr,&lv_font_montserrat_28,0xF4E4D0); lv_obj_align(s_score,LV_ALIGN_TOP_RIGHT,-12,10);
    s_board=lv_obj_create(scr); lv_obj_set_size(s_board,CW*CS,CH*CS); lv_obj_set_pos(s_board,0,BY);
    lv_obj_set_style_bg_color(s_board,lv_color_hex(0x0F0C09),0); lv_obj_set_style_radius(s_board,0,0);
    lv_obj_set_style_border_width(s_board,1,0); lv_obj_set_style_border_color(s_board,lv_color_hex(0x5A3A1E),0);
    lv_obj_set_style_border_post(s_board,true,0); lv_obj_set_style_pad_all(s_board,0,0);
    lv_obj_clear_flag(s_board,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_board,board_draw_cb,LV_EVENT_DRAW_MAIN_END,NULL);
    lv_obj_add_event_cb(s_board,board_input_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(s_board,board_input_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(s_board,board_input_cb,LV_EVENT_CLICKED,NULL);
    s_toast=label(scr,&lv_font_montserrat_28,0xE8731C); lv_obj_set_width(s_toast,480);
    lv_obj_set_style_text_align(s_toast,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_pos(s_toast,0,BY+260);
    lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_toast,LV_OBJ_FLAG_CLICKABLE);
    s_pause=label(scr,&lv_font_montserrat_28,0xF4E4D0); lv_obj_set_width(s_pause,480);
    lv_obj_set_style_text_align(s_pause,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_pos(s_pause,0,BY+300);
    lv_obj_clear_flag(s_pause,LV_OBJ_FLAG_CLICKABLE);
    s_over=lv_obj_create(scr); lv_obj_set_size(s_over,400,340); lv_obj_set_pos(s_over,40,BY+140);
    lv_obj_set_style_bg_color(s_over,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s_over,LV_OPA_80,0);
    lv_obj_set_style_border_color(s_over,lv_color_hex(0xE8731C),0); lv_obj_set_style_border_width(s_over,2,0);
    lv_obj_set_style_radius(s_over,18,0); lv_obj_clear_flag(s_over,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    s_over_lbl=label(s_over,&lv_font_montserrat_16,0xF4E4D0); lv_label_set_recolor(s_over_lbl,true);
    lv_obj_set_style_text_align(s_over_lbl,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_style_text_line_space(s_over_lbl,4,0);
    lv_obj_center(s_over_lbl);
    new_game();
    if(resume && load_save(&G)){ hud(); set_pause(1); lv_label_set_text(s_pause,"PAUSED - tap to go on");
        lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); s_toast_until=0; lv_obj_invalidate(s_board); }
    unlink(SAVE);
    s_tmr=lv_timer_create(tick_cb,15,NULL);
    return scr;
}
