#ifndef R3_AUDIO_H
#define R3_AUDIO_H
/* Background MP3 playback to the DAC (minimp3 + tinyalsa). */
void audio_play(const char*path);   /* start playing a file (stops current) */
void audio_stop(void);
void audio_toggle_pause(void);
int  audio_is_playing(void);
int  audio_is_paused(void);
void audio_set_volume(int v);       /* 0..255 (DAC L/R Playback Volume) */
int  audio_get_volume(void);
long audio_pos_ms(void);
long audio_dur_ms(void);
#endif
