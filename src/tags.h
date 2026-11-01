#ifndef TAGS_H
#define TAGS_H
#include "common.h"

enum { FMT_UNKNOWN, FMT_MP3, FMT_FLAC, FMT_OGG, FMT_WAV };
enum { ART_NONE, ART_ID3, ART_FLAC, ART_OGG };

typedef struct {
	char *title, *artist, *album, *albumartist;
	int year, track, disc;
	int duration_ms;
	int format;
	int art;
} Meta;

/* returns 1 if the file looks like playable audio */
int tags_read(const char *path, Meta *m);
void meta_free(Meta *m);
/* load embedded cover image bytes (jpeg/png); malloc'd */
uint8_t *tags_load_art(const char *path, int kind, size_t *len);

/* text decoding helpers (exposed for tests) */
char *text_8bit_to_utf8(const uint8_t *s, size_t n);
#endif
