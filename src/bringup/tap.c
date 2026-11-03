/* inject a tap (or long press) into the goodix evdev node: tap X Y [hold_ms] */
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
static int fd;
static void ev(int t,int c,int v){ struct input_event e; memset(&e,0,sizeof e); e.type=t;e.code=c;e.value=v; write(fd,&e,sizeof e); }
int main(int ac,char**av){ fd=open("/dev/input/event1",O_WRONLY); if(fd<0||ac<3)return 1;
  int x=atoi(av[1]),y=atoi(av[2]); int hold=ac>3?atoi(av[3]):80;
  ev(EV_ABS,ABS_MT_POSITION_X,x); ev(EV_ABS,ABS_MT_POSITION_Y,y); ev(EV_ABS,ABS_X,x); ev(EV_ABS,ABS_Y,y);
  ev(EV_KEY,BTN_TOUCH,1); ev(EV_SYN,0,0); usleep(hold*1000);
  ev(EV_KEY,BTN_TOUCH,0); ev(EV_SYN,0,0); return 0; }
