/* cover.c — album art for Now Playing.
 *
 * Why not just lv_img_set_src("S:file.jpg")?  (1) LVGL's SJPG decoder is a line reader,
 * so it can't zoom: a 500px cover was cropped to the 480x360 box. (2) the image cache
 * keys on the path, and embedded covers all went through one temp file, so switching
 * artist kept showing the previous cover.
 *
 * Here: get the JPEG bytes (folder image, ID3 APIC, FLAC PICTURE or MP4 covr), decode
 * with TJpgDec using its free 1/2^s prescale (baseline JPEG; progressive JPEG and PNG go
 * through stb_image, capped in pixels for the 56MB RAM), bilinear-fit into the box (aspect kept, no
 * crop), and hand LVGL an in-memory RGB565 image. Two slots alternate so the screen being
 * deleted can still reference the old pixels; each slot is invalidated in the cache. */
#include "cover.h"
#include "tjpgd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_JPEG (8L*1024*1024)
#define STB_MAX_PX (1600L*1600)          /* stb decodes the full image: cap its RGB buffer */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

/* ---------- locating JPEG bytes ---------- */
static unsigned char* read_file(const char*p,long*n){
    FILE*f=fopen(p,"rb"); if(!f) return 0;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0||sz>MAX_JPEG){ fclose(f); return 0; }
    unsigned char*b=malloc(sz); if(!b){ fclose(f); return 0; }
    *n=(long)fread(b,1,sz,f); fclose(f); return b;
}
static int find_cover_file(const char*dir,char*out,int cap){
    static const char*names[]={"cover.jpg","Cover.jpg","folder.jpg","Folder.jpg","front.jpg","Front.jpg","cover.jpeg",
                               "cover.png","Cover.png","folder.png","Folder.png",0};
    struct stat st;
    for(int i=0;names[i];i++){ snprintf(out,cap,"%s/%s",dir,names[i]); if(stat(out,&st)==0) return 1; }
    DIR*dp=opendir(dir); if(!dp) return 0; struct dirent*de; int ok=0;
    while(!ok && (de=readdir(dp))){ const char*e=strrchr(de->d_name,'.');
        if(de->d_name[0]!='.' && e&&(!strcasecmp(e,".jpg")||!strcasecmp(e,".jpeg")||!strcasecmp(e,".png"))){
            snprintf(out,cap,"%s/%s",dir,de->d_name); ok=1; } }
    closedir(dp); return ok;
}
/* copy the first SOI..EOI JPEG found in buf */
static unsigned char* jpeg_in(const unsigned char*buf,long n,long*outn){
    long s=-1; for(long i=0;i+2<n;i++) if(buf[i]==0xFF&&buf[i+1]==0xD8&&buf[i+2]==0xFF){ s=i; break; }
    if(s<0) return 0;
    long e=n; for(long i=n-2;i>s;i--) if(buf[i]==0xFF&&buf[i+1]==0xD9){ e=i+2; break; }
    unsigned char*o=malloc(e-s); if(!o) return 0;
    memcpy(o,buf+s,e-s); *outn=e-s; return o;
}
static unsigned long be32(const unsigned char*p){ return ((unsigned long)p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
/* MP4/M4A: ...'covr' [size]'data'[type][locale] <image>. moov is at the head or the tail. */
static unsigned char* mp4_covr(FILE*f,long fsz,long*outn){
    const long W=4L*1024*1024;
    for(int pass=0;pass<2;pass++){
        long off = pass==0 ? 0 : (fsz>W ? fsz-W : 0);
        if(pass==1 && off==0) break;              /* small file: head already covered it */
        long len = fsz-off<W ? fsz-off : W;
        unsigned char*b=malloc(len); if(!b) return 0;
        fseek(f,off,SEEK_SET); len=(long)fread(b,1,len,f);
        for(long i=4;i+24<len;i++){
            if(memcmp(b+i,"covr",4)) continue;
            const unsigned char*d=b+i+4;
            if(memcmp(d+4,"data",4)) continue;
            unsigned long dsz=be32(d), type=be32(d+8)&0xFFFFFF;
            if(dsz<16 || i+4+(long)dsz>len) continue;
            if(type!=13 && type!=14 && type!=0) continue;   /* 13 = JPEG, 14 = PNG */
            unsigned char*o;
            if(type==14){ o=malloc(dsz-16); if(o){ memcpy(o,d+16,dsz-16); *outn=(long)dsz-16; } }
            else o=jpeg_in(d+16,(long)dsz-16,outn);
            free(b); return o;
        }
        free(b);
    }
    return 0;
}
static unsigned char* embedded(const char*path,long*n){
    FILE*f=fopen(path,"rb"); if(!f) return 0;
    unsigned char hd[12]; if(fread(hd,1,12,f)!=12){ fclose(f); return 0; }
    fseek(f,0,SEEK_END); long fsz=ftell(f);
    unsigned char*r=0;
    if(hd[0]=='I'&&hd[1]=='D'&&hd[2]=='3'){                 /* ID3v2 tag body */
        long sz=((long)(hd[6]&0x7f)<<21)|((long)(hd[7]&0x7f)<<14)|((long)(hd[8]&0x7f)<<7)|(hd[9]&0x7f);
        if(sz>0&&sz<=MAX_JPEG){ unsigned char*b=malloc(sz);
            if(b){ fseek(f,10,SEEK_SET); long got=(long)fread(b,1,sz,f); r=jpeg_in(b,got,n); free(b); } }
    } else if(!memcmp(hd,"fLaC",4)){                         /* FLAC metadata blocks */
        fseek(f,4,SEEK_SET); unsigned char bh[4]; long off=4; int last=0;
        while(!last && fread(bh,1,4,f)==4){ last=bh[0]&0x80;
            long bl=((long)bh[1]<<16)|((long)bh[2]<<8)|bh[3]; off+=4+bl; fseek(f,bl,SEEK_CUR); }
        if(off>0&&off<=MAX_JPEG){ unsigned char*b=malloc(off);
            if(b){ fseek(f,0,SEEK_SET); long got=(long)fread(b,1,off,f); r=jpeg_in(b,got,n); free(b); } }
    } else if(!memcmp(hd+4,"ftyp",4)){                       /* MP4 / M4A */
        r=mp4_covr(f,fsz,n);
    }
    fclose(f); return r;
}

/* ---------- decode + fit ---------- */
typedef struct { const unsigned char*src; long n,pos; uint16_t*px; int w,h; } ctx_t;
static size_t in_fn(JDEC*jd,uint8_t*buf,size_t nd){ ctx_t*c=jd->device;
    long left=c->n-c->pos; if((long)nd>left) nd=(size_t)left;
    if(buf) memcpy(buf,c->src+c->pos,nd); c->pos+=(long)nd; return nd; }
static int out_fn(JDEC*jd,void*bmp,JRECT*r){ ctx_t*c=jd->device; const uint8_t*s=bmp;
    for(int y=r->top;y<=r->bottom;y++) for(int x=r->left;x<=r->right;x++,s+=3)
        if(x<c->w && y<c->h) c->px[y*c->w+x]=(uint16_t)(((s[0]&0xF8)<<8)|((s[1]&0xFC)<<3)|(s[2]>>3));
    return 1; }

static uint16_t* fit(const uint16_t*sp,int sw,int sh,int dw,int dh){
    uint16_t*d=malloc((size_t)dw*dh*2); if(!d) return 0;
    for(int y=0;y<dh;y++){
        int fy=(int)(((long)y*(sh-1)<<8)/(dh>1?dh-1:1)); int y0=fy>>8, wy=fy&255, y1=y0+1<sh?y0+1:y0;
        for(int x=0;x<dw;x++){
            int fx=(int)(((long)x*(sw-1)<<8)/(dw>1?dw-1:1)); int x0=fx>>8, wx=fx&255, x1=x0+1<sw?x0+1:x0;
            uint16_t a=sp[y0*sw+x0],b=sp[y0*sw+x1],c=sp[y1*sw+x0],e=sp[y1*sw+x1];
            #define CH(v,sh_,m) (((v)>>(sh_))&(m))
            #define LERP(p,q,w) ((p)+((((q)-(p))*(w))>>8))
            int rr=LERP(LERP(CH(a,11,31),CH(b,11,31),wx),LERP(CH(c,11,31),CH(e,11,31),wx),wy);
            int gg=LERP(LERP(CH(a,5,63),CH(b,5,63),wx),LERP(CH(c,5,63),CH(e,5,63),wx),wy);
            int bb=LERP(LERP(CH(a,0,31),CH(b,0,31),wx),LERP(CH(c,0,31),CH(e,0,31),wx),wy);
            d[y*dw+x]=(uint16_t)((rr<<11)|(gg<<5)|bb);
        }
    }
    return d;
}

static lv_img_dsc_t slot[2]; static int cur_slot;

const lv_img_dsc_t* cover_load(const char*dir,const char*track,int boxw,int boxh){
    long n=0; unsigned char*jpg=0; char p[1600];
    if(dir && find_cover_file(dir,p,sizeof p)) jpg=read_file(p,&n);
    if(!jpg && track) jpg=embedded(track,&n);
    if(!jpg) return 0;

    uint16_t*src=0; int sw=0,sh=0, dw=0,dh=0;
    static uint8_t pool[16384]; JDEC jd; ctx_t c={jpg,n,0,0,0,0};
    if(jd_prepare(&jd,in_fn,pool,sizeof pool,&c)==JDR_OK){      /* baseline JPEG */
        int W=jd.width,H=jd.height;
        dw=boxw; dh=(int)((long)H*boxw/W); if(dh>boxh){ dh=boxh; dw=(int)((long)W*boxh/H); }
        int s=0; while(s<3 && (W>>(s+1))>=dw && (H>>(s+1))>=dh) s++;   /* cheapest prescale */
        c.w=(W+(1<<s)-1)>>s; c.h=(H+(1<<s)-1)>>s;
        c.px=calloc((size_t)c.w*c.h,2);
        if(c.px && jd_decomp(&jd,out_fn,(uint8_t)s)==JDR_OK){ src=c.px; sw=c.w; sh=c.h; }
        else free(c.px);
    }
    if(!src){                                                    /* progressive JPEG / PNG */
        int W,H,comp;
        if(stbi_info_from_memory(jpg,(int)n,&W,&H,&comp) && W>0 && H>0 && (long)W*H<=STB_MAX_PX){
            unsigned char*rgb=stbi_load_from_memory(jpg,(int)n,&W,&H,&comp,3);
            if(rgb){ src=malloc((size_t)W*H*2);
                if(src){ for(long i=0;i<(long)W*H;i++){ const unsigned char*q=rgb+i*3;
                        src[i]=(uint16_t)(((q[0]&0xF8)<<8)|((q[1]&0xFC)<<3)|(q[2]>>3)); }
                    sw=W; sh=H;
                    dw=boxw; dh=(int)((long)H*boxw/W); if(dh>boxh){ dh=boxh; dw=(int)((long)W*boxh/H); } }
                stbi_image_free(rgb); }
        }
    }
    free(jpg);
    if(!src || dw<1 || dh<1){ free(src); return 0; }
    uint16_t*o=fit(src,sw,sh,dw,dh); free(src);
    if(!o) return 0;

    cur_slot^=1; lv_img_dsc_t*d=&slot[cur_slot];
    lv_img_cache_invalidate_src(d);
    free((void*)d->data);
    memset(d,0,sizeof *d);
    d->header.cf=LV_IMG_CF_TRUE_COLOR; d->header.w=dw; d->header.h=dh;
    d->data_size=(uint32_t)dw*dh*2; d->data=(const uint8_t*)o;
    return d;
}
