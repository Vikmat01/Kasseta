/* Asynchronous cover loading with a two-level cache:
 * masters (decoded + shrunk to <=600px) and per-size square thumbnails. */
#include "covers.h"
#include "tags.h"
#include <pthread.h>

#define MAX_ENTRIES 160
#define MAX_MASTERS 6
#define MAX_REQ 64

typedef struct {
	int album, size, state; /* 0 loading, 1 ready, 2 missing */
	Image *img;
	uint64_t used;
} Entry;

typedef struct {
	int album, size, gen;
	char *cover_path, *track_path;
	int art_kind;
} Req;

typedef struct { int album; Image *img; int missing; uint64_t used; } Master;

static Entry entries[MAX_ENTRIES];
static int nentries;
static Req queue[MAX_REQ];
static int nqueue;
static Req done_q[MAX_REQ];
static Image *done_img[MAX_REQ];
static int ndone;
static Master masters[MAX_MASTERS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t thr;
static int running, gen;
static Library *lib;
static uint64_t tick;

static Image *load_master(Req *r, int *missing) {
	Image *im = NULL;
	if (r->cover_path) im = img_load_file(r->cover_path, 600);
	if (!im && r->track_path && r->art_kind) {
		size_t n;
		uint8_t *d = tags_load_art(r->track_path, r->art_kind, &n);
		if (d) { im = img_load_mem(d, n, 600); free(d); }
	}
	*missing = im == NULL;
	return im;
}

static void *worker(void *arg) {
	(void)arg;
	pthread_mutex_lock(&mu);
	while (running) {
		if (!nqueue) { pthread_cond_wait(&cv, &mu); continue; }
		Req r = queue[--nqueue]; /* LIFO: newest request first */
		int mygen = gen;
		/* master cache lookup */
		Master *m = NULL;
		for (int i = 0; i < MAX_MASTERS; i++) if (masters[i].used && masters[i].album == r.album) m = &masters[i];
		Image *mimg = NULL, *out = NULL;
		int missing = 0;
		int fresh = 0;
		if (m) {
			/* masters may be freed by the main thread, so resize under the lock */
			mimg = m->img; missing = m->missing;
			out = (!missing && mimg) ? img_square(mimg, r.size) : NULL;
		} else {
			pthread_mutex_unlock(&mu);
			mimg = load_master(&r, &missing);
			fresh = 1;
			out = (!missing && mimg) ? img_square(mimg, r.size) : NULL;
			pthread_mutex_lock(&mu);
		}
		free(r.cover_path); free(r.track_path);
		r.cover_path = r.track_path = NULL;
		if (fresh) {
			if (mygen == gen && r.gen == gen) {
				int slot = 0;
				for (int i = 1; i < MAX_MASTERS; i++) if (masters[i].used < masters[slot].used) slot = i;
				if (masters[slot].img) cv_free(masters[slot].img);
				masters[slot].album = r.album; masters[slot].img = mimg; masters[slot].missing = missing;
				masters[slot].used = ++tick;
			} else if (mimg) cv_free(mimg);
		} else if (m) m->used = ++tick;
		if (r.gen == gen && ndone < MAX_REQ) { done_q[ndone] = r; done_img[ndone] = out; ndone++; }
		else if (out) cv_free(out);
	}
	pthread_mutex_unlock(&mu);
	return NULL;
}

void covers_init(void) {
	running = 1;
	pthread_create(&thr, NULL, worker, NULL);
}

static void clear_all(void) {
	for (int i = 0; i < nentries; i++) if (entries[i].img) cv_free(entries[i].img);
	nentries = 0;
	for (int i = 0; i < nqueue; i++) { free(queue[i].cover_path); free(queue[i].track_path); }
	nqueue = 0;
	for (int i = 0; i < ndone; i++) { if (done_img[i]) cv_free(done_img[i]); free(done_q[i].cover_path); free(done_q[i].track_path); }
	ndone = 0;
	for (int i = 0; i < MAX_MASTERS; i++) { if (masters[i].img) cv_free(masters[i].img); memset(&masters[i], 0, sizeof masters[i]); }
}

void covers_set_library(Library *l) {
	pthread_mutex_lock(&mu);
	clear_all();
	lib = l;
	gen++;
	pthread_mutex_unlock(&mu);
}

Image *covers_get(int album, int size, int *missing) {
	if (missing) *missing = 0;
	if (!lib || album < 0 || album >= lib->nalbums) { if (missing) *missing = 1; return NULL; }
	for (int i = 0; i < nentries; i++) {
		Entry *e = &entries[i];
		if (e->album == album && e->size == size) {
			e->used = ++tick;
			if (e->state == 2 && missing) *missing = 1;
			return e->img;
		}
	}
	Album *a = &lib->albums[album];
	if (!a->cover_path && a->art_track < 0) {
		if (missing) *missing = 1;
	}
	/* new entry (evict LRU) */
	int slot = nentries;
	if (nentries == MAX_ENTRIES) {
		slot = 0;
		for (int i = 1; i < nentries; i++) if (entries[i].used < entries[slot].used) slot = i;
		if (entries[slot].state == 0) { /* still loading: pick another */
			for (int i = 0; i < nentries; i++) if (entries[i].state != 0) { slot = i; break; }
		}
		if (entries[slot].img) cv_free(entries[slot].img);
	} else nentries++;
	Entry *e = &entries[slot];
	e->album = album; e->size = size; e->img = NULL; e->used = ++tick;
	if (!a->cover_path && a->art_track < 0) { e->state = 2; return NULL; }
	e->state = 0;
	Req r = { album, size, gen, a->cover_path ? xstrdup(a->cover_path) : NULL,
	          a->art_track >= 0 ? xstrdup(lib->tracks[a->art_track].path) : NULL,
	          a->art_track >= 0 ? lib->tracks[a->art_track].art : 0 };
	pthread_mutex_lock(&mu);
	if (nqueue == MAX_REQ) { /* drop oldest */
		free(queue[0].cover_path); free(queue[0].track_path);
		/* mark its entry as not-loading so it can be requested again */
		for (int i = 0; i < nentries; i++)
			if (entries[i].album == queue[0].album && entries[i].size == queue[0].size && entries[i].state == 0) {
				entries[i] = entries[--nentries];
				break;
			}
		memmove(queue, queue + 1, (MAX_REQ - 1) * sizeof(Req));
		nqueue--;
	}
	queue[nqueue++] = r;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	return NULL;
}

int covers_poll(void) {
	int any = 0;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ndone; i++) {
		for (int k = 0; k < nentries; k++) {
			Entry *e = &entries[k];
			if (e->album == done_q[i].album && e->size == done_q[i].size && e->state == 0) {
				e->img = done_img[i];
				done_img[i] = NULL;
				e->state = e->img ? 1 : 2;
				any = 1;
				break;
			}
		}
		if (done_img[i]) cv_free(done_img[i]);
	}
	ndone = 0;
	pthread_mutex_unlock(&mu);
	return any;
}

void covers_shutdown(void) {
	pthread_mutex_lock(&mu);
	running = 0;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	pthread_join(thr, NULL);
	clear_all();
}
