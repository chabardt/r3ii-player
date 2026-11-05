#ifndef R3_SHARE_H
#define R3_SHARE_H
/* LAN peer discovery (UDP beacon on :8201) + a tiny HTTP client, for sharing
 * playlists between R3 devices running r3srv. */
typedef struct { char name[48]; char ip[20]; int port; } peer_t;

void share_start(void);                  /* start the discovery listener (once) */
int  share_peers(peer_t *out, int cap);  /* peers heard in the last ~20 s */

/* HTTP/1.0 GET of a small text resource into out; returns body length or -1. */
int  share_get(const char *ip, int port, const char *path, char *out, int cap);
/* Download path to outfile (creating parent dirs); returns 0 on success. */
int  share_download(const char *ip, int port, const char *path, const char *outfile);
#endif
