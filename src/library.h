#ifndef LIBRARY_H
#define LIBRARY_H
#include "common.h"
#include "tags.h"

typedef struct {
	char *path, *title, *artist, *album, *albumartist;
	int year, track, disc, dur_ms, format, art;
	int album_idx;
} Track;

typedef struct {
	char *name, *artist;
	int year, dur_ms;
	int *tracks, ntracks;
	char *cover_path;   /* image file in the folder, or NULL */
	int art_track;      /* track with embedded art, or -1 */
	int artist_idx;
	char *sortkey;
} Album;

typedef struct {
	char *name;
	int *albums, nalbums;
	int ntracks;
	char *sortkey;
	int special; /* "various"/"unknown" sort last */
} Artist;

typedef struct {
	Track *tracks; int ntracks;
	Album *albums; int nalbums;
	Artist *artists; int nartists;
	int *tracks_by_title;
	int *albums_sorted;
	int *hash; int hcap;
} Library;

/* raw record (scan result / cache line) */
typedef struct {
	char *path;
	Meta m;
} Rec;

typedef struct {
	char *dir, *image;
} DirImage;

typedef struct {
	Rec *recs; int nrecs, caprecs;
	DirImage *imgs; int nimgs, capimgs;
} RecSet;

Library *lib_build(RecSet *rs, const char *root);
void lib_free(Library *l);
int lib_find(Library *l, const char *path);

int recset_load(RecSet *rs, const char *cache_path);
int recset_save(RecSet *rs, const char *cache_path);
void recset_free(RecSet *rs);

/* background scanner */
typedef struct {
	volatile int running, done, changed;
	volatile int found, parsed, to_parse;
	char current[256];
	RecSet *result;  /* valid when done */
} ScanStatus;

/* full=1 ignores the cache and re-reads every tag */
void scan_start(const char *root, const char *cache_path, const char *skip_file, int full, ScanStatus *st);
void scan_join(void);

#endif
