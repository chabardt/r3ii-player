/* wifi.c — see wifi.h */
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>

extern char **environ;
/* children must not inherit our fds (framebuffer, touch, audio): daemons would hold them forever.
 * musl ignores close errors on unused numbers. */
static void fa_init(posix_spawn_file_actions_t*fa){ posix_spawn_file_actions_init(fa); }
static void fa_close_rest(posix_spawn_file_actions_t*fa){   /* last: after the dup2/open actions */
    for(int fd=3;fd<128;fd++) posix_spawn_file_actions_addclose(fa,fd); }
#define IFACE   "wlan0"
#define CONF    "/usr/data/wpa_supplicant.conf"
#define PREFS   "/usr/data/r3_wifi"
#define SRV_BIN "/usr/data/r3srv"
#define SRV_STATUS "/tmp/r3srv.status"

static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv=PTHREAD_COND_INITIALIZER;
static volatile int g_ver=1, g_busy, g_on, g_srv_on;
static char g_state[24], g_ssid[33], g_ip[20], g_msg[64];
static wifi_net_t g_net[WIFI_MAX]; static int g_nnet;
static pid_t g_srv_pid; static int g_streams;

enum { J_INIT=1, J_ON=2, J_OFF=4, J_SCAN=8, J_CONNECT=16, J_FORGET=32, J_POLL=64, J_SRV=128 };
static int g_jobs; static char g_c_ssid[33], g_c_psk[64], g_f_ssid[33];

static void bump(void){ g_ver++; }
static void msg(const char*s){ pthread_mutex_lock(&mu); snprintf(g_msg,sizeof g_msg,"%s",s); bump(); pthread_mutex_unlock(&mu); }

/* run argv (no shell), capture stdout. out==NULL: output to /dev/null and no pipe — daemons
 * (wpa_supplicant -B) keep inherited fds open, so reading a pipe would never see EOF. */
static int run(char*const argv[],char*out,int cap){
    if(!out){ posix_spawn_file_actions_t fa; fa_init(&fa);
        posix_spawn_file_actions_addopen(&fa,0,"/dev/null",O_RDONLY,0);
        posix_spawn_file_actions_addopen(&fa,1,"/dev/null",O_WRONLY,0);
        posix_spawn_file_actions_addopen(&fa,2,"/dev/null",O_WRONLY,0);
        fa_close_rest(&fa);
        pid_t pid; int rc=posix_spawnp(&pid,argv[0],&fa,NULL,argv,environ);
        posix_spawn_file_actions_destroy(&fa); if(rc) return -1; waitpid(pid,NULL,0); return 0; }
    int pf[2]; if(pipe2(pf,O_CLOEXEC)) return -1;   /* other threads spawning now must not inherit it */
    posix_spawn_file_actions_t fa; fa_init(&fa);
    posix_spawn_file_actions_adddup2(&fa,pf[1],1); posix_spawn_file_actions_addopen(&fa,2,"/dev/null",O_WRONLY,0);
    fa_close_rest(&fa);
    pid_t pid; int rc=posix_spawnp(&pid,argv[0],&fa,NULL,argv,environ);
    posix_spawn_file_actions_destroy(&fa); close(pf[1]);
    int n=0; char tmp[512]; ssize_t r;
    while((r=read(pf[0],tmp,sizeof tmp))>0) for(ssize_t i=0;i<r;i++) if(out && n<cap-1) out[n++]=tmp[i];
    close(pf[0]); if(out&&cap) out[n]=0;
    if(rc) return -1; waitpid(pid,NULL,0); return 0;   /* SIGCHLD ignored: no exit status, just wait */
}
#define RUN(out,cap,...) do{ char*const _a[]={__VA_ARGS__,NULL}; run(_a,out,cap); }while(0)
static void cli(char*out,int cap,const char*a,const char*b,const char*c,const char*d){
    char*v[8]={"wpa_cli","-i",IFACE}; int n=3; const char*x[4]={a,b,c,d};
    for(int i=0;i<4 && x[i];i++) v[n++]=(char*)x[i];
    v[n]=NULL; run(v,out,cap); }
static int cli_ok(const char*a,const char*b,const char*c,const char*d){ char o[64]; cli(o,sizeof o,a,b,c,d); return !strncmp(o,"OK",2); }

/* wpa_cli prints SSIDs escaped (\xNN, \\, \") */
static void unescape(const char*s,char*o,int cap){ int n=0;
    while(*s && n<cap-1){
        if(s[0]=='\\' && s[1]=='x' && isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3])){
            char h[3]={s[2],s[3],0}; o[n++]=(char)strtol(h,0,16); s+=4; }
        else if(s[0]=='\\' && s[1]){ o[n++]=s[1]; s+=2; }
        else o[n++]=*s++; }
    o[n]=0; }
static void hex(const char*s,char*o,int cap){ int n=0; for(;*s && n<cap-3;s++) n+=snprintf(o+n,cap-n,"%02x",(unsigned char)*s); o[n]=0; }

static void prefs_save(void){ FILE*f=fopen(PREFS,"w"); if(!f) return; fprintf(f,"on=%d\nserver=%d\n",g_on,g_srv_on); fclose(f); }
static void prefs_load(void){ FILE*f=fopen(PREFS,"r"); if(!f) return; char l[32]; int v;
    while(fgets(l,sizeof l,f)){ if(sscanf(l,"on=%d",&v)==1) g_on=v; if(sscanf(l,"server=%d",&v)==1) g_srv_on=v; } fclose(f); }

/* ---- music server child ---- */
/* r3player runs with SIGCHLD=SIG_IGN, so children are reaped by the kernel: alive = kill(pid,0) works */
static int srv_alive(void){ return g_srv_pid>0 && kill(g_srv_pid,0)==0; }
static void srv_reap(void){ if(g_srv_pid>0 && !srv_alive()){ g_srv_pid=0; g_streams=0; bump(); } }
static void srv_stop(void){
    if(g_srv_pid>0){ kill(g_srv_pid,SIGTERM);                       /* r3srv says byebye on SSDP */
        for(int i=0;i<20 && srv_alive();i++) usleep(50000);
        if(srv_alive()) kill(g_srv_pid,SIGKILL);
        g_srv_pid=0; g_streams=0; unlink(SRV_STATUS); bump(); } }
static void srv_start(void){
    struct stat st; if(g_srv_pid>0 || stat(SRV_BIN,&st)) return;
    char*const v[]={SRV_BIN,"-s",SRV_STATUS,NULL};
    posix_spawn_file_actions_t fa; fa_init(&fa);
    posix_spawn_file_actions_addopen(&fa,0,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&fa,1,"/tmp/r3srv.log",O_WRONLY|O_CREAT|O_TRUNC,0644);
    posix_spawn_file_actions_adddup2(&fa,1,2);
    fa_close_rest(&fa);
    if(posix_spawn(&g_srv_pid,SRV_BIN,&fa,NULL,v,environ)) g_srv_pid=0;
    posix_spawn_file_actions_destroy(&fa); bump(); }
static void srv_sync(void){                 /* run it exactly while: switch on + address */
    srv_reap();
    int want = g_on && g_srv_on && g_ip[0];
    if(want && g_srv_pid<=0) srv_start(); else if(!want && g_srv_pid>0) srv_stop();
    if(g_srv_pid>0){ FILE*f=fopen(SRV_STATUS,"r"); int s=0; if(f){ if(fscanf(f,"streams=%d",&s)!=1) s=0; fclose(f); }
        if(s!=g_streams){ g_streams=s; bump(); } } }

/* ---- radio ---- */
static int wpa_running(void){ char o[16]; cli(o,sizeof o,"ping",0,0,0); return !strncmp(o,"PONG",4); }
static void do_on(void){
    msg("Starting Wi-Fi...");
    mkdir("/var/run/wpa_supplicant",0755);
    RUN(0,0,"ifconfig",IFACE,"up");
    if(!wpa_running()) RUN(0,0,"wpa_supplicant","-B","-i",IFACE,"-c",CONF,"-P","/tmp/r3wpa.pid");
    for(int i=0;i<20 && !wpa_running();i++) usleep(100000);
    cli_ok("enable_network","all",0,0);     /* the stock firmware leaves old networks disabled */
    cli_ok("scan",0,0,0); msg("");
}
static void do_off(void){
    srv_stop();
    if(wpa_running()) cli_ok("terminate",0,0,0);
    RUN(0,0,"killall","udhcpc");
    RUN(0,0,"ifconfig",IFACE,"0.0.0.0","down");
    pthread_mutex_lock(&mu); g_state[0]=g_ssid[0]=g_ip[0]=0; g_nnet=0; bump(); pthread_mutex_unlock(&mu);
}
static void do_dhcp(void){
    msg("Getting an address...");
    RUN(0,0,"udhcpc","-i",IFACE,"-n","-q","-t","4","-T","2");
    msg(""); }
/* saved networks (list_networks) + scan results into g_net */
static void do_list(int with_scan){
    wifi_net_t n[WIFI_MAX]; int k=0; char *buf=malloc(8192), *save, *line; if(!buf) return;
    cli(buf,8192,"list_networks",0,0,0);
    for(line=strtok_r(buf,"\n",&save);line;line=strtok_r(0,"\n",&save)){
        if(!isdigit((unsigned char)line[0]) || k>=WIFI_MAX) continue;
        char*f[4]={0}; int nf=0; char*p=line; f[nf++]=p;
        while(*p && nf<4){ if(*p=='\t'){ *p=0; f[nf++]=p+1; } p++; }
        if(nf<2 || !f[1][0]) continue;
        memset(&n[k],0,sizeof n[k]); n[k].id=atoi(f[0]); n[k].saved=1; n[k].secured=1;
        unescape(f[1],n[k].ssid,sizeof n[k].ssid); n[k].current= nf>3 && strstr(f[3],"CURRENT"); k++; }
    if(with_scan){
        cli(buf,8192,"scan_results",0,0,0);
        for(line=strtok_r(buf,"\n",&save);line;line=strtok_r(0,"\n",&save)){
            char*f[5]={0}; int nf=0; char*p=line; f[nf++]=p;
            while(*p && nf<5){ if(*p=='\t'){ *p=0; f[nf++]=p+1; } p++; }
            if(nf<5 || !f[4][0] || strlen(f[0])!=17) continue;      /* header / hidden networks */
            char ss[33]; unescape(f[4],ss,sizeof ss); int sig=atoi(f[2]), sec=strstr(f[3],"WPA")||strstr(f[3],"WEP")||strstr(f[3],"SAE");
            int j=0; while(j<k && strcmp(n[j].ssid,ss)) j++;
            if(j==k){ if(k>=WIFI_MAX) continue; memset(&n[k],0,sizeof n[k]); snprintf(n[k].ssid,sizeof n[k].ssid,"%s",ss); n[k].id=-1; k++; }
            if(!n[j].signal || sig>n[j].signal) n[j].signal=sig;
            n[j].secured=sec; } }
    free(buf);
    /* current first, then by signal (unseen saved ones last) */
    for(int a=1;a<k;a++) for(int b=a;b>0;b--){
        wifi_net_t*x=&n[b-1],*y=&n[b];
        int kx=x->current?1000:(x->signal?x->signal+200:0), ky=y->current?1000:(y->signal?y->signal+200:0);
        if(ky>kx){ wifi_net_t t=*x; *x=*y; *y=t; } else break; }
    pthread_mutex_lock(&mu);
    if(!with_scan){   /* keep the signal levels from the last scan */
        for(int a=0;a<k;a++) for(int b=0;b<g_nnet;b++) if(!strcmp(n[a].ssid,g_net[b].ssid)){ n[a].signal=g_net[b].signal; n[a].secured=g_net[b].secured; }
        for(int b=0;b<g_nnet && k<WIFI_MAX;b++) if(!g_net[b].saved){ int dup=0; for(int a=0;a<k;a++) if(!strcmp(n[a].ssid,g_net[b].ssid)) dup=1;
            if(!dup){ n[k]=g_net[b]; n[k].current=0; k++; } } }
    memcpy(g_net,n,sizeof(wifi_net_t)*k); g_nnet=k; bump(); pthread_mutex_unlock(&mu);
}
static void do_poll(void){
    char o[1024]; cli(o,sizeof o,"status",0,0,0);
    char st[24]="", ss[33]="", ip[20]="", *p;
    if((p=strstr(o,"wpa_state="))) sscanf(p+10,"%23[^\n]",st);
    if((p=strstr(o,"\nssid="))){ char raw[100]; if(sscanf(p+6,"%99[^\n]",raw)==1) unescape(raw,ss,sizeof ss); }
    if((p=strstr(o,"ip_address="))) sscanf(p+11,"%19[^\n]",ip);
    if(!strcmp(st,"COMPLETED")){
        static char dhcp_for[33]; static time_t last_try;
        if((!ip[0] || strcmp(dhcp_for,ss)) && time(0)-last_try>=8){ last_try=time(0);
            snprintf(dhcp_for,sizeof dhcp_for,"%s",ss); do_dhcp();
            cli(o,sizeof o,"status",0,0,0); if((p=strstr(o,"ip_address="))) sscanf(p+11,"%19[^\n]",ip); }
    } else if(!strcmp(st,"DISCONNECTED")||!strcmp(st,"INACTIVE")||!strcmp(st,"INTERFACE_DISABLED")) ip[0]=0;
    else if(!ip[0]) snprintf(ip,sizeof ip,"%s",g_ip);   /* only this worker writes g_ip */
    /* else (SCANNING / AUTHENTICATING... while a scan or roam runs) keep the address: a scan
     * on the Wi-Fi page must not bounce the music server and cut listeners */
    pthread_mutex_lock(&mu);
    int ch=strcmp(st,g_state)||strcmp(ss,g_ssid)||strcmp(ip,g_ip);
    snprintf(g_state,sizeof g_state,"%s",st); snprintf(g_ssid,sizeof g_ssid,"%s",ss); snprintf(g_ip,sizeof g_ip,"%s",ip);
    if(ch) bump(); pthread_mutex_unlock(&mu);
    if(ch) do_list(0);
}
static int find_id(const char*ssid){ int id=-1; pthread_mutex_lock(&mu);
    for(int i=0;i<g_nnet;i++) if(g_net[i].saved && !strcmp(g_net[i].ssid,ssid)) id=g_net[i].id; pthread_mutex_unlock(&mu); return id; }
static void do_connect(const char*ssid,const char*psk){
    char ids[12]; int id=find_id(ssid);
    if(id>=0 && psk[0]){ snprintf(ids,sizeof ids,"%d",id); cli_ok("remove_network",ids,0,0); id=-1; }   /* new password */
    if(id<0){
        char o[32]; cli(o,sizeof o,"add_network",0,0,0); if(!isdigit((unsigned char)o[0])){ msg("Could not add the network"); return; }
        id=atoi(o); snprintf(ids,sizeof ids,"%d",id);
        char h[80]; hex(ssid,h,sizeof h); cli_ok("set_network",ids,"ssid",h);
        cli_ok("set_network",ids,"scan_ssid","1");
        if(psk[0]){ char q[80]; snprintf(q,sizeof q,"\"%s\"",psk); cli_ok("set_network",ids,"psk",q); }
        else cli_ok("set_network",ids,"key_mgmt","NONE");
    } else snprintf(ids,sizeof ids,"%d",id);
    msg("Connecting...");
    cli_ok("select_network",ids,0,0);
    for(int i=0;i<30;i++){ char o[512]; usleep(500000); cli(o,sizeof o,"status",0,0,0);
        if(strstr(o,"wpa_state=COMPLETED")) break;
        if(i==29){ msg("Could not connect - wrong password?"); } }
    cli_ok("enable_network","all",0,0);     /* select_network disabled the others: keep them for auto-join */
    cli_ok("save_config",0,0,0);
    if(!strstr(g_msg,"Could")) msg("");
    do_poll(); do_list(0);
}
static void do_forget(const char*ssid){
    int id=find_id(ssid); if(id<0) return; char ids[12]; snprintf(ids,sizeof ids,"%d",id);
    cli_ok("remove_network",ids,0,0); cli_ok("save_config",0,0,0); do_poll(); do_list(0); }

static void* worker(void*arg){ (void)arg;
    time_t last_poll=0;
    for(;;){
        pthread_mutex_lock(&mu);
        if(!g_jobs){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=2; pthread_cond_timedwait(&cv,&mu,&ts); }
        int j=g_jobs; g_jobs=0; char cs[33],cp[64],fs[33];
        snprintf(cs,sizeof cs,"%s",g_c_ssid); snprintf(cp,sizeof cp,"%s",g_c_psk); snprintf(fs,sizeof fs,"%s",g_f_ssid);
        memset(g_c_psk,0,sizeof g_c_psk);
        g_busy = (j & ~(J_POLL|J_SRV))!=0; if(g_busy) bump();
        pthread_mutex_unlock(&mu);
        if(j&J_INIT){ RUN(0,0,"killall","r3srv"); if(g_on) j|=J_ON; }   /* orphan from a previous r3player */
        if(j&J_OFF) do_off();
        if((j&J_ON) && g_on) do_on();
        if(g_on && wpa_running()){
            if(j&J_FORGET) do_forget(fs);
            if(j&J_CONNECT) do_connect(cs,cp);
            if(j&J_SCAN){ msg("Scanning..."); cli_ok("scan",0,0,0); sleep(4); do_list(1); msg(""); }
            time_t now=time(0); if((j&(J_POLL|J_ON)) || now-last_poll>=3){ last_poll=now; do_poll(); if(j&J_ON){ sleep(3); do_list(1); } }
        }
        memset(cp,0,sizeof cp);
        srv_sync();
        if(g_busy){ g_busy=0; bump(); }
    }
    return 0;
}
static void post(int job){ pthread_mutex_lock(&mu); g_jobs|=job; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu); }

void wifi_start(void){
    prefs_load();
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,256*1024);
    pthread_create(&t,&at,worker,0); pthread_attr_destroy(&at); pthread_detach(t);
    post(J_INIT);
}
int  wifi_version(void){ return g_ver; }
int  wifi_enabled(void){ return g_on; }
int  wifi_busy(void){ return g_busy; }
void wifi_set_enabled(int on){ g_on=on?1:0; prefs_save(); bump(); post(on?J_ON:J_OFF); }
void wifi_status(char*out,int cap){
    pthread_mutex_lock(&mu);
    if(!g_on) snprintf(out,cap,"Off");
    else if(g_msg[0]) snprintf(out,cap,"%s",g_msg);
    else if(!strcmp(g_state,"COMPLETED")) snprintf(out,cap,g_ip[0]?"Connected to %s":"Connected to %s - no address yet",g_ssid);
    else if(!strcmp(g_state,"SCANNING")||!strcmp(g_state,"DISCONNECTED")||!strcmp(g_state,"INACTIVE")||!g_state[0]) snprintf(out,cap,"Not connected");
    else snprintf(out,cap,"Connecting...");
    pthread_mutex_unlock(&mu); }
void wifi_ip(char*out,int cap){ pthread_mutex_lock(&mu); snprintf(out,cap,"%s",g_on?g_ip:""); pthread_mutex_unlock(&mu); }
int  wifi_nets(wifi_net_t*out,int cap){ pthread_mutex_lock(&mu); int n=g_on?g_nnet:0; if(n>cap) n=cap;
    memcpy(out,g_net,sizeof(wifi_net_t)*n); pthread_mutex_unlock(&mu); return n; }
void wifi_scan(void){ post(J_SCAN); }
void wifi_connect(const char*ssid,const char*psk){ pthread_mutex_lock(&mu);
    snprintf(g_c_ssid,sizeof g_c_ssid,"%s",ssid); snprintf(g_c_psk,sizeof g_c_psk,"%s",psk?psk:"");
    pthread_mutex_unlock(&mu); post(J_CONNECT); }
void wifi_forget(const char*ssid){ pthread_mutex_lock(&mu); snprintf(g_f_ssid,sizeof g_f_ssid,"%s",ssid); pthread_mutex_unlock(&mu); post(J_FORGET); }
int  wifi_server(void){ return g_srv_on; }
void wifi_set_server(int on){ g_srv_on=on?1:0; prefs_save(); bump(); post(J_SRV); }
int  wifi_server_running(void){ return g_srv_pid>0; }
int  wifi_server_streams(void){ return g_streams; }
