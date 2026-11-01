#include "audio.h"
#include "tags.h"
#include <math.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD_DETECTION_ON_MACOS
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_WCHAR
#include "dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

struct Decoder {
	int fmt;
	uint32_t rate, channels;
	uint64_t length;
	drmp3 mp3;
	drflac *flac;
	drwav wav;
	stb_vorbis *vorbis;
	drmp3_seek_point *seekpts;
	int seek_built;
	int16_t *tmp;
	int tmp_frames;
};

static int sniff_format(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return FMT_UNKNOWN;
	uint8_t b[12] = {0};
	long start = 0;
	if (fread(b, 1, 10, f) == 10 && !memcmp(b, "ID3", 3))
		start = 10 + ((b[6] & 0x7F) << 21 | (b[7] & 0x7F) << 14 | (b[8] & 0x7F) << 7 | (b[9] & 0x7F)) + ((b[5] & 0x10) ? 10 : 0);
	fseek(f, start, SEEK_SET);
	memset(b, 0, sizeof b);
	size_t n = fread(b, 1, 12, f);
	fclose(f);
	if (n >= 4 && !memcmp(b, "fLaC", 4)) return FMT_FLAC;
	if (n >= 4 && !memcmp(b, "OggS", 4)) return FMT_OGG;
	if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) return FMT_WAV;
	return FMT_MP3;
}

Decoder *dec_open(const char *path) {
	Decoder *d = xcalloc(1, sizeof *d);
	d->fmt = sniff_format(path);
	switch (d->fmt) {
	case FMT_MP3:
		if (!drmp3_init_file(&d->mp3, path, NULL)) goto fail;
		d->rate = d->mp3.sampleRate; d->channels = d->mp3.channels;
		if (d->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX) d->length = d->mp3.totalPCMFrameCount;
		break;
	case FMT_FLAC:
		d->flac = drflac_open_file(path, NULL);
		if (!d->flac) goto fail;
		d->rate = d->flac->sampleRate; d->channels = d->flac->channels; d->length = d->flac->totalPCMFrameCount;
		break;
	case FMT_WAV:
		if (!drwav_init_file(&d->wav, path, NULL)) goto fail;
		d->rate = d->wav.sampleRate; d->channels = d->wav.channels; d->length = d->wav.totalPCMFrameCount;
		break;
	case FMT_OGG: {
		int err;
		d->vorbis = stb_vorbis_open_filename(path, &err, NULL);
		if (!d->vorbis) goto fail;
		stb_vorbis_info in = stb_vorbis_get_info(d->vorbis);
		d->rate = in.sample_rate; d->channels = in.channels;
		d->length = stb_vorbis_stream_length_in_samples(d->vorbis);
		break;
	}
	default: goto fail;
	}
	if (!d->rate || !d->channels) { dec_close(d); return NULL; }
	return d;
fail:
	free(d);
	return NULL;
}

int dec_read(Decoder *d, int16_t *out, int frames) {
	if (d->fmt == FMT_OGG) return stb_vorbis_get_samples_short_interleaved(d->vorbis, 2, out, frames * 2);
	int ch = d->channels;
	int16_t *buf = out;
	if (ch != 2) {
		if (d->tmp_frames < frames) { d->tmp = xrealloc(d->tmp, (size_t)frames * ch * 2); d->tmp_frames = frames; }
		buf = d->tmp;
	}
	uint64_t got = 0;
	if (d->fmt == FMT_MP3) got = drmp3_read_pcm_frames_s16(&d->mp3, frames, buf);
	else if (d->fmt == FMT_FLAC) got = drflac_read_pcm_frames_s16(d->flac, frames, buf);
	else if (d->fmt == FMT_WAV) got = drwav_read_pcm_frames_s16(&d->wav, frames, buf);
	if (ch == 1) for (uint64_t i = 0; i < got; i++) out[2 * i] = out[2 * i + 1] = buf[i];
	else if (ch > 2) for (uint64_t i = 0; i < got; i++) { out[2 * i] = buf[i * ch]; out[2 * i + 1] = buf[i * ch + 1]; }
	return (int)got;
}

int dec_seek(Decoder *d, uint64_t frame) {
	switch (d->fmt) {
	case FMT_MP3:
		if (!d->seek_built) {
			d->seek_built = 1;
			drmp3_uint32 n = 2048;
			d->seekpts = xmalloc(n * sizeof(drmp3_seek_point));
			if (drmp3_calculate_seek_points(&d->mp3, &n, d->seekpts)) {
				drmp3_bind_seek_table(&d->mp3, n, d->seekpts);
				if (!d->length) d->length = drmp3_get_pcm_frame_count(&d->mp3);
			}
		}
		return drmp3_seek_to_pcm_frame(&d->mp3, frame);
	case FMT_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame);
	case FMT_WAV: return drwav_seek_to_pcm_frame(&d->wav, frame);
	case FMT_OGG: return stb_vorbis_seek(d->vorbis, (unsigned)frame);
	}
	return 0;
}

uint32_t dec_rate(Decoder *d) { return d->rate; }
uint64_t dec_length(Decoder *d) { return d->length; }

void dec_close(Decoder *d) {
	if (!d) return;
	if (d->fmt == FMT_MP3) drmp3_uninit(&d->mp3);
	else if (d->fmt == FMT_FLAC) drflac_close(d->flac);
	else if (d->fmt == FMT_WAV) drwav_uninit(&d->wav);
	else if (d->fmt == FMT_OGG && d->vorbis) stb_vorbis_close(d->vorbis);
	free(d->seekpts);
	free(d->tmp);
	free(d);
}

/* ---------- player ---------- */

#define CHUNK 2048
static struct {
	Decoder *dec;
	int out_rate;
	volatile int playing, ended, eof;
	uint64_t pos;       /* source frames consumed */
	uint64_t len;
	int dur_hint_ms;
	uint64_t step, frac; /* Q32 */
	int16_t buf[CHUNK * 2];
	int nbuf, ibuf;
	int32_t s0[2], s1[2];
	int gain;           /* 0..256 */
	float level;
} P;

void player_init(int out_rate) {
	memset(&P, 0, sizeof P);
	P.out_rate = out_rate > 0 ? out_rate : 44100;
}

static int next_frame(int32_t *s) {
	if (P.ibuf >= P.nbuf) {
		if (P.eof) return 0;
		P.nbuf = dec_read(P.dec, P.buf, CHUNK);
		P.ibuf = 0;
		if (P.nbuf <= 0) { P.nbuf = 0; P.eof = 1; return 0; }
	}
	s[0] = P.buf[P.ibuf * 2];
	s[1] = P.buf[P.ibuf * 2 + 1];
	P.ibuf++;
	P.pos++;
	return 1;
}

static void reset_stream(void) {
	P.nbuf = P.ibuf = 0;
	P.eof = 0;
	P.frac = 0;
	P.s0[0] = P.s0[1] = 0;
	if (!next_frame(P.s1)) P.s1[0] = P.s1[1] = 0;
	else P.pos--; /* s1 is a lookahead */
}

int player_load(const char *path, int dur_hint_ms, int start_ms) {
	player_unload();
	Decoder *d = dec_open(path);
	if (!d) return 0;
	P.dec = d;
	P.dur_hint_ms = dur_hint_ms;
	P.len = dec_length(d);
	P.step = ((uint64_t)dec_rate(d) << 32) / P.out_rate;
	P.ended = 0;
	P.pos = 0;
	P.gain = 0;
	if (start_ms > 0) {
		uint64_t fr = (uint64_t)start_ms * dec_rate(d) / 1000;
		if (dec_seek(d, fr)) P.pos = fr;
		P.len = dec_length(d);
	}
	reset_stream();
	return 1;
}

void player_unload(void) {
	if (P.dec) dec_close(P.dec);
	P.dec = NULL;
	P.playing = 0;
	P.ended = 0;
	P.pos = 0;
	P.len = 0;
}

void player_set_playing(int on) { P.playing = on && P.dec; }
int player_is_playing(void) { return P.playing; }
int player_has_track(void) { return P.dec != NULL; }
int player_take_ended(void) {
	if (P.ended) { P.ended = 0; return 1; }
	return 0;
}

int player_pos_ms(void) {
	if (!P.dec) return 0;
	return (int)(P.pos * 1000 / dec_rate(P.dec));
}
int player_len_ms(void) {
	if (!P.dec) return 0;
	if (P.len) return (int)(P.len * 1000 / dec_rate(P.dec));
	return P.dur_hint_ms;
}

void player_seek_ms(int ms) {
	if (!P.dec) return;
	if (ms < 0) ms = 0;
	int len = player_len_ms();
	if (len > 0 && ms > len - 500) ms = MAX(0, len - 500);
	uint64_t fr = (uint64_t)ms * dec_rate(P.dec) / 1000;
	if (dec_seek(P.dec, fr)) P.pos = fr;
	P.len = dec_length(P.dec);
	reset_stream();
	P.gain = 0;
}

float player_level(void) { return P.level; }

void player_mix(int16_t *out, int frames) {
	int target = P.playing ? 256 : 0;
	if (!P.dec || (!P.playing && P.gain == 0)) {
		memset(out, 0, frames * 4);
		P.level *= 0.8f;
		return;
	}
	int64_t acc = 0;
	for (int i = 0; i < frames; i++) {
		if (P.gain < target) P.gain++;
		else if (P.gain > target) P.gain--;
		if (P.gain == 0 && target == 0) { memset(out + 2 * i, 0, (frames - i) * 4); break; }
		int drained = 0;
		while (P.frac >= (1ull << 32)) {
			P.frac -= 1ull << 32;
			P.s0[0] = P.s1[0]; P.s0[1] = P.s1[1];
			if (!next_frame(P.s1)) { drained = 1; break; }
		}
		if (drained) {
			memset(out + 2 * i, 0, (frames - i) * 4);
			P.playing = 0;
			P.gain = 0;
			P.ended = 1;
			break;
		}
		int32_t t = (int32_t)(P.frac >> 16); /* 0..65535 */
		int32_t l = P.s0[0] + (((P.s1[0] - P.s0[0]) * t) >> 16);
		int32_t r = P.s0[1] + (((P.s1[1] - P.s0[1]) * t) >> 16);
		l = (l * P.gain) >> 8;
		r = (r * P.gain) >> 8;
		out[2 * i] = (int16_t)l;
		out[2 * i + 1] = (int16_t)r;
		acc += (l < 0 ? -l : l) + (r < 0 ? -r : r);
		P.frac += P.step;
	}
	float lv = (float)acc / (frames * 2 * 32768.0f) * 3.0f;
	if (lv > 1) lv = 1;
	P.level = P.level * 0.6f + lv * 0.4f;
}
