#ifndef R3_M4A_H
#define R3_M4A_H
/* M4A/AAC playback glue (minimp4 demux + faad2 decode) — kept in its OWN .c file
 * because minimp4 and minimp3 both define bs_t/get_bits and can't share a TU. */
typedef struct m4a m4a_t;
m4a_t* m4a_open(const char*path,int*hz,int*ch,long*dur_ms);
long   m4a_decode(m4a_t*,short*out,int maxsamp);  /* returns frames (per channel); 0 = EOF */
void   m4a_seek(m4a_t*,long ms);
void   m4a_close(m4a_t*);
#endif
