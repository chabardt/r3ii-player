/* inject input for testing without hands:
 *   tap X Y [hold_ms] [X2 Y2]   touch tap / long press / drag (X2 Y2: drag over hold_ms)
 *   tap -k CODE [DEV]           press+release a key (default /dev/input/event2: vol/play/next;
 *                               event0 = power/prev). 115 vol+, 114 vol-, 164 play, 163 next */
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
static int fd;
static void ev(int t,int c,int v){ struct input_event e; memset(&e,0,sizeof e); e.type=t;e.code=c;e.value=v; write(fd,&e,sizeof e); }
static void pos(int x,int y){ ev(EV_ABS,ABS_MT_POSITION_X,x); ev(EV_ABS,ABS_MT_POSITION_Y,y); ev(EV_ABS,ABS_X,x); ev(EV_ABS,ABS_Y,y); }
int main(int ac,char**av){
  if(ac>2 && !strcmp(av[1],"-k")){
    fd=open(ac>3?av[3]:"/dev/input/event2",O_WRONLY); if(fd<0) return 1;
    int c=atoi(av[2]); ev(EV_KEY,c,1); ev(EV_SYN,0,0); usleep(ac>4?atoi(av[4])*1000:60000); ev(EV_KEY,c,0); ev(EV_SYN,0,0); return 0; }
  if(ac>3 && !strcmp(av[1],"-a")){   /* -a CODE VALUE DEV [hold_ms]: axis to VALUE, then back to 0 */
    fd=open(av[4],O_WRONLY); if(fd<0) return 1; int c=atoi(av[2]);
    ev(EV_ABS,c,atoi(av[3])); ev(EV_SYN,0,0); usleep(ac>5?atoi(av[5])*1000:150000); ev(EV_ABS,c,0); ev(EV_SYN,0,0); return 0; }
  fd=open("/dev/input/event1",O_WRONLY); if(fd<0||ac<3)return 1;
  int x=atoi(av[1]),y=atoi(av[2]); int hold=ac>3?atoi(av[3]):80;
  pos(x,y); ev(EV_KEY,BTN_TOUCH,1); ev(EV_SYN,0,0);
  if(ac>5){ int x2=atoi(av[4]),y2=atoi(av[5]);
    for(int i=1;i<=20;i++){ usleep(hold*1000/20); pos(x+(x2-x)*i/20,y+(y2-y)*i/20); ev(EV_SYN,0,0); }
    usleep(60000);
  } else usleep(hold*1000);
  ev(EV_KEY,BTN_TOUCH,0); ev(EV_SYN,0,0); return 0; }
