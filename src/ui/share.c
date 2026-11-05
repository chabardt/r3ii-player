/* share.c — LAN discovery + tiny HTTP client (see share.h). */
#include "share.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DISC_PORT 8201
#define MAXPEER   16

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static peer_t P[MAXPEER]; static long Pseen[MAXPEER]; static int NP;
static long nows(void){ return (long)time(0); }

static void* disc_thread(void*a){ (void)a;
    int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0) return 0;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof one);
    struct sockaddr_in sa={0}; sa.sin_family=AF_INET; sa.sin_port=htons(DISC_PORT); sa.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(fd,(struct sockaddr*)&sa,sizeof sa)<0){ close(fd); return 0; }
    for(;;){
        char buf[256]; struct sockaddr_in from; socklen_t fl=sizeof from;
        int n=(int)recvfrom(fd,buf,sizeof buf-1,0,(struct sockaddr*)&from,&fl);
        if(n<=0) continue; buf[n]=0;
        if(strncmp(buf,"R3PLAY\t",7)) continue;
        char name[48]={0}; int port=0;
        if(sscanf(buf+7,"%47[^\t]\t%d",name,&port)!=2) continue;
        char ip[20]; inet_ntop(AF_INET,&from.sin_addr,ip,sizeof ip);
        pthread_mutex_lock(&mu);
        int idx=-1; for(int i=0;i<NP;i++) if(!strcmp(P[i].ip,ip)){ idx=i; break; }
        if(idx<0 && NP<MAXPEER) idx=NP++;
        if(idx>=0){ snprintf(P[idx].ip,sizeof P[idx].ip,"%s",ip);
                    snprintf(P[idx].name,sizeof P[idx].name,"%s",name);
                    P[idx].port=port; Pseen[idx]=nows(); }
        pthread_mutex_unlock(&mu);
    }
    return 0;
}
void share_start(void){
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at,64*1024);
    pthread_create(&t,&at,disc_thread,0); pthread_attr_destroy(&at); pthread_detach(t);
}
int share_peers(peer_t*out,int cap){ long now=nows(); int k=0;
    pthread_mutex_lock(&mu);
    for(int i=0;i<NP && k<cap;i++) if(now-Pseen[i]<=20) out[k++]=P[i];
    pthread_mutex_unlock(&mu); return k;
}

/* connect + send request, return the socket positioned at the start of the body
 * (headers consumed), with any already-read body bytes in pre[0..*prelen]. -1 on error. */
static int http_open(const char*ip,int port,const char*path,char*pre,int precap,int*prelen){
    int s=socket(AF_INET,SOCK_STREAM,0); if(s<0) return -1;
    struct timeval tv={8,0}; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(port);
    if(inet_pton(AF_INET,ip,&a.sin_addr)!=1){ close(s); return -1; }
    if(connect(s,(struct sockaddr*)&a,sizeof a)<0){ close(s); return -1; }
    char req[1200]; int rl=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",path,ip);
    if(write(s,req,rl)!=rl){ close(s); return -1; }
    /* read until we pass the "\r\n\r\n" header terminator */
    char h[4096]; int hl=0;
    for(;;){ int n=(int)read(s,h+hl,sizeof h-1-hl); if(n<=0){ close(s); return -1; } hl+=n; h[hl]=0;
        char*e=strstr(h,"\r\n\r\n");
        if(e){ /* check 2xx status */
            if(strncmp(h,"HTTP/1.",7)||(h[9]!='2')){ close(s); return -1; }
            int off=(int)(e-h)+4; *prelen=hl-off; if(*prelen>precap)*prelen=precap;
            memcpy(pre,h+off,*prelen); return s; }
        if(hl>=(int)sizeof h-1){ close(s); return -1; } }
}
int share_get(const char*ip,int port,const char*path,char*out,int cap){
    char pre[4096]; int prelen=0;
    int s=http_open(ip,port,path,pre,sizeof pre,&prelen); if(s<0) return -1;
    int total=0;
    if(prelen>0){ int c=prelen>cap?cap:prelen; memcpy(out,pre,c); total=c; }
    while(total<cap){ int n=(int)read(s,out+total,cap-total); if(n<=0) break; total+=n; }
    close(s); if(total<cap) out[total]=0; return total;
}
static void mkparents(const char*path){
    char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char*p=tmp+1;*p;p++) if(*p=='/'){ *p=0; mkdir(tmp,0777); *p='/'; }
}
int share_download(const char*ip,int port,const char*path,const char*outfile){
    char pre[4096]; int prelen=0;
    int s=http_open(ip,port,path,pre,sizeof pre,&prelen); if(s<0) return -1;
    mkparents(outfile);
    FILE*f=fopen(outfile,"wb"); if(!f){ close(s); return -1; }
    if(prelen>0) fwrite(pre,1,prelen,f);
    char rb[8192]; int n;
    while((n=(int)read(s,rb,sizeof rb))>0) fwrite(rb,1,n,f);
    fclose(f); close(s); return 0;
}
