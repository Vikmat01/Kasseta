#ifndef AUDIO_H
#define AUDIO_H
#include "common.h"

typedef struct Decoder Decoder;
Decoder *dec_open(const char *path);
int dec_read(Decoder *d, int16_t *out, int frames); /* stereo s16 */
int dec_seek(Decoder *d, uint64_t frame);
uint32_t dec_rate(Decoder *d);
uint64_t dec_length(Decoder *d); /* 0 = unknown */
void dec_close(Decoder *d);

/* Player: everything below must be called with the audio device locked,
 * except player_mix which is called from the audio callback. */
void player_init(int out_rate);
int player_load(const char *path, int dur_hint_ms, int start_ms); /* returns 1 on success */
void player_unload(void);
void player_set_playing(int on);
int player_is_playing(void);
int player_has_track(void);
int player_take_ended(void);       /* returns 1 once after the track finished */
int player_pos_ms(void);
int player_len_ms(void);
void player_seek_ms(int ms);
void player_mix(int16_t *out, int frames);
float player_level(void);          /* recent output level 0..1 (for visuals) */

#endif
