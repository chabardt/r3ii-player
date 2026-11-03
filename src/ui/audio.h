#ifndef R3_AUDIO_H
#define R3_AUDIO_H
/* Background MP3 playback to the DAC (minimp3 + tinyalsa). */
void audio_play(const char*path);   /* start playing a file (stops current) */
void audio_stop(void);
void audio_toggle_pause(void);
int  audio_is_playing(void);
int  audio_is_paused(void);
int  audio_finished(void);          /* 1 once if the last track ended on its own (not stop/skip) */
void audio_set_volume(int v);       /* 0..255 (DAC L/R Playback Volume) */
int  audio_get_volume(void);
long audio_pos_ms(void);
long audio_dur_ms(void);
void audio_seek(long ms);           /* jump to position (ms) */
int  audio_bt_failed(void);         /* 1 once if the BT speaker vanished mid-track */
int  audio_on_jack(void);           /* local playback currently holds the DAC */
void audio_dac_follow(void);        /* receiver mode: keep DAC routing/volume applied */
void audio_play_at(const char*path,long ms);   /* start at a position (resume) */
void audio_reroute(int bt_ready);   /* output selection changed: switch sinks mid-track */
#endif
