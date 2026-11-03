#ifndef R3_BT_H
#define R3_BT_H
/* Bluetooth control (BlueZ 5.54 + bluealsa 4.1.1 on the stock rootfs).
 * Every slow operation (connect, pair, scan) runs on a worker thread through the
 * stock CLI tools; the UI only reads snapshots, so it never blocks.
 *
 *  - Output: local playback goes to the headphone jack, or to one remembered A2DP
 *    speaker/headphone (audio.c pipes PCM into `aplay -D r3bt`).
 *  - Receiver: the R3 acts as an A2DP sink (iPhone -> R3 -> jack) via bluealsa-aplay. */

#define BT_MAX 24

typedef struct {
    char mac[18];
    char name[64];
    int  paired, connected;
    int  phone;           /* major device class = phone/computer => a source, not a speaker */
} bt_dev_t;

void bt_start(void);                    /* spawn worker: power on, daemons, auto-reconnect */
int  bt_version(void);                  /* bumps whenever the snapshot below changes */
int  bt_devices(bt_dev_t*out,int cap);  /* paired devices snapshot */
int  bt_found(bt_dev_t*out,int cap);    /* unpaired devices from the last scan */
void bt_status(char*out,int cap);       /* one-line human status ("Scanning…") */
int  bt_busy(void);

const char* bt_output(void);            /* selected output MAC, "" = headphone jack */
void bt_select_output(const char*mac);  /* "" or NULL = jack; else connect + use it */
int  bt_receiver(void);
void bt_set_receiver(int on);           /* phone -> R3 sink mode */

void bt_scan(void);
void bt_pair(const char*mac);           /* pair + trust + connect (just-works agent) */
void bt_forget(const char*mac);
void bt_refresh(void);
void bt_preempt_receiver(void);         /* local jack playback wins: pause phone, free the DAC */

#define BT_ALSA_CONF "/tmp/r3bt_alsa.conf"   /* defines pcm.r3bt for the selected output */
#endif
