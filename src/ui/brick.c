/* brick.c — Brickstorm screen (LVGL) on top of brick_core.c.
 *
 * Drag anywhere on the field: the paddle follows your finger. Tap: launch the ball.
 * Keys: prev/next = paddle left/right, play = launch / pause (new game after game over).
 * Only the areas around moving things are redrawn each frame (smooth on the X1600). */
#include "brick.h"
#include "brick_core.h"
#include "hiscore.h"
#include "r3_tick.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FY 54                                   /* field top on screen */
static const uint32_t ROWC[7]={0xE8731C,0xF2B33D,0x9BC15B,0x4FB0A5,0x6C8CD5,0xC75B8F,0xE04848};

static bk_t G; static int paused;
static hiscore_t HS={ "/usr/data/r3_brickstorm" };
static lv_obj_t *s_scr,*s_field,*s_score,*s_level,*s_toast,*s_over,*s_over_lbl,*s_pause;
static lv_timer_t *s_tmr; static uint32_t s_last, s_toast_until;
static lv_area_t prev_ball[BK_MAXB], prev_pad; static int prev_ball_on[BK_MAXB];

/* ---- drawing ---- */
static void rect(lv_draw_ctx_t*dc,int x1,int y1,int x2,int y2,uint32_t col,int radius,lv_opa_t opa){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color=lv_color_hex(col); d.bg_opa=opa; d.radius=radius;
    lv_area_t a={x1,y1,x2,y2}; lv_draw_rect(dc,&d,&a);
}
static const char* SPECIAL_TXT[7]={0,"M","X","F","S","W","N"};
static void field_draw_cb(lv_event_t*e){
    lv_draw_ctx_t*dc=lv_event_get_draw_ctx(e); lv_obj_t*o=lv_event_get_target(e);
    lv_area_t c; lv_obj_get_coords(o,&c); int ox=c.x1, oy=c.y1;
    const lv_area_t*clip=dc->clip_area;
    /* bricks (skip the whole band when the clip doesn't touch it) */
    if(clip->y1 <= oy+BK_Y0+BK_NR*BK_DY && clip->y2 >= oy+BK_Y0){
        lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
        ld.font=&lv_font_montserrat_14; ld.color=lv_color_hex(0x0A0A0A); ld.align=LV_TEXT_ALIGN_CENTER;
        for(int r=0;r<BK_NR;r++) for(int k=0;k<BK_NC;k++){ bk_brick_t*b=&G.br[r][k]; if(b->hp<=0) continue;
            int x=ox+(int)bk_brick_x(k), y=oy+(int)bk_brick_y(r);
            uint32_t col = b->type!=BT_NORMAL ? 0xF4E4D0 : ROWC[b->col%7];
            rect(dc,x,y,x+BK_BW-1,y+BK_BH-1,col,3,b->hp>1?LV_OPA_COVER:LV_OPA_80);
            if(b->hp>1) rect(dc,x+3,y+3,x+BK_BW-4,y+BK_BH-4,0x000000,2,LV_OPA_30);   /* tough: inset */
            if(b->type!=BT_NORMAL){ lv_area_t a={x,y+1,x+BK_BW-1,y+BK_BH-1};
                lv_draw_label(dc,&ld,&a,SPECIAL_TXT[(int)b->type],NULL); }
        }
    }
    /* paddle */
    int px=ox+(int)G.px; rect(dc,px,oy+BK_PY,px+G.pw-1,oy+BK_PY+BK_PH-1,0xE8731C,7,LV_OPA_COVER);
    /* balls (orange-red when armed to explode) */
    for(int i=0;i<BK_MAXB;i++){ bk_ball_t*b=&G.b[i]; if(!b->on) continue;
        int x=ox+(int)b->x, y=oy+(int)b->y;
        rect(dc,x-BK_R,y-BK_R,x+BK_R,y+BK_R,b->bomb?0xE04848:0xF4E4D0,LV_RADIUS_CIRCLE,LV_OPA_COVER); }
}
static lv_area_t ball_area(const bk_ball_t*b){ lv_area_t a; lv_area_t c; lv_obj_get_coords(s_field,&c);
    a.x1=c.x1+(int)b->x-BK_R-2; a.y1=c.y1+(int)b->y-BK_R-2; a.x2=c.x1+(int)b->x+BK_R+2; a.y2=c.y1+(int)b->y+BK_R+2; return a; }
static lv_area_t pad_area(void){ lv_area_t a; lv_area_t c; lv_obj_get_coords(s_field,&c);
    a.x1=c.x1+(int)G.px-2; a.y1=c.y1+BK_PY-1; a.x2=c.x1+(int)G.px+G.pw+2; a.y2=c.y1+BK_PY+BK_PH+1; return a; }
/* invalidate where things were and where they are now */
static void invalidate_moving(void){
    for(int i=0;i<BK_MAXB;i++){
        if(prev_ball_on[i]) lv_obj_invalidate_area(s_field,&prev_ball[i]);
        prev_ball_on[i]=G.b[i].on;
        if(G.b[i].on){ prev_ball[i]=ball_area(&G.b[i]); lv_obj_invalidate_area(s_field,&prev_ball[i]); } }
    lv_area_t p=pad_area();
    if(p.x1!=prev_pad.x1 || p.x2!=prev_pad.x2){ lv_obj_invalidate_area(s_field,&prev_pad); lv_obj_invalidate_area(s_field,&p); prev_pad=p; }
    if(G.dirty_bricks){ G.dirty_bricks=0; lv_area_t c; lv_obj_get_coords(s_field,&c);
        lv_area_t a={c.x1,c.y1+BK_Y0-2,c.x2,c.y1+BK_Y0+BK_NR*BK_DY+2}; lv_obj_invalidate_area(s_field,&a); }
}

/* ---- HUD ---- */
static void hud(void){ char b[32];
    snprintf(b,sizeof b,"%d",G.score); lv_label_set_text(s_score,b);
    snprintf(b,sizeof b,"LEVEL %d",G.level); lv_label_set_text(s_level,b); }
static void toast(const char*t){ lv_label_set_text(s_toast,t); lv_obj_clear_flag(s_toast,LV_OBJ_FLAG_HIDDEN);
    s_toast_until=r3_tick_ms()+1300; }
static void game_over(void){
    int rank=hs_add(&HS,G.score); static char b[512];
    hs_format(&HS,rank,G.score,b,sizeof b); lv_label_set_text(s_over_lbl,b);
    lv_obj_clear_flag(s_over,LV_OBJ_FLAG_HIDDEN);
}
static void new_game(void){
    bk_new_game(&G,(unsigned)time(0)^r3_tick_ms()); paused=0;
    lv_label_set_text(s_pause,""); lv_obj_add_flag(s_over,LV_OBJ_FLAG_HIDDEN);
    memset(prev_ball_on,0,sizeof prev_ball_on); prev_pad=pad_area();
    hud(); toast("TAP TO LAUNCH"); lv_obj_invalidate(s_field); s_last=r3_tick_ms();
}

static void tick_cb(lv_timer_t*t){ (void)t;
    uint32_t now=r3_tick_ms(); float dt=(now-s_last)/1000.0f; s_last=now;
    if(s_toast_until && now>s_toast_until){ s_toast_until=0; lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); }
    if(G.over||paused) return;
    int sc=G.score, lv=G.level;
    bk_step(&G,dt);
    if(G.toast){ int k=G.toast; G.toast=0;
        toast(k==BT_MULTI?"MULTI BALL!":k==BT_BOMB?"EXPLOSIVE BALL!":k==BT_FAST?"FASTER!":
              k==BT_SLOW?"SLOWER":k==BT_WIDE?"WIDE PADDLE!":k==BT_TINY?"TINY PADDLE":k==100?"LEVEL CLEAR!":""); }
    if(G.score!=sc||G.level!=lv) hud();
    invalidate_moving();
    if(G.over) game_over();
}

/* ---- input ---- */
static int touch_moved; static lv_coord_t touch_x0;
static void field_input_cb(lv_event_t*e){
    lv_event_code_t code=lv_event_get_code(e); lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    if(G.over){ if(code==LV_EVENT_CLICKED) new_game(); return; }
    if(code==LV_EVENT_PRESSED){ touch_moved=0; touch_x0=p.x; }
    if(code==LV_EVENT_PRESSING && !paused){ if(LV_ABS(p.x-touch_x0)>12) touch_moved=1; bk_set_paddle(&G,(float)p.x); }
    if(code==LV_EVENT_RELEASED){ if(paused){ paused=0; lv_label_set_text(s_pause,""); s_last=r3_tick_ms(); }
        else if(bk_any_stuck(&G) && !touch_moved) bk_launch(&G); }
}
int brick_key(int code){
    if(!s_scr) return 0;
    if(G.over){ if(code==164) new_game(); return 1; }
    if(code==164){
        if(bk_any_stuck(&G) && !paused){ bk_launch(&G); return 1; }
        paused=!paused; lv_label_set_text(s_pause,paused?"PAUSED":""); s_last=r3_tick_ms(); return 1; }
    if(paused) return 1;
    if(code==165) bk_set_paddle(&G,G.px+G.pw/2.0f-48);
    else if(code==163) bk_set_paddle(&G,G.px+G.pw/2.0f+48);
    else return 0;
    return 1;
}
int brick_running(void){ return s_scr && !G.over; }

static void (*s_exit)(void);
static void back_cb(lv_event_t*e){ (void)e; if(s_exit) s_exit(); }
/* ---- save / resume ---- */
#define BK_SAVE "/usr/data/r3_brickstorm.sav"
#define BK_MAGIC 0x42534B32u   /* "BSK2" */
void brick_save(void){
    if(!s_scr) return;
    if(G.over){ unlink(BK_SAVE); return; }
    uint32_t m=BK_MAGIC; FILE*f=fopen(BK_SAVE,"wb"); if(!f) return;
    fwrite(&m,sizeof m,1,f); fwrite(&G,sizeof G,1,f); fclose(f);
}
static int load_save(uint32_t*m,bk_t*g){ FILE*f=fopen(BK_SAVE,"rb"); if(!f) return 0;
    int ok=fread(m,sizeof *m,1,f)==1 && *m==BK_MAGIC && fread(g,sizeof *g,1,f)==1; fclose(f); return ok; }
int brick_has_save(int*score){ uint32_t m; static bk_t g; if(!load_save(&m,&g)) return 0;
    if(score) *score=g.score; return 1; }
static void del_cb(lv_event_t*e){ (void)e; brick_save(); if(s_tmr){ lv_timer_del(s_tmr); s_tmr=0; } s_scr=0; }
static lv_obj_t* label(lv_obj_t*p,const lv_font_t*f,uint32_t col){ lv_obj_t*l=lv_label_create(p);
    lv_obj_set_style_text_font(l,f,0); lv_obj_set_style_text_color(l,lv_color_hex(col),0); lv_label_set_text(l,""); return l; }

lv_obj_t* brick_create(void (*on_exit)(void),int resume){
    s_exit=on_exit; hs_load(&HS);
    lv_obj_t*scr=lv_obj_create(NULL); s_scr=scr;
    lv_obj_set_style_bg_color(scr,lv_color_hex(0x0A0A0A),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr,del_cb,LV_EVENT_DELETE,NULL);
    lv_obj_t*bk=lv_btn_create(scr); lv_obj_set_size(bk,46,40); lv_obj_set_pos(bk,8,7);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x1A1510),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(0xE8731C),0);
    lv_obj_add_event_cb(bk,back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl,lv_color_hex(0xE8731C),0); lv_obj_center(bl);
    lv_obj_t*t=label(scr,&lv_font_montserrat_20,0xE8731C); lv_label_set_text(t,"BRICKSTORM"); lv_obj_set_pos(t,64,16);
    s_level=label(scr,&lv_font_montserrat_14,0x8A6A4A); lv_obj_align(s_level,LV_ALIGN_TOP_RIGHT,-110,20);
    s_score=label(scr,&lv_font_montserrat_28,0xF4E4D0); lv_obj_align(s_score,LV_ALIGN_TOP_RIGHT,-12,10);
    lv_obj_set_style_text_align(s_score,LV_TEXT_ALIGN_RIGHT,0);
    /* field */
    s_field=lv_obj_create(scr); lv_obj_set_size(s_field,BK_W,BK_H); lv_obj_set_pos(s_field,0,FY);
    lv_obj_set_style_bg_color(s_field,lv_color_hex(0x15110D),0); lv_obj_set_style_radius(s_field,0,0);
    lv_obj_set_style_border_width(s_field,0,0); lv_obj_set_style_pad_all(s_field,0,0);
    lv_obj_clear_flag(s_field,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_field,field_draw_cb,LV_EVENT_DRAW_MAIN_END,NULL);
    lv_obj_add_event_cb(s_field,field_input_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(s_field,field_input_cb,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(s_field,field_input_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(s_field,field_input_cb,LV_EVENT_CLICKED,NULL);
    /* toast + pause (on the screen, over the lower field) */
    s_toast=label(scr,&lv_font_montserrat_28,0xE8731C); lv_obj_set_width(s_toast,480);
    lv_obj_set_style_text_align(s_toast,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_pos(s_toast,0,FY+380);
    lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_toast,LV_OBJ_FLAG_CLICKABLE);
    s_pause=label(scr,&lv_font_montserrat_28,0xF4E4D0); lv_obj_set_width(s_pause,480);
    lv_obj_set_style_text_align(s_pause,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_pos(s_pause,0,FY+300);
    /* game over */
    s_over=lv_obj_create(scr); lv_obj_set_size(s_over,400,340); lv_obj_set_pos(s_over,40,FY+170);
    lv_obj_set_style_bg_color(s_over,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s_over,LV_OPA_80,0);
    lv_obj_set_style_border_color(s_over,lv_color_hex(0xE8731C),0); lv_obj_set_style_border_width(s_over,2,0);
    lv_obj_set_style_radius(s_over,0,0); lv_obj_clear_flag(s_over,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    s_over_lbl=label(s_over,&lv_font_montserrat_16,0xF4E4D0); lv_label_set_recolor(s_over_lbl,true);
    lv_obj_set_style_text_align(s_over_lbl,LV_TEXT_ALIGN_CENTER,0); lv_obj_set_style_text_line_space(s_over_lbl,4,0);
    lv_obj_center(s_over_lbl);
    new_game();
    struct { uint32_t magic; bk_t g; } sv;
    if(resume && load_save(&sv.magic,&sv.g)){   /* continue where we left off, paused until a tap */
        G=sv.g; paused=1; lv_label_set_text(s_pause,"PAUSED - tap to go on");
        lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); s_toast_until=0;
        hud(); prev_pad=pad_area(); lv_obj_invalidate(s_field); }
    unlink(BK_SAVE);              /* the live game owns the state now (saved again on leave) */
    s_tmr=lv_timer_create(tick_cb,16,NULL);
    return scr;
}
