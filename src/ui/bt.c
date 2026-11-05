/* bt.c - Bluetooth for r3ii-player, driving the stock BlueZ/bluealsa CLI tools from one
 * worker thread (popen => posix_spawn on musl; safe in a threaded process).
 *
 * Stock daemons on the R3 Pro II: bluetoothd, bt-agent (NoInputNoOutput, just-works),
 * bluealsa -p a2dp-source. We (re)start bluealsa with a2dp-sink too, so the same daemon
 * serves both roles. Bonds live in /usr/data/bluetooth/<adapter>/<peer>/info (persistent). */
#include "bt.h"
#include "audio.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
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
    snprintf(g_status,sizeof g_status,"%s",s); bump(); pthread_mutex_unlock(&mu);
    if(*s){ FILE*f=fopen("/tmp/r3bt.log","a"); if(f){ fprintf(f,"%ld %s\n",(long)time(0),s); fclose(f); } } }

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
    d->pad   = (major==5);                 /* peripheral: gamepad, joystick, remote, keyboard */
    d->paired=haskey;
    return 1;
}
static void do_refresh(void){
    /* self-heal: keep the adapter powered (cold boot can leave it down if the power-on in
     * do_init raced bluetoothd). Cheap check; only acts when it's actually off. */
    char pw[256]; sh("bt-adapter --info 2>/dev/null",pw,sizeof pw);
    if(!strstr(pw,"Powered: 1")) sh("bt-adapter --set Powered On >/dev/null 2>&1",0,0);
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
    for(int i=0;i<n;i++){ tmp[i].connected = strstr(con,tmp[i].mac)!=NULL; tmp[i].battery=-1;
        if(tmp[i].connected && !tmp[i].phone && !tmp[i].pad){   /* battery reported over the HFP link, if any */
            char mp[24],cmd[256],out[1024]; snprintf(mp,sizeof mp,"%s",tmp[i].mac);
            for(char*c=mp;*c;c++) if(*c==':')*c='_';
            snprintf(cmd,sizeof cmd,"dbus-send --system --print-reply --reply-timeout=1500 --dest=org.bluealsa "
                     "/org/bluealsa/hci0/dev_%s/rfcomm org.freedesktop.DBus.Properties.Get "
                     "string:org.bluealsa.RFCOMM1 string:Battery 2>/dev/null",mp);
            sh(cmd,out,sizeof out);
            char*b=strstr(out,"byte "); if(b){ int v=atoi(b+5); if(v>=0&&v<=100) tmp[i].battery=v; } }
        if(tmp[i].connected && tmp[i].battery<0){   /* else the standard BlueZ battery (BLE / some headsets) */
            char mp[24],cmd[256],out[512]; snprintf(mp,sizeof mp,"%s",tmp[i].mac); for(char*c=mp;*c;c++) if(*c==':')*c='_';
            snprintf(cmd,sizeof cmd,"dbus-send --system --print-reply --reply-timeout=1500 --dest=org.bluez "
                     "/org/bluez/hci0/dev_%s org.freedesktop.DBus.Properties.Get "
                     "string:org.bluez.Battery1 string:Percentage 2>/dev/null",mp);
            sh(cmd,out,sizeof out);
            char*b=strstr(out,"byte "); if(b){ int v=atoi(b+5); if(v>=0&&v<=100) tmp[i].battery=v; } } }
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
/* Game controllers (Joy-Con, Pro Controller, DS4...) are Bluetooth HID. BlueZ can hand them
 * to the kernel through /dev/uhid (UserspaceHID) -> hid-generic -> /dev/input/eventN, which
 * pad.c reads. /etc is read-only squashfs, so we bind-mount a copy of /etc/bluetooth with
 * input.conf added, and restart bluetoothd once (bonds live in /usr/data, untouched). */
static int ensure_uhid(void){
    if(access("/dev/uhid",F_OK)!=0) return 0;
    char cur[512]=""; sh("cat /etc/bluetooth/input.conf 2>/dev/null",cur,sizeof cur);
    if(strstr(cur,"UserspaceHID=true")) return 0;          /* already set up this boot */
    /* input.conf: UserspaceHID (controllers attach as /dev/input).
     * main.conf: AutoEnable=true so bluetoothd powers the adapter on itself at startup and
     * keeps it on (AutoEnable is off in the stock config, so it boots powered down). */
    sh("mkdir -p /tmp/r3bt_etc && cp -a /etc/bluetooth/. /tmp/r3bt_etc/ && "
       "printf '[General]\\nUserspaceHID=true\\n' > /tmp/r3bt_etc/input.conf && "
       "sed -i 's/^#*AutoEnable=.*/AutoEnable=true/' /tmp/r3bt_etc/main.conf && "
       "grep -q '^AutoEnable=true' /tmp/r3bt_etc/main.conf || printf '\\n[Policy]\\nAutoEnable=true\\n' >> /tmp/r3bt_etc/main.conf && "
       "mount --bind /tmp/r3bt_etc /etc/bluetooth",0,0);
    sh("cat /etc/bluetooth/input.conf 2>/dev/null",cur,sizeof cur);
    if(!strstr(cur,"UserspaceHID=true")) return 0;
    sh("killall bt-agent bluealsa bluealsa-aplay bluetoothd 2>/dev/null; sleep 1; "
       "setsid sh -c '/usr/libexec/bluetooth/bluetoothd -E -C </dev/null >/tmp/bluetoothd.log 2>&1 &'; sleep 2",0,0);
    return 1;
}
static void do_init(void){
    set_status("Starting Bluetooth...");
    /* Bluetooth game controllers (Joy-Con, Pro Controller, DS4) only attach as /dev/input/
     * when BlueZ routes HID through userspace (uhid). Without UserspaceHID a controller links
     * at the ACL level but no event device is created and it drops after ~2s (bluetoothd:
     * "input-hid ... connecting -> disconnected"). Set it up on every boot — bonds in
     * /usr/data are untouched. (Required for the Joy-Con to work at all after a reboot.) */
    ensure_uhid();
    /* Power the adapter on and make sure it STAYS on. ensure_uhid() restarts bluetoothd, and
     * for a moment hciconfig still shows the pre-restart "UP" before the fresh daemon resets
     * the controller — so confirm BlueZ's own Powered state twice before trusting it. Without
     * this nothing (phone, speaker or controller) can ever connect. */
    for(int i=0;i<16;i++){ char st[256]; sh("bt-adapter --info 2>/dev/null",st,sizeof st);
        if(strstr(st,"Powered: 1")){ usleep(300000); sh("bt-adapter --info 2>/dev/null",st,sizeof st);
            if(strstr(st,"Powered: 1")) break; }
        sh("bt-adapter --set Powered On >/dev/null 2>&1",0,0); usleep(500000); }
    char ps[4096]; sh("ps",ps,sizeof ps);
    /* stock runs source-only with --a2dp-volume ("native": volume is only reported over
     * AVRCP, never applied). We need the sink role too, and bluealsa's soft-volume so the
     * phone's slider (receiver) and a speaker's own buttons actually change the level.
     * hfp-ag: headphones report their battery over the hands-free link (Apple AT
     * extension), which bluealsa exposes as RFCOMM1.Battery. No calls are routed. */
    if(!strstr(ps,"a2dp-sink") || strstr(ps,"a2dp-volume") || !strstr(ps,"hfp-ag")){
        sh("killall bluealsa 2>/dev/null; sleep 1; setsid sh -c 'bluealsa -p a2dp-source -p a2dp-sink -p hfp-ag "
           "</dev/null >/tmp/bluealsa.log 2>&1 &'; sleep 1",0,0); }
    if(!strstr(ps,"bt-agent"))
        sh("setsid sh -c 'bt-agent -c NoInputNoOutput </dev/null >/dev/null 2>&1 &'",0,0);
    write_alsa_conf(g_out);
    do_refresh();
    set_status("");
}
/* iOS only lists us as an output (Control Centre, Qobuz...) while the A2DP link is up, and
 * it doesn't reconnect on its own after bluealsa restarts, so we dial the phones we know
 * (first one that answers wins). Used when the receiver is enabled, and periodically. */
static void do_rx_connect(void){
    bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX);
    char m[128]; int linked=0;
    for(int i=0;i<n && !linked;i++) if(d[i].phone){
        if(d[i].connected){ linked=1; snprintf(m,sizeof m,"Receiver on - %s connected",d[i].name); break; }
        snprintf(m,sizeof m,"Receiver on - connecting %s...",d[i].name); set_status(m);
        if(dbus_connect(d[i].mac)){ linked=1; snprintf(m,sizeof m,"Receiver on - %s connected",d[i].name); }
    }
    if(!linked){
        char al[256],*q; sh("printf 'show\\n' | bluetoothctl 2>/dev/null | grep Alias:",al,sizeof al);
        q=strstr(al,"Alias: "); if(q) q+=7; else { strcpy(al,"R3"); q=al; } q[strcspn(q,"\n")]=0;
        snprintf(m,sizeof m,"Receiver on - pick \"%s\" on your phone",q);
    }
    set_status(m);
    do_refresh();
}
#define APLAY_START "setsid sh -c 'bluealsa-aplay -D hw:0,0 --profile-a2dp </dev/null >/tmp/bluealsa-aplay.log 2>&1 &'"
/* Called from the audio thread when local jack playback finds the DAC held by the phone
 * stream: pause the phone's player over AVRCP (Qobuz etc. stop instead of playing into
 * the void) and release the DAC. The worker restarts bluealsa-aplay once we let go. */
void bt_preempt_receiver(void){
    bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX); char cmd[256],mp[24];
    for(int i=0;i<n;i++) if(d[i].phone && d[i].connected){ mac_path(d[i].mac,mp,sizeof mp);
        snprintf(cmd,sizeof cmd,"dbus-send --system --print-reply --reply-timeout=2000 --dest=org.bluez "
                 "/org/bluez/hci0/dev_%s org.bluez.MediaControl1.Pause >/dev/null 2>&1",mp); sh(cmd,0,0); }
    sh("killall bluealsa-aplay 2>/dev/null",0,0);
    set_status("Phone paused - R3 has the jack");
}
/* receiver on, R3 not holding the DAC, bluealsa-aplay gone (preempted) => bring it back */
static void rx_keepalive(void){
    if(!g_rx || audio_on_jack()) return;
    char ps[4096]; sh("ps",ps,sizeof ps);
    if(!strstr(ps,"bluealsa-aplay")) sh(APLAY_START,0,0);
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
        do_rx_connect();
    } else set_status("");
}
static void do_out(void){
    char mac[18]; pthread_mutex_lock(&mu); snprintf(mac,sizeof mac,"%s",g_out);
    /* a controller (or phone) can't be an audio output — e.g. picked with an older build */
    for(int i=0;i<g_ndev;i++) if(!strcmp(g_dev[i].mac,mac) && (g_dev[i].pad||g_dev[i].phone)){
        g_out[0]=0; mac[0]=0; save_state(); bump(); }
    pthread_mutex_unlock(&mu);
    write_alsa_conf(mac);
    if(!mac[0]){ audio_reroute(0); set_status("Output: headphone jack"); return; }
    char m[96]; snprintf(m,sizeof m,"Connecting %s...",name_of(mac)); set_status(m);
    int ok=dbus_connect(mac);
    if(ok){   /* wait for bluealsa to expose the speaker's PCM before aplay opens it */
        char mp[24],want[64],pcms[2048]; mac_path(mac,mp,sizeof mp);
        snprintf(want,sizeof want,"dev_%s/a2dpsrc",mp); ok=0;
        for(int i=0;i<30 && !ok;i++){ sh("bluealsa-cli list-pcms 2>/dev/null",pcms,sizeof pcms);
            if(strstr(pcms,want)) ok=1; else usleep(200000); }
    }
    /* switch the running track over (or stay where we are if the speaker isn't there) */
    if(ok) audio_reroute(1);
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
        dbus_connect(mac);            /* bring the link up now (controllers: HID input appears) */
        sh("sync",0,0); }
    do_refresh();
    /* a freshly paired speaker becomes the output straight away */
    bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX), spk=0;
    for(int i=0;i<n;i++) if(!strcmp(d[i].mac,mac) && !d[i].phone && !d[i].pad) spk=1;
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
    uint64_t last_ref=0, last_rx=0, last_pad=0;
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
        if((j&J_REFRESH) || now-last_ref>=4){ last_ref=now; do_refresh(); rx_keepalive(); }
        /* receiver on but no phone linked (restart, out of range): dial again now and then,
         * only while idle so paging never stutters a stream */
        if(g_rx && now-last_rx>=60 && !audio_is_playing()){ last_rx=now;
            bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX), any=0;
            for(int i=0;i<n;i++) if(d[i].phone && d[i].connected) any=1;
            if(!any) do_rx_connect(); }
        /* controllers don't page us back after a deep sleep — the host must dial them. Keep
         * dialing a bonded-but-disconnected pad so pressing a Joy-Con button reconnects it
         * automatically (a dial to an asleep pad just fails fast). One per cycle to bound the
         * worker; skip while a BT speaker is streaming so paging never stutters the audio. */
        if(now-last_pad>=7 && !(bt_output()[0] && audio_is_playing())){ last_pad=now;
            bt_dev_t d[BT_MAX]; int n=bt_devices(d,BT_MAX);
            for(int i=0;i<n;i++) if(d[i].pad && !d[i].connected){ dbus_connect(d[i].mac); do_refresh(); break; } }
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
int bt_audio_battery(char*name,int cap){   /* the selected output first, else any connected speaker */
    int best=-1; pthread_mutex_lock(&mu);
    for(int i=0;i<g_ndev;i++){ bt_dev_t*d=&g_dev[i]; if(!d->connected||d->phone||d->battery<0) continue;
        if(best<0 || !strcmp(d->mac,g_out)){ best=d->battery; if(name) snprintf(name,cap,"%s",d->name); } }
    pthread_mutex_unlock(&mu); return best; }
