/* pet.c — Moko screen: vector-drawn pet in a day/night scene + care actions.
 * Life rules live in pet_core.c; this file draws, animates and saves. */
#include "pet.h"
#include "pet_core.h"
#include "r3_tick.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#ifndef PET_FILE
#define PET_FILE "/usr/data/r3_pet.sav"
#endif
#define SY 54                    /* scene top */
#define SW 480
#define SH 430
#define GROUND (SH-86)           /* pet feet line in scene coords */

static pet_t P; static int P_loaded, P_dirty; static uint32_t P_saved_at;

/* ---------- persistence + background ticking (works while the screen is closed) ---------- */
static void pet_save(void){ FILE*f=fopen(PET_FILE,"wb"); if(f){ fwrite(&P,sizeof P,1,f); fclose(f); } P_dirty=0; P_saved_at=r3_tick_ms(); }
static void pet_load(void){
    if(P_loaded) return; P_loaded=1;
    FILE*f=fopen(PET_FILE,"rb"); int ok=0;
    if(f){ ok=fread(&P,sizeof P,1,f)==1; fclose(f); }
    if(!ok || P.magic!=0x4D4F4B31u){ pet_new(&P,time(0),(unsigned)time(0)^r3_tick_ms()); P_dirty=1; }
    pet_catch_up(&P,time(0));
}
void pet_tick(void){                     /* called every ~30 s from the main loop */
    pet_load(); int64_t before=P.last;
    pet_catch_up(&P,time(0));
    if(P.last!=before) P_dirty=1;
    if(P_dirty && r3_tick_ms()-P_saved_at>5*60*1000u) pet_save();
}
int pet_needs_you(void){ pet_load(); return pet_attention(&P)!=0 && P.stage!=PS_GONE; }
const char* pet_summary(void){ static char b[64]; pet_load();
    if(P.stage==PS_GONE){ snprintf(b,sizeof b,"went back to the stars"); return b; }
    int a=pet_attention(&P);
    snprintf(b,sizeof b,"%s, %s%s",pet_form_name(&P), P.asleep?"asleep":"awake",
        a&PA_SICK?" - sick!":a&PA_HUNGRY?" - hungry!":a&PA_POOP?" - needs cleaning":a&PA_SAD?" - bored":a?" - calling":"");
    return b; }

/* ---------- screen state ---------- */
static lv_obj_t *s_scr,*s_scene,*s_title,*s_toast,*s_status,*s_gone;
static lv_timer_t *s_tmr; static void (*s_exit)(void);
static float px=SW/2, ptx=SW/2, phase; static int look=0, blink_ms=0, eat_ms=0, eat_kind=0, happy_ms=0;
static uint32_t next_wander, next_blink, last_ms, toast_until;
static lv_area_t prev_box; static int game_on; static uint32_t game_end;
typedef struct { float x,y,v; int on; } bubble_t; static bubble_t bub[10]; static int popped;

static void toast(const char*t){ lv_label_set_text(s_toast,t); lv_obj_clear_flag(s_toast,LV_OBJ_FLAG_HIDDEN); toast_until=r3_tick_ms()+1800; }
static void title(void){ char b[80];
    if(P.stage==PS_GONE) snprintf(b,sizeof b,"MOKO");
    else if(P.stage==PS_EGG) snprintf(b,sizeof b,"MOKO  -  egg");
    else snprintf(b,sizeof b,"%s  -  %s, day %d",pet_form_name(&P),pet_stage_name(&P),P.age_days+1);
    lv_label_set_text(s_title,b); }

/* ---------- drawing helpers ---------- */
static lv_draw_ctx_t*DC; static int OX,OY;
static void circ(float cx,float cy,float r,uint32_t col,lv_opa_t opa){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color=lv_color_hex(col); d.bg_opa=opa; d.radius=LV_RADIUS_CIRCLE;
    lv_area_t a={OX+(int)(cx-r),OY+(int)(cy-r),OX+(int)(cx+r),OY+(int)(cy+r)}; lv_draw_rect(DC,&d,&a); }
static void ell(float cx,float cy,float rx,float ry,uint32_t col,lv_opa_t opa){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color=lv_color_hex(col); d.bg_opa=opa; d.radius=LV_RADIUS_CIRCLE;
    lv_area_t a={OX+(int)(cx-rx),OY+(int)(cy-ry),OX+(int)(cx+rx),OY+(int)(cy+ry)}; lv_draw_rect(DC,&d,&a); }
static void box(int x1,int y1,int x2,int y2,uint32_t top,uint32_t bot,int rad){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color=lv_color_hex(top); d.bg_grad.dir=top==bot?LV_GRAD_DIR_NONE:LV_GRAD_DIR_VER;
    d.bg_grad.stops[0].color=lv_color_hex(top); d.bg_grad.stops[1].color=lv_color_hex(bot);
    d.bg_grad.stops[0].frac=0; d.bg_grad.stops[1].frac=255; d.bg_grad.stops_count=2; d.radius=rad;
    lv_area_t a={OX+x1,OY+y1,OX+x2,OY+y2}; lv_draw_rect(DC,&d,&a); }
static void arc(float cx,float cy,float r,int a0,int a1,int w,uint32_t col){
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d); d.color=lv_color_hex(col); d.width=w; d.rounded=1;
    lv_point_t c={OX+(lv_coord_t)cx,OY+(lv_coord_t)cy}; lv_draw_arc(DC,&d,&c,(uint16_t)r,a0,a1); }
static void text(int x,int y,const char*t,uint32_t col,const lv_font_t*f){
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d); d.color=lv_color_hex(col); d.font=f;
    lv_area_t a={OX+x,OY+y,OX+x+200,OY+y+40}; lv_draw_label(DC,&d,&a,t,NULL); }

/* ---------- the pet ---------- */
typedef struct { float r; uint32_t body, belly, accent; int ears, tufts, antenna, droopy; } look_t;
static look_t pet_look(void){
    look_t l={46,0xF6D58E,0xFBEBC4,0xE8731C,0,0,0,0};
    switch(P.stage){
    case PS_BABY:  l.r=34; l.body=0xF8E3A6; l.belly=0xFDF3D8; break;
    case PS_CHILD: l.r=44; l.body=0xF4C978; l.ears=1; break;
    default:
        if(P.form==PF_LUMI){ l.r=P.stage==PS_TEEN?52:60; l.body=0x7FD6C8; l.belly=0xD6F5EF; l.accent=0xFFE27A; l.antenna=1; }
        else if(P.form==PF_BRAMBLE){ l.r=P.stage==PS_TEEN?52:62; l.body=0xD08A4E; l.belly=0xF1C79A; l.accent=0x8A4E24; l.ears=1; l.tufts=1; }
        else { l.r=P.stage==PS_TEEN?50:58; l.body=0xA4AA62; l.belly=0xD8DDA2; l.accent=0x6E7440; l.droopy=1; }
        if(P.stage==PS_ELDER){ l.body=(l.body&0xFEFEFE)/2+0x7F7F7F; l.belly=0xF2F2EA; }
    }
    return l;
}
static void draw_pet(float cx,float base){
    look_t L=pet_look(); float r=L.r;
    float bounce = P.asleep?0 : (game_on||eat_ms? fabsf(sinf(phase*2.2f))*6 : fabsf(sinf(phase))*4);
    float squash = P.asleep? 1.0f+0.03f*sinf(phase*0.8f) : 1.0f;
    float cy=base-r*(L.droopy?0.85f:1.0f)-bounce;
    float rx=r*(L.droopy?1.18f:1.0f)*squash, ry=r*(L.droopy?0.85f:1.0f);
    ell(cx,base-2,rx*0.8f,7,0x000000,LV_OPA_20);                                  /* shadow */
    if(L.ears){ circ(cx-rx*0.62f,cy-ry*0.78f,r*0.26f,L.body,LV_OPA_COVER); circ(cx+rx*0.62f,cy-ry*0.78f,r*0.26f,L.body,LV_OPA_COVER);
                circ(cx-rx*0.62f,cy-ry*0.78f,r*0.13f,L.belly,LV_OPA_COVER); circ(cx+rx*0.62f,cy-ry*0.78f,r*0.13f,L.belly,LV_OPA_COVER); }
    if(L.antenna){ for(int i=0;i<6;i++) circ(cx+sinf(phase*1.3f)*i*1.2f,cy-ry-4-i*5,2.2f,L.body,LV_OPA_COVER);
        float sx=cx+sinf(phase*1.3f)*8, sy=cy-ry-36; circ(sx,sy,12,L.accent,LV_OPA_30); circ(sx,sy,7,L.accent,LV_OPA_COVER); }
    ell(cx-rx*0.45f,base-4,r*0.28f,r*0.14f,L.accent,LV_OPA_COVER);              /* feet */
    ell(cx+rx*0.45f,base-4,r*0.28f,r*0.14f,L.accent,LV_OPA_COVER);
    ell(cx,cy,rx,ry,L.body,LV_OPA_COVER);                                         /* body */
    if(L.tufts) for(int i=-2;i<=2;i++) circ(cx+i*rx*0.28f,cy-ry*0.92f+fabsf((float)i)*4,r*0.17f,L.body,LV_OPA_COVER);
    ell(cx,cy+ry*0.32f,rx*0.62f,ry*0.55f,L.belly,LV_OPA_COVER);                  /* belly */
    /* face */
    float ey=cy-ry*0.18f, ex=rx*0.36f, er=r*0.17f, lookx=look*er*0.35f;
    int sick=P.sick, sad=P.happy<=1||P.hunger==0;
    if(P.asleep || blink_ms>0){ arc(cx-ex,ey,er*0.7f,20,160,3,0x3A2A1E); arc(cx+ex,ey,er*0.7f,20,160,3,0x3A2A1E); }
    else { circ(cx-ex,ey,er,0xFFFFFF,LV_OPA_COVER); circ(cx+ex,ey,er,0xFFFFFF,LV_OPA_COVER);
        circ(cx-ex+lookx,ey+1,er*0.58f,0x2A1E16,LV_OPA_COVER); circ(cx+ex+lookx,ey+1,er*0.58f,0x2A1E16,LV_OPA_COVER);
        circ(cx-ex+lookx-er*0.2f,ey-er*0.25f,er*0.18f,0xFFFFFF,LV_OPA_COVER); circ(cx+ex+lookx-er*0.2f,ey-er*0.25f,er*0.18f,0xFFFFFF,LV_OPA_COVER);
        if(L.droopy) box((int)(cx-ex-er-1),(int)(ey-er-1),(int)(cx-ex+er+1),(int)(ey-er*0.15f),pet_look().body,pet_look().body,6),
                     box((int)(cx+ex-er-1),(int)(ey-er-1),(int)(cx+ex+er+1),(int)(ey-er*0.15f),pet_look().body,pet_look().body,6); }
    if(P.stage==PS_ELDER){ arc(cx-ex,ey-er-4,er*0.8f,200,340,3,0xFFFFFF); arc(cx+ex,ey-er-4,er*0.8f,200,340,3,0xFFFFFF); }
    circ(cx-ex-er*0.6f,ey+er*1.3f,er*0.45f,sick?0x9BC15B:0xF29A9A,LV_OPA_60);    /* cheeks */
    circ(cx+ex+er*0.6f,ey+er*1.3f,er*0.45f,sick?0x9BC15B:0xF29A9A,LV_OPA_60);
    float my=cy+ry*0.18f;
    if(eat_ms>0){ float o=fabsf(sinf(phase*6))*er*0.6f+2; ell(cx,my+er*0.4f,er*0.55f,o,0x5A2E22,LV_OPA_COVER); }
    else if(P.asleep) ell(cx,my+er*0.3f,er*0.18f,er*0.14f,0x5A2E22,LV_OPA_COVER);
    else if(happy_ms>0 || (!sad && !sick)) arc(cx,my-er*0.1f,er*0.7f,20,160,4,0x5A2E22);
    else arc(cx,my+er*0.9f,er*0.6f,205,335,4,0x5A2E22);
    if(sick){ circ(cx+rx+16,cy-ry-6,15,0xFFFFFF,LV_OPA_90); box((int)cx+(int)rx+10,(int)(cy-ry-8),(int)cx+(int)rx+22,(int)(cy-ry-4),0xE04848,0xE04848,1);
        box((int)cx+(int)rx+14,(int)(cy-ry-12),(int)cx+(int)rx+18,(int)(cy-ry),0xE04848,0xE04848,1); }
    if(pet_attention(&P)&~PA_SICK && !P.asleep){ circ(cx-rx-14,cy-ry-8,14,0xFFFFFF,LV_OPA_90); text((int)(cx-rx-18),(int)(cy-ry-19),"!",0xE8731C,&lv_font_montserrat_20); }
    if(eat_ms>0){ float k=eat_ms/1500.0f, fx=cx+rx+6, fy=my;           /* the food shrinking */
        if(eat_kind==0){ ell(fx,fy,14*k+2,10*k+2,0xC9763C,LV_OPA_COVER); circ(fx+10*k,fy-6*k,5*k+1,0xF4E4D0,LV_OPA_COVER); }
        else { circ(fx,fy,12*k+2,0xF29AB6,LV_OPA_COVER); circ(fx,fy-10*k,4*k+1,0xE04848,LV_OPA_COVER); } }
}
static void draw_egg(float cx,float base){
    float w=sinf(phase*3)*(P.stage_min>=3?6:2);
    ell(cx,base-2,34,7,0x000000,LV_OPA_20);
    ell(cx+w*0.3f,base-46,36,46,0xFDF3E0,LV_OPA_COVER);
    circ(cx-12+w*0.3f,base-60,7,0xE8731C,LV_OPA_COVER); circ(cx+14+w*0.3f,base-40,9,0x7FD6C8,LV_OPA_COVER); circ(cx-2+w*0.3f,base-22,5,0xF2B33D,LV_OPA_COVER);
}
static void scene_draw_cb(lv_event_t*e){
    DC=lv_event_get_draw_ctx(e); lv_area_t c; lv_obj_get_coords(lv_event_get_target(e),&c); OX=c.x1; OY=c.y1;
    time_t t=time(0); struct tm tm; localtime_r(&t,&tm); int h=tm.tm_hour;
    int night = h>=20||h<7, dark = P.asleep && !P.lights;
    if(dark) box(0,0,SW-1,SH-1,0x05070D,0x0B0F1A,0);
    else if(night) box(0,0,SW-1,SH-1,0x0B1530,0x2A3766,0);
    else if(h<9||h>=18) box(0,0,SW-1,SH-1,0xF2A65E,0xF9D9A6,0);                 /* dawn / dusk */
    else box(0,0,SW-1,SH-1,0x6CA8D8,0xCFEAF5,0);
    if(night && !dark){ static const short st[][2]={{30,190},{170,90},{200,30},{300,70},{430,40},{380,120},{70,150},{250,140}};
        for(int i=0;i<8;i++) circ(st[i][0],st[i][1],1.6f+((i+(int)(phase))%3==0),0xFFFFFF,LV_OPA_80);
        circ(400,70,26,0xF4E4D0,LV_OPA_COVER); circ(388,62,24,0x0B1530,LV_OPA_COVER); }        /* moon */
    else if(!dark){ circ(400,70,40,0xFFE27A,LV_OPA_30); circ(400,70,28,0xFFD34A,LV_OPA_COVER); }  /* sun */
    box(0,GROUND,SW-1,SH-1,dark?0x0E140E:night?0x24402A:0x7DBF6A,dark?0x070A07:night?0x18301E:0x5E9F4E,0);
    for(int i=0;i<P.poop && P.stage!=PS_GONE;i++){ float x=SW-40-i*34, y=GROUND+30;   /* poop piles */
        ell(x,y,14,6,0x6B4423,LV_OPA_COVER); ell(x,y-7,10,5,0x7A4E2A,LV_OPA_COVER); circ(x,y-13,4,0x7A4E2A,LV_OPA_COVER); }
    if(P.stage==PS_GONE){ circ(SW/2,120+sinf(phase)*6,10,0xFFE27A,LV_OPA_COVER); circ(SW/2,120+sinf(phase)*6,22,0xFFE27A,LV_OPA_30); }
    else if(P.stage==PS_EGG) draw_egg(px,GROUND+10);
    else draw_pet(px,GROUND+10);
    if(P.asleep && P.stage!=PS_GONE){ for(int i=0;i<3;i++){ float k=fmodf(phase*0.6f+i*0.33f,1.0f);
        text((int)(px+40+k*40),(int)(GROUND-120-k*70),"z",0xF4E4D0,i==1?&lv_font_montserrat_28:&lv_font_montserrat_20); } }
    /* need pips: food and joy */
    if(P.stage!=PS_EGG && P.stage!=PS_GONE){ text(10,8,"food",0xF4E4D0,&lv_font_montserrat_14); text(10,30,"joy",0xF4E4D0,&lv_font_montserrat_14);
        for(int i=0;i<4;i++){ circ(66+i*20,17,7,i<P.hunger?0xE8731C:0x000000,i<P.hunger?LV_OPA_COVER:LV_OPA_30);
                              circ(66+i*20,39,7,i<P.happy?0xF29AB6:0x000000,i<P.happy?LV_OPA_COVER:LV_OPA_30); } }
    for(int i=0;i<10;i++) if(bub[i].on){ circ(bub[i].x,bub[i].y,22,0xCFEAF5,LV_OPA_50); circ(bub[i].x-7,bub[i].y-8,5,0xFFFFFF,LV_OPA_90); }
    if(game_on){ char b[32]; snprintf(b,sizeof b,"popped %d  -  %ds",popped,(int)((game_end-r3_tick_ms()+999)/1000)); text(300,10,b,0xFFFFFF,&lv_font_montserrat_16); }
}

/* ---------- animation ---------- */
static void tick_cb(lv_timer_t*t){ (void)t;
    uint32_t now=r3_tick_ms(); int dt=(int)(now-last_ms); if(dt>200)dt=200; last_ms=now;
    phase+=dt/1000.0f*3.0f;
    if(toast_until && now>toast_until){ toast_until=0; lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); }
    if(eat_ms>0) eat_ms-=dt; if(happy_ms>0) happy_ms-=dt; if(blink_ms>0) blink_ms-=dt;
    if(!P.asleep && P.stage!=PS_EGG && P.stage!=PS_GONE && !eat_ms && !game_on){
        if(now>next_wander){ ptx=90+(float)(now%300); next_wander=now+3000+(now%4000); look=(int)(now/7%3)-1; }
        float d=ptx-px; px+= d>0? fminf(d,dt*0.05f) : fmaxf(d,-dt*0.05f);
        if(now>next_blink){ blink_ms=140; next_blink=now+2500+(now%3000); } }
    if(game_on){
        for(int i=0;i<10;i++) if(bub[i].on){ bub[i].y-=bub[i].v*dt/1000.0f; if(bub[i].y<-30) bub[i].on=0; }
        if(now%700<(uint32_t)dt){ for(int i=0;i<10;i++) if(!bub[i].on){ bub[i].on=1; bub[i].x=40+(float)((now*7919)%400); bub[i].y=SH+20; bub[i].v=90+(float)(now%90); break; } }
        if(now>=game_end){ game_on=0; memset(bub,0,sizeof bub); int won=popped>=5;
            pet_play_result(&P,won); P_dirty=1; happy_ms=won?2500:0;
            toast(won?"Moko had fun!":"Moko wants a rematch"); lv_obj_invalidate(s_scene); } }
    /* redraw: whole scene while playing/asleep (z's), else just around the pet */
    if(game_on || P.asleep || P.stage==PS_GONE) lv_obj_invalidate(s_scene);
    else { lv_area_t c; lv_obj_get_coords(s_scene,&c);
        lv_area_t b={c.x1+(int)px-120,c.y1+GROUND-230,c.x1+(int)px+120,c.y1+GROUND+24};
        lv_obj_invalidate_area(s_scene,&prev_box); lv_obj_invalidate_area(s_scene,&b); prev_box=b;
        lv_area_t pips={c.x1,c.y1,c.x1+160,c.y1+50}; lv_obj_invalidate_area(s_scene,&pips); }
    /* the simulation keeps going while we watch */
    static uint32_t lsim; if(now-lsim>5000){ lsim=now; int st=P.stage; pet_tick();
        if(P.events&2) toast("It hatched!"); else if(P.events&1 && P.stage!=PS_GONE){ char b[48]; snprintf(b,sizeof b,"Evolved into %s!",pet_form_name(&P)); toast(b); }
        P.events=0; if(P.stage!=st){ title(); lv_obj_invalidate(s_scene); }
        if(P.stage==PS_GONE) lv_obj_clear_flag(s_gone,LV_OBJ_FLAG_HIDDEN); }
}
static void scene_input_cb(lv_event_t*e){
    if(!game_on) return; lv_point_t p; lv_indev_get_point(lv_indev_get_act(),&p);
    lv_area_t c; lv_obj_get_coords(s_scene,&c); float x=p.x-c.x1, y=p.y-c.y1;
    for(int i=0;i<10;i++) if(bub[i].on && (bub[i].x-x)*(bub[i].x-x)+(bub[i].y-y)*(bub[i].y-y)<34*34){ bub[i].on=0; popped++; break; }
}

/* ---------- actions ---------- */
static const char*MSG[]={0,"Not now","Not needed","Shh... asleep","",""};
static void after(int r,const char*ok){ if(r==PR_OK){ if(ok) toast(ok); } else if(r<5 && MSG[r]) toast(MSG[r]);
    P_dirty=1; pet_save(); lv_obj_invalidate(s_scene); }
static void act_cb(lv_event_t*e){ long a=(long)lv_event_get_user_data(e); int r;
    if(P.stage==PS_GONE || game_on) return;
    switch(a){
    case 0: r=pet_meal(&P); if(r==PR_OK){ eat_ms=1500; eat_kind=0; } after(r,r==PR_REFUSED?"Full!":"Yum!"); if(r==PR_REFUSED) toast("Full - no more"); break;
    case 1: r=pet_snack(&P); if(r==PR_OK){ eat_ms=1500; eat_kind=1; happy_ms=2000; } after(r,"Sweet!"); break;
    case 2: if(P.asleep){ toast("Shh... asleep"); break; } if(P.stage==PS_EGG){ toast("Still an egg"); break; }
            game_on=1; popped=0; memset(bub,0,sizeof bub); game_end=r3_tick_ms()+12000; toast("Pop 5 bubbles!"); break;
    case 3: after(pet_clean(&P),"All clean"); break;
    case 4: after(pet_medicine(&P),"Feeling better"); break;
    case 5: after(pet_lights(&P),P.lights?"Lights on":"Lights off"); break;
    case 6: r=pet_scold(&P); after(r,"Behaving now"); if(r==PR_NOTNEEDED) toast("That was unfair..."); break;
    case 7: { char b[200]; snprintf(b,sizeof b,"%s (%s)\nage %d days\nweight %d\ndiscipline %d%%\ncare mistakes %d\n\ntap to close",
                pet_form_name(&P),pet_stage_name(&P),P.age_days,P.weight,P.discipline,P.mistakes_total);
              lv_label_set_text(lv_obj_get_child(s_status,0),b); lv_obj_clear_flag(s_status,LV_OBJ_FLAG_HIDDEN); } break;
    }
}
static void status_close_cb(lv_event_t*e){ (void)e; lv_obj_add_flag(s_status,LV_OBJ_FLAG_HIDDEN); }
static void new_egg_cb(lv_event_t*e){ (void)e; pet_new(&P,time(0),(unsigned)time(0)^r3_tick_ms()); pet_save();
    lv_obj_add_flag(s_gone,LV_OBJ_FLAG_HIDDEN); title(); px=ptx=SW/2; lv_obj_invalidate(s_scene); toast("A new egg!"); }
static void back_cb(lv_event_t*e){ (void)e; if(s_exit) s_exit(); }
static void del_cb(lv_event_t*e){ (void)e; pet_save(); if(s_tmr){ lv_timer_del(s_tmr); s_tmr=0; } s_scr=0; game_on=0; }

lv_obj_t* pet_create(void (*on_exit)(void)){
    s_exit=on_exit; pet_load(); pet_catch_up(&P,time(0)); P.events=0;
    lv_obj_t*scr=lv_obj_create(NULL); s_scr=scr;
    lv_obj_set_style_bg_color(scr,lv_color_hex(0x0A0A0A),0); lv_obj_clear_flag(scr,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr,del_cb,LV_EVENT_DELETE,NULL);
    lv_obj_t*bk=lv_btn_create(scr); lv_obj_set_size(bk,46,40); lv_obj_set_pos(bk,8,7);
    lv_obj_set_style_bg_color(bk,lv_color_hex(0x1A1510),0); lv_obj_set_style_radius(bk,0,0);
    lv_obj_set_style_border_width(bk,2,0); lv_obj_set_style_border_color(bk,lv_color_hex(0xE8731C),0);
    lv_obj_add_event_cb(bk,back_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t*bl=lv_label_create(bk); lv_label_set_text(bl,LV_SYMBOL_LEFT); lv_obj_set_style_text_color(bl,lv_color_hex(0xE8731C),0); lv_obj_center(bl);
    s_title=lv_label_create(scr); lv_obj_set_style_text_color(s_title,lv_color_hex(0xE8731C),0);
    lv_obj_set_style_text_font(s_title,&lv_font_montserrat_20,0); lv_obj_set_pos(s_title,64,16); title();
    s_scene=lv_obj_create(scr); lv_obj_set_size(s_scene,SW,SH); lv_obj_set_pos(s_scene,0,SY);
    lv_obj_set_style_radius(s_scene,0,0); lv_obj_set_style_border_width(s_scene,0,0); lv_obj_set_style_pad_all(s_scene,0,0);
    lv_obj_set_style_bg_opa(s_scene,LV_OPA_TRANSP,0); lv_obj_clear_flag(s_scene,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scene,scene_draw_cb,LV_EVENT_DRAW_MAIN,NULL);
    lv_obj_add_event_cb(s_scene,scene_input_cb,LV_EVENT_PRESSED,NULL);
    /* actions: 2 rows x 4 */
    static const char*NAMES[8]={"Meal","Snack","Play","Clean","Medicine","Lights","Scold","Status"};
    for(long i=0;i<8;i++){ lv_obj_t*b=lv_btn_create(scr); lv_obj_set_size(b,114,108);
        lv_obj_set_pos(b,6+(i%4)*118,SY+SH+8+(i/4)*114);
        lv_obj_set_style_radius(b,10,0); lv_obj_set_style_shadow_width(b,0,0);
        lv_obj_set_style_bg_color(b,lv_color_hex(0x1A1510),0); lv_obj_set_style_bg_color(b,lv_color_hex(0xE8731C),LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b,lv_color_hex(0x5A4632),0); lv_obj_set_style_border_width(b,2,0);
        lv_obj_add_event_cb(b,act_cb,LV_EVENT_CLICKED,(void*)i);
        lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,NAMES[i]); lv_obj_center(l);
        lv_obj_set_style_text_color(l,lv_color_hex(0xF4E4D0),0); lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0); }
    s_toast=lv_label_create(scr); lv_obj_set_width(s_toast,480); lv_obj_set_style_text_align(s_toast,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(s_toast,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(s_toast,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_pos(s_toast,0,SY+60); lv_obj_add_flag(s_toast,LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_toast,LV_OBJ_FLAG_CLICKABLE);
    /* status card + gone card */
    s_status=lv_obj_create(scr); lv_obj_set_size(s_status,360,300); lv_obj_set_pos(s_status,60,SY+60);
    lv_obj_set_style_bg_color(s_status,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s_status,LV_OPA_80,0);
    lv_obj_set_style_border_color(s_status,lv_color_hex(0xE8731C),0); lv_obj_set_style_border_width(s_status,2,0);
    lv_obj_add_event_cb(s_status,status_close_cb,LV_EVENT_CLICKED,NULL); lv_obj_add_flag(s_status,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t*sl=lv_label_create(s_status); lv_obj_set_style_text_color(sl,lv_color_hex(0xF4E4D0),0);
    lv_obj_set_style_text_font(sl,&lv_font_montserrat_20,0); lv_obj_set_style_text_align(sl,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(sl);
    s_gone=lv_obj_create(scr); lv_obj_set_size(s_gone,400,220); lv_obj_set_pos(s_gone,40,SY+170);
    lv_obj_set_style_bg_color(s_gone,lv_color_hex(0x000000),0); lv_obj_set_style_bg_opa(s_gone,LV_OPA_80,0);
    lv_obj_set_style_border_color(s_gone,lv_color_hex(0xFFE27A),0); lv_obj_set_style_border_width(s_gone,2,0);
    lv_obj_add_event_cb(s_gone,new_egg_cb,LV_EVENT_CLICKED,NULL);
    { lv_obj_t*gl=lv_label_create(s_gone); char b[160];
      snprintf(b,sizeof b,"Moko went back\nto the stars after %d days.\n\ntap for a new egg",P.age_days);
      lv_label_set_text(gl,b); lv_obj_set_style_text_color(gl,lv_color_hex(0xF4E4D0),0);
      lv_obj_set_style_text_font(gl,&lv_font_montserrat_20,0); lv_obj_set_style_text_align(gl,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(gl); }
    if(P.stage!=PS_GONE) lv_obj_add_flag(s_gone,LV_OBJ_FLAG_HIDDEN);
    last_ms=r3_tick_ms(); next_wander=last_ms+1500; next_blink=last_ms+2000; px=ptx=SW/2;
    lv_area_t z={0,0,0,0}; prev_box=z;
    s_tmr=lv_timer_create(tick_cb,60,NULL);
    return scr;
}
