#ifndef R3_WIFI_H
#define R3_WIFI_H
/* Wi-Fi + music server control (stock wpa_supplicant / wpa_cli / udhcpc on wlan0).
 * Like bt.c: slow work runs on a worker thread, the UI reads snapshots.
 * Networks live in the stock /usr/data/wpa_supplicant.conf (shared with the HiBy firmware).
 * The music server is r3srv (UPnP/DLNA + HTTP), started while Wi-Fi has an address. */

#define WIFI_MAX 24
typedef struct {
    char ssid[33];
    int  signal;      /* dBm, 0 = not seen in the last scan */
    int  secured;     /* needs a password */
    int  saved;       /* in the config (id >= 0) */
    int  id;
    int  current;     /* the network we're on */
} wifi_net_t;

void wifi_start(void);
int  wifi_version(void);                 /* bumps whenever the snapshot changes */
int  wifi_enabled(void);
void wifi_set_enabled(int on);
int  wifi_busy(void);
void wifi_status(char*out,int cap);      /* "Connected to X", "Connecting...", "Off" */
void wifi_ip(char*out,int cap);          /* "" until we have an address */
int  wifi_nets(wifi_net_t*out,int cap);  /* saved + scanned, current first, then by signal */
void wifi_scan(void);
void wifi_connect(const char*ssid,const char*psk);   /* psk NULL/"" = saved or open network */
void wifi_forget(const char*ssid);

int  wifi_server(void);                  /* music server switch */
void wifi_set_server(int on);
int  wifi_server_running(void);
int  wifi_server_streams(void);          /* players streaming right now */
#define WIFI_SRV_PORT 8200
#endif
