/* r3ii-player bring-up test: verify framebuffer + Goodix touch on HiBy R3 Pro II.
 * Ingenic X1600, 480x720 RGB565. Draws a retro test pattern and tracks touch.
 * Usage: fbtest [seconds]   (default 40)  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <time.h>

static inline unsigned short rgb565(int r,int g,int b){
    return (unsigned short)(((r&0xf8)<<8)|((g&0xfc)<<3)|(b>>3));
}

int main(int argc,char**argv){
    int fb=open("/dev/fb0",O_RDWR);
    if(fb<0){perror("open /dev/fb0");return 1;}
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if(ioctl(fb,FBIOGET_VSCREENINFO,&v)||ioctl(fb,FBIOGET_FSCREENINFO,&f)){perror("ioctl");return 1;}
    int W=v.xres,H=v.yres,stride=f.line_length;
    printf("fb: %dx%d (virt %dx%d) bpp=%d stride=%d smem=%u\n",
           W,H,v.xres_virtual,v.yres_virtual,v.bits_per_pixel,stride,f.smem_len);
    fflush(stdout);
    unsigned char*m=mmap(0,f.smem_len,PROT_READ|PROT_WRITE,MAP_SHARED,fb,0);
    if(m==MAP_FAILED){perror("mmap");return 1;}

    /* retro palette vertical bars */
    unsigned short pal[6]={rgb565(242,233,216),rgb565(47,143,143),rgb565(224,122,63),
                           rgb565(156,59,78),rgb565(230,182,76),rgb565(36,31,28)};
    for(int y=0;y<H;y++){unsigned short*row=(unsigned short*)(m+(long)y*stride);
        for(int x=0;x<W;x++) row[x]=pal[(x*6)/W];}
    /* center block so we can confirm orientation/geometry */
    for(int y=H/2-50;y<H/2+50;y++)for(int x=W/2-90;x<W/2+90;x++)
        ((unsigned short*)(m+(long)y*stride))[x]=rgb565(224,122,63);
    printf("drew test pattern\n");fflush(stdout);

    int ts=open("/dev/input/event1",O_RDONLY); /* goodix-ts */
    if(ts<0)perror("open event1"); else printf("event1 opened ok\n");
    fflush(stdout);
    struct input_event ev; int tx=-1,ty=-1,down=0,nraw=0;
    int secs=(argc>1)?atoi(argv[1]):40; time_t start=time(0);
    while(time(0)-start<secs){
        if(ts<0){sleep(1);continue;}
        fd_set s;FD_ZERO(&s);FD_SET(ts,&s);struct timeval tv={1,0};
        if(select(ts+1,&s,0,0,&tv)<=0)continue;
        if(read(ts,&ev,sizeof ev)!=sizeof ev)continue;
        if(nraw<200 && ev.type!=EV_SYN){ /* log raw events to learn the protocol */
            printf("raw type=%d code=0x%x val=%d\n",ev.type,ev.code,ev.value);fflush(stdout);nraw++;
        }
        if(ev.type==EV_ABS){
            if(ev.code==ABS_MT_POSITION_X||ev.code==ABS_X)tx=ev.value;
            else if(ev.code==ABS_MT_POSITION_Y||ev.code==ABS_Y)ty=ev.value;
        }else if(ev.type==EV_KEY&&ev.code==BTN_TOUCH)down=ev.value;
        else if(ev.type==EV_SYN&&tx>=0&&ty>=0){
            printf("touch x=%d y=%d down=%d\n",tx,ty,down);fflush(stdout);
            int cx=tx<10?10:(tx>W-10?W-10:tx), cy=ty<10?10:(ty>H-10?H-10:ty);
            for(int y=cy-10;y<cy+10;y++)for(int x=cx-10;x<cx+10;x++)
                ((unsigned short*)(m+(long)y*stride))[x]=rgb565(47,143,143);
        }
    }
    munmap(m,f.smem_len);close(fb);if(ts>=0)close(ts);
    printf("fbtest done\n");return 0;
}
