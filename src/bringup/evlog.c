/* evlog DEV SECONDS — print evdev events (type code value) with a timestamp; for
 * mapping controller buttons. e.g. evlog /dev/input/event4 90 > /tmp/ev.log */
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
int main(int ac,char**av){
    if(ac<2) return 1;
    int fd=open(av[1],O_RDONLY); if(fd<0){ perror(av[1]); return 1; }
    int secs=ac>2?atoi(av[2]):60; time_t end=time(0)+secs; struct input_event e;
    setvbuf(stdout,NULL,_IOLBF,0);
    while(time(0)<end){ struct pollfd p={fd,POLLIN,0}; if(poll(&p,1,500)<=0) continue;
        if(read(fd,&e,sizeof e)!=sizeof e) break;
        if(e.type==EV_SYN||e.type==EV_MSC) continue;
        printf("%ld.%03ld type %d code 0x%x value %d\n",(long)e.input_event_sec,(long)e.input_event_usec/1000,e.type,e.code,e.value); }
    return 0;
}
