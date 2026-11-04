#ifndef R3_VOLBAR_H
#define R3_VOLBAR_H
/* The r3player volume overlay (black strip, orange bottom edge, speaker icon, orange bar)
 * for the framebuffer games (r3gb, r3doom), plus the shared volume value.
 *  - volume is 0..255 "loudness", saved in /usr/data/r3_vol (r3player reads it back);
 *  - the DAC "Playback Volume" is attenuation: hw = 255 - vol.
 * rot = 0: strip along the top of the portrait screen; rot = 1: the R3 is turned
 * clockwise (landscape), so the viewer's top is the physical left edge. */
#include <stdint.h>
#include <stdio.h>

#define VB_FILE "/usr/data/r3_vol"
#define VB_W 480
#define VB_H 720
#define VB_THICK 54

static inline int vb_load(void){ int v=160; FILE*f=fopen(VB_FILE,"r"); if(f){ if(fscanf(f,"%d",&v)!=1) v=160; fclose(f); }
    return v<0?0:v>255?255:v; }
static inline void vb_save(int v){ FILE*f=fopen(VB_FILE,"w"); if(f){ fprintf(f,"%d\n",v); fclose(f); } }
static inline uint16_t vb_rgb(uint32_t c){ return (uint16_t)(((c>>8)&0xF800)|((c>>5)&0x07E0)|((c>>3)&0x1F)); }

/* draw in "viewer" coordinates (u along the strip 0..W-1, w across it 0..THICK-1) */
static inline void vb_px(uint16_t*fb,int stride,int rot,int u,int w,uint16_t c){
    int x,y; if(!rot){ x=u; y=w; } else { x=w; y=VB_H-1-u; }
    if(x>=0&&x<VB_W&&y>=0&&y<VB_H) fb[y*stride+x]=c; }
static inline void vb_rect(uint16_t*fb,int stride,int rot,int u0,int w0,int lu,int lw,uint16_t c){
    for(int w=w0;w<w0+lw;w++) for(int u=u0;u<u0+lu;u++) vb_px(fb,stride,rot,u,w,c); }

static inline void vb_draw(uint16_t*fb,int stride,int rot,int vol){
    int L = rot?VB_H:VB_W;                        /* strip length */
    uint16_t bg=vb_rgb(0x000000), acc=vb_rgb(0xE8731C), track=vb_rgb(0x1A1510);
    vb_rect(fb,stride,rot,0,0,L,VB_THICK-2,bg);
    vb_rect(fb,stride,rot,0,VB_THICK-2,L,2,acc);  /* orange bottom edge */
    /* speaker: body + cone + two waves */
    int cx=14, cy=VB_THICK/2-1;
    vb_rect(fb,stride,rot,cx,cy-5,6,10,acc);
    for(int i=0;i<8;i++) vb_rect(fb,stride,rot,cx+6+i,cy-5-i,1,10+2*i,acc);
    for(int a=0;a<2;a++){ int r=6+a*5; for(int d=-r;d<=r;d++){ int e=(r*r-d*d); int s=0; while((s+1)*(s+1)<=e) s++;
        vb_px(fb,stride,rot,cx+16+s,cy+d,acc); vb_px(fb,stride,rot,cx+17+s,cy+d,acc); } }
    /* bar: track + fill (like r3player: 390 x 14, right-aligned, 14 px margin) */
    int bl=L-90, bu=L-14-bl, bw=cy-6;
    vb_rect(fb,stride,rot,bu,bw,bl,14,track);
    vb_rect(fb,stride,rot,bu,bw,bl*vol/255,14,acc);
}
/* after hiding: clear the strip (the game redraws its own pixels on the next frame) */
static inline void vb_erase(uint16_t*fb,int stride,int rot){
    vb_rect(fb,stride,rot,0,0,rot?VB_H:VB_W,VB_THICK,0); }
#endif
