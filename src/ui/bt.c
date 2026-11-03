/* bt.c - Bluetooth for r3ii-player, driving the stock BlueZ/bluealsa CLI tools from one
 * worker thread (popen => posix_spawn on musl; safe in a threaded process).
 *
 * Stock daemons on the R3 Pro II: bluetoothd, bt-agent (NoInputNoOutput, just-works),
 * bluealsa -p a2dp-source. We (re)start bluealsa with a2dp-sink too, so the same daemon
 * serves both roles. Bonds live in /usr/data/bluetooth/<adapter>/<peer>/info (persistent). */
#include "bt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define BT_STORE "/usr/data/bluetooth"
#define BT_STATE "/usr/data/r3_bt"

static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv=PTHREAD_COND_INITIALIZER;
static bt_dev_t g_dev[BT_MAX];   static int g_ndev;
static bt_dev_t g_fnd[BT_MAX];   static int g_nfnd;
static char g_status[96]="";
static char g_out[18]="";        /* selected output MAC */
static int  g_rx=0;              /* receiver mode */
static volatile int g_ver=1, g_busy=0;

/* single-slot-per-kind job queue (newer request of the same kind replaces the older) */
enum { J_INIT=1, J_REFRESH=2, J_OUT=4, J_RX=8, J_SCAN=16, J_PAIR=32, J_FORGET=64 };
static int g_jobs; static char g_pair_mac[18], g_forget_mac[18];

static void bump(void){ g_ver++; }
static void set_status(const char*s){ pthread_mutex_lock(&mu);
    snprintf(g_status,sizeof g_status,"%s",s); bump(); pthread_mutex_unlock(&mu); }

/* run a shell command, capture up to cap bytes of stdout (ANSI/CR stripped) */
static int sh(const char*cmd,char*out,int cap){
    FILE*p=popen(cmd,"r"); if(!p){ if(out&&cap)out[0]=0; return -1; }
    int n=0,c,esc=0;
    while((c=fgetc(p))!=EOF){
        if(!out||n>=cap-1) continue;
        if(esc){ if(isalpha(c)) esc=0; continue; }   /* skip ESC [ ... letter */
        if(c==0x1b){ esc=1; continue; }
        if(c=='\r'||c==0) continue;
        out[n++]=(char)c;
    }
    if(out&&cap) out[n]=0;
    pclose(p);
    return n;
}
static int valid_mac(const char*m){
    if(!m||strlen(m)<17) return 0;
    for(int i=0;i<17;i++){ if(i%3==2){ if(m[i]!=':')return 0; } else if(!isxdigit((unsigned char)m[i]))return 0; }
    return 1;
}
static void mac_path(const char*mac,char*o,int cap){   /* AA:BB -> AA_BB for D-Bus paths */
    snprintf(o,cap,"%s",mac); for(char*c=o;*c;c++) if(*c==':')*c='_';
}

static void save_state(void){
    FILE*f=fopen(BT_STATE,"w"); if(!f)return;
    fprintf(f,"out=%s\nrx=%d\n",g_out,g_rx); fclose(f);
}
static void load_state(void){
    FILE*f=fopen(BT_STATE,"r"); if(!f)return; char l[64];
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\n")]=0;
        if(!strncmp(l,"out=",4) && (valid_mac(l+4)||!l[4])) snprintf(g_out,sizeof g_out,"%s",l+4);
        else if(!strncmp(l,"rx=",3)) g_rx=atoi(l+3)?1:0; }
    fclose(f);
}

/* ALSA config naming the selected speaker as pcm "r3bt" (read by aplay via ALSA_CONFIG_PATH) */
static void write_alsa_conf(const char*mac){
    FILE*f=fopen(BT_ALSA_CONF,"w"); if(!f)return;
    fprintf(f,"</usr/share/alsa/alsa.conf>\n"
              "pcm.r3bt {\n  type plug\n  slave.pcm {\n    type bluealsa\n"
              "    device \"%s\"\n    profile \"a2dp\"\n  }\n}\n", mac&&*mac?mac:"00:00:00:00:00:00");
    fclose(f);
}

/* ---- device list from BlueZ storage + live ACL links ---- */
static int read_info(const char*adir,const char*mac,bt_dev_t*d){
    char p[256],l[512]; snprintf(p,sizeof p,"%s/%s/info",adir,mac);
    FILE*f=fopen(p,"r"); if(!f) return 0;
    memset(d,0,sizeof *d); snprintf(d->mac,sizeof d->mac,"%s",mac);
    int gen=0,haskey=0; unsigned cls=0; char alias[64]="";
    while(fgets(l,sizeof l,f)){ l[strcspn(l,"\n")]=0;
        if(l[0]=='['){ gen=!strcmp(l,"[General]"); if(!strcmp(l,"[LinkKey]"))haskey=1; continue; }
        if(!gen) continue;
        if(!strncmp(l,"Name=",5))  snprintf(d->name,sizeof d->name,"%s",l+5);
        else if(!strncmp(l,"Alias=",6)) snprintf(alias,sizeof alias,"%s",l+6);
        else if(!strncmp(l,"Class=",6)) cls=(unsigned)strtoul(l+6,0,16);
    }
    fclose(f);
    if(alias[0]) snprintf(d->name,sizeof d->name,"%s",alias);
    if(!d->name[0]) snprintf(d->name,sizeof d->name,"%s",mac);
    int major=(cls>>8)&0x1f;              /* 1=computer 2=phone 4=audio/video */
    d->phone = (major==1||major==2);
    d->paired=haskey;
    return 1;
}
static void do_refresh(void){
    bt_dev_t tmp[BT_MAX]; int n=0;
    DIR*ad=opendir(BT_STORE);
    if(ad){ struct dirent*a;
        while((a=readdir(ad))){ if(!valid_mac(a->d_name)) continue;   /* adapter dir */
            char adir[128]; snprintf(adir,sizeof adir,"%s/%s",BT_STORE,a->d_name);
            DIR*pd=opendir(adir); if(!pd) continue; struct dirent*e;
            while((e=readdir(pd)) && n<BT_MAX){ if(!valid_mac(e->d_name)) continue;
                if(read_info(adir,e->d_name,&tmp[n]) && tmp[n].paired) n++; }
            closedir(pd); }
        closedir(ad); }
    char con[2048]; sh("hcitool con 2>/dev/null",con,sizeof con);
    for(int i=0;i<n;i++) tmp[i].connected = strstr(con,tmp[i].mac)!=NULL;
    /* stable order: speakers first, then by name */
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++){
        int sw = tmp[j].phone<tmp[i].phone ||
                 (tmp[j].phone==tmp[i].phone && strcasecmp(tmp[j].name,tmp[i].name)<0);
        if(sw){ bt_dev_t t=tmp[i]; tmp[i]=tmp[j]; tmp[j]=t; } }
    pthread_mutex_lock(&mu);
    if(n!=g_ndev || memcmp(tmp,g_dev,sizeof(bt_dev_t)*n)){ memcpy(g_dev,tmp,sizeof(bt_dev_t)*n); g_ndev=n; bump(); }
    pthread_mutex_unlock(&mu);
}
static const char* name_of(const char*mac){ static char nm[64];
    pthread_mutex_lock(&mu); snprintf(nm,sizeof nm,"%s",mac);
    for(int i=0;i<g_ndev;i++) if(!strcmp(g_dev[i].mac,mac)) snprintf(nm,sizeof nm,"%s",g_dev[i].name);
    for(int i=0;i<g_nfnd;i++) if(!strcmp(g_fnd[i].mac,mac)) snprintf(nm,sizeof nm,"%s",g_fnd[i].name);
    pthread_mutex_unlock(&mu); return nm; }

static int dbus_connect(const char*mac){
    char mp[24],cmd[256],out[512]; mac_path(mac,mp,sizeof mp);
    snprintf(cmd,sizeof cmd,"dbus-send --system --print-reply --reply-timeout=20000 --dest=org.bluez "
             "/org/bluez/hci0/dev_%s org.bluez.Device1.Connect 2>&1",mp);
    sh(cmd,out,sizeof out);
    return strstr(out,"method return")!=NULL;
}

/* ---- daemons ---- */
static void do_init(void){
    set_status("Starting Bluetooth...");
    sh("bt-adapter --set Powered On >/dev/null 2>&1",0,0);
    char ps[4096]; sh("ps",ps,sizeof ps);
    /* stock runs source-only with --a2dp-volume ("native": volume is only reported over
     * AVRCP, never applied). We need the sink role too, and bluealsa's soft-volume so the
     * phone's slider (receiver) and a speaker's own buttons actually change the level. */
    if(!strstr(ps,"a2dp-sink") || strstr(ps,"a2dp-volume")){
        sh("killall bluealsa 2>/dev/null; sleep 1; setsid sh -c 'bluealsa -p a2dp-source -p a2dp-sink "
           "</dev/null >/tmp/bluealsa.log 2>&1 &'; sleep 1",0,0); }
    if(!strstr(ps,"bt-agent"))
        sh("setsid sh -c 'bt-agent -c NoInputNoOutput </dev/null >/dev/null 2>&1 &'",0,0);
    write_alsa_conf(g_out);
    do_refresh();
    set_status("");
}
static void do_rx(void){
    int on; pthread_mutex_lock(&mu); on=g_rx; pthread_mutex_unlock(&mu);
    sh("killall bluealsa-aplay 2>/dev/null",0,0);
    if(on){
        sh("bt-adapter --set Discoverable On >/dev/null 2>&1; bt-adapter --set Pairable On >/dev/null 2>&1;"
           "setsid sh -c 'bluealsa-aplay -D hw:0,0 --profile-a2dp </dev/null >/tmp/bluealsa-aplay.log 2>&1 &'",0,0);
        /* phones we know: trust them so they can (re)connect to us without a prompt */
        bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX); char cmd[96];
        for(int i=0;i<n;i++) if(d[i].phone){
            snprintf(cmd,sizeof cmd,"bluetoothctl trust %s >/dev/null 2>&1",d[i].mac); sh(cmd,0,0); }
        char al[256],*q,m[128]; sh("printf 'show\\n' | bluetoothctl 2>/dev/null | grep Alias:",al,sizeof al);
        q=strstr(al,"Alias: "); if(q) q+=7; else { strcpy(al,"R3"); q=al; } q[strcspn(q,"\n")]=0;
        snprintf(m,sizeof m,"Receiver on - pick \"%s\" on your phone",q); set_status(m);
    } else set_status("");
}
static void do_out(void){
    char mac[18]; pthread_mutex_lock(&mu); snprintf(mac,sizeof mac,"%s",g_out); pthread_mutex_unlock(&mu);
    write_alsa_conf(mac);
    if(!mac[0]){ set_status(""); return; }
    char m[96]; snprintf(m,sizeof m,"Connecting %s...",name_of(mac)); set_status(m);
    int ok=dbus_connect(mac);
    do_refresh();
    snprintf(m,sizeof m,ok?"Output: %s":"%s unreachable (switched off?)",name_of(mac)); set_status(m);
}
static void parse_found(const char*buf){
    bt_dev_t tmp[BT_MAX]; int n=0;
    const char*p=buf;
    while(p&&*p&&n<BT_MAX){
        const char*eol=strchr(p,'\n'); int len=eol?(int)(eol-p):(int)strlen(p);
        char l[256]; snprintf(l,sizeof l,"%.*s",len<255?len:255,p);
        char*d=strstr(l,"Device ");
        if(d && valid_mac(d+7)){
            char*rest=d+7+17; while(*rest==' ')rest++;
            /* skip "[CHG] Device X RSSI: -60" style property lines */
            int prop=0; for(char*q=rest;*q&&*q!=' ';q++) if(*q==':'){ prop=1; break; }
            if(!prop && *rest){
                int dup=-1; for(int i=0;i<n;i++) if(!strncmp(tmp[i].mac,d+7,17)) dup=i;
                bt_dev_t*t= dup>=0?&tmp[dup]:&tmp[n];
                if(dup<0){ memset(t,0,sizeof *t); snprintf(t->mac,18,"%.17s",d+7); n++; }
                snprintf(t->name,sizeof t->name,"%s",rest);
            }
        }
        p=eol?eol+1:0;
    }
    /* drop already-paired ones and nameless (name == MAC with dashes) */
    bt_dev_t pd[BT_MAX]; int np=bt_devices(pd,BT_MAX); int k=0;
    for(int i=0;i<n;i++){ int skip=0;
        for(int j=0;j<np;j++) if(!strcmp(pd[j].mac,tmp[i].mac)) skip=1;
        if(strlen(tmp[i].name)==17 && tmp[i].name[2]=='-') skip=1;
        if(!skip) tmp[k++]=tmp[i]; }
    pthread_mutex_lock(&mu); memcpy(g_fnd,tmp,sizeof(bt_dev_t)*k); g_nfnd=k; bump(); pthread_mutex_unlock(&mu);
}
static void do_scan(void){
    set_status("Scanning... (put the speaker in pairing mode)");
    static char buf[16384];
    sh("bluetoothctl --timeout 12 scan on 2>/dev/null; bluetoothctl devices 2>/dev/null",buf,sizeof buf);
    parse_found(buf);
    char m[64]; int n; pthread_mutex_lock(&mu); n=g_nfnd; pthread_mutex_unlock(&mu);
    snprintf(m,sizeof m,n?"Found %d - tap one to pair":"Nothing found",n); set_status(m);
}
static void do_pair(const char*mac){
    char m[128],cmd[256],out[2048];
    snprintf(m,sizeof m,"Pairing %s...",name_of(mac)); set_status(m);
    snprintf(cmd,sizeof cmd,"bluetoothctl --agent NoInputNoOutput --timeout 20 pair %s 2>&1",mac);
    sh(cmd,out,sizeof out);
    int ok=strstr(out,"Pairing successful")||strstr(out,"AlreadyExists");
    if(ok){ snprintf(cmd,sizeof cmd,"bluetoothctl trust %s >/dev/null 2>&1",mac); sh(cmd,0,0);
        sh("sync",0,0); }
    do_refresh();
    /* a freshly paired speaker becomes the output straight away */
    bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX), spk=0;
    for(int i=0;i<n;i++) if(!strcmp(d[i].mac,mac) && !d[i].phone) spk=1;
    pthread_mutex_lock(&mu);
    for(int i=0;i<g_nfnd;i++) if(!strcmp(g_fnd[i].mac,mac)){ g_fnd[i]=g_fnd[--g_nfnd]; break; }
    if(ok&&spk){ snprintf(g_out,sizeof g_out,"%s",mac); save_state(); g_jobs|=J_OUT; }
    bump(); pthread_mutex_unlock(&mu);
    snprintf(m,sizeof m,ok?"Paired %s":"Pairing %s failed",name_of(mac)); set_status(m);
}
static void do_forget(const char*mac){
    char cmd[96]; snprintf(cmd,sizeof cmd,"bluetoothctl remove %s >/dev/null 2>&1; sync",mac); sh(cmd,0,0);
    pthread_mutex_lock(&mu);
    if(!strcmp(g_out,mac)){ g_out[0]=0; save_state(); write_alsa_conf(""); }
    pthread_mutex_unlock(&mu);
    do_refresh(); set_status("");
}

static void* worker(void*arg){ (void)arg;
    uint64_t last_ref=0;
    for(;;){
        pthread_mutex_lock(&mu);
        if(!g_jobs){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=4;
            pthread_cond_timedwait(&cv,&mu,&ts); }
        int j=g_jobs; g_jobs=0;
        char pm[18],fm[18]; snprintf(pm,sizeof pm,"%s",g_pair_mac); snprintf(fm,sizeof fm,"%s",g_forget_mac);
        g_busy = (j & ~J_REFRESH)!=0;
        pthread_mutex_unlock(&mu);
        if(j&J_INIT){ do_init(); if(g_rx) j|=J_RX; if(g_out[0]) j|=J_OUT; }
        if(j&J_FORGET) do_forget(fm);
        if(j&J_RX)     do_rx();
        if(j&J_OUT)    do_out();
        if(j&J_SCAN)   do_scan();
        if(j&J_PAIR)   do_pair(pm);
        /* periodic: track connection changes (speaker switched on/off, phone connected) */
        uint64_t now=(uint64_t)time(0);
        if((j&J_REFRESH) || now-last_ref>=4){ last_ref=now; do_refresh(); }
        g_busy=0;
    }
    return 0;
}
static void post(int job){ pthread_mutex_lock(&mu); g_jobs|=job; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu); }

void bt_start(void){
    load_state();
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at,256*1024);
    pthread_create(&t,&at,worker,0); pthread_attr_destroy(&at); pthread_detach(t);
    post(J_INIT);
}
int  bt_version(void){ return g_ver; }
int  bt_busy(void){ return g_busy; }
int  bt_devices(bt_dev_t*o,int cap){ pthread_mutex_lock(&mu); int n=g_ndev<cap?g_ndev:cap;
    memcpy(o,g_dev,sizeof(bt_dev_t)*n); pthread_mutex_unlock(&mu); return n; }
int  bt_found(bt_dev_t*o,int cap){ pthread_mutex_lock(&mu); int n=g_nfnd<cap?g_nfnd:cap;
    memcpy(o,g_fnd,sizeof(bt_dev_t)*n); pthread_mutex_unlock(&mu); return n; }
void bt_status(char*o,int cap){ pthread_mutex_lock(&mu); snprintf(o,cap,"%s",g_status); pthread_mutex_unlock(&mu); }
const char* bt_output(void){ return g_out; }
void bt_select_output(const char*mac){
    pthread_mutex_lock(&mu); snprintf(g_out,sizeof g_out,"%s",valid_mac(mac)?mac:"");
    save_state(); bump(); pthread_mutex_unlock(&mu); post(J_OUT); }
int  bt_receiver(void){ return g_rx; }
void bt_set_receiver(int on){ pthread_mutex_lock(&mu); g_rx=on?1:0; save_state(); bump(); pthread_mutex_unlock(&mu); post(J_RX); }
void bt_scan(void){ post(J_SCAN); }
void bt_pair(const char*mac){ if(!valid_mac(mac))return; pthread_mutex_lock(&mu);
    snprintf(g_pair_mac,sizeof g_pair_mac,"%s",mac); pthread_mutex_unlock(&mu); post(J_PAIR); }
void bt_forget(const char*mac){ if(!valid_mac(mac))return; pthread_mutex_lock(&mu);
    snprintf(g_forget_mac,sizeof g_forget_mac,"%s",mac); pthread_mutex_unlock(&mu); post(J_FORGET); }
void bt_refresh(void){ post(J_REFRESH); }
