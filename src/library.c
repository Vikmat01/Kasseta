#include "library.h"
#include <dirent.h>
#include <pthread.h>
#include <ctype.h>

/* ---------- RecSet ---------- */

static void rs_add(RecSet *rs, Rec r) {
	if (rs->nrecs == rs->caprecs) { rs->caprecs = rs->caprecs * 2 + 256; rs->recs = xrealloc(rs->recs, rs->caprecs * sizeof(Rec)); }
	rs->recs[rs->nrecs++] = r;
}
static void rs_add_img(RecSet *rs, const char *dir, const char *img) {
	if (rs->nimgs == rs->capimgs) { rs->capimgs = rs->capimgs * 2 + 64; rs->imgs = xrealloc(rs->imgs, rs->capimgs * sizeof(DirImage)); }
	rs->imgs[rs->nimgs].dir = xstrdup(dir);
	rs->imgs[rs->nimgs].image = xstrdup(img);
	rs->nimgs++;
}

void recset_free(RecSet *rs) {
	for (int i = 0; i < rs->nrecs; i++) { free(rs->recs[i].path); meta_free(&rs->recs[i].m); }
	for (int i = 0; i < rs->nimgs; i++) { free(rs->imgs[i].dir); free(rs->imgs[i].image); }
	free(rs->recs); free(rs->imgs);
	memset(rs, 0, sizeof *rs);
}

static void put_field(FILE *f, const char *s) {
	fputc('\t', f);
	if (!s) return;
	for (; *s; s++) fputc((*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s, f);
}

#define CACHE_MAGIC "KASSETA-CACHE 2"

int recset_save(RecSet *rs, const char *path) {
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return 0;
	fprintf(f, "%s\n", CACHE_MAGIC);
	for (int i = 0; i < rs->nrecs; i++) {
		Rec *r = &rs->recs[i];
		fputc('T', f);
		put_field(f, r->path); put_field(f, r->m.title); put_field(f, r->m.artist);
		put_field(f, r->m.album); put_field(f, r->m.albumartist);
		fprintf(f, "\t%d\t%d\t%d\t%d\t%d\t%d\n", r->m.year, r->m.track, r->m.disc, r->m.duration_ms, r->m.format, r->m.art);
	}
	for (int i = 0; i < rs->nimgs; i++) {
		fputc('I', f);
		put_field(f, rs->imgs[i].dir);
		put_field(f, rs->imgs[i].image);
		fputc('\n', f);
	}
	fclose(f);
	return rename(tmp, path) == 0;
}

static char *opt(char *s) { return *s ? xstrdup(s) : NULL; }

int recset_load(RecSet *rs, const char *path) {
	size_t n;
	char *d = (char *)read_file(path, &n);
	if (!d) return 0;
	char *line = d, *next;
	int first = 1, ok = 0;
	for (; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		if (first) { first = 0; if (strcmp(line, CACHE_MAGIC)) break; ok = 1; continue; }
		char *fld[16];
		int nf = 0;
		char *p = line;
		while (nf < 16) {
			fld[nf++] = p;
			char *t = strchr(p, '\t');
			if (!t) break;
			*t = 0;
			p = t + 1;
		}
		if (fld[0][0] == 'T' && nf >= 12) {
			Rec r;
			memset(&r, 0, sizeof r);
			r.path = xstrdup(fld[1]);
			r.m.title = opt(fld[2]); r.m.artist = opt(fld[3]); r.m.album = opt(fld[4]); r.m.albumartist = opt(fld[5]);
			r.m.year = atoi(fld[6]); r.m.track = atoi(fld[7]); r.m.disc = atoi(fld[8]);
			r.m.duration_ms = atoi(fld[9]); r.m.format = atoi(fld[10]); r.m.art = atoi(fld[11]);
			rs_add(rs, r);
		} else if (fld[0][0] == 'I' && nf >= 3) {
			rs_add_img(rs, fld[1], fld[2]);
		}
	}
	free(d);
	return ok;
}

/* ---------- building ---------- */

static const char *generic_dirs[] = {
	"music", "музыка", "mp3", "flac", "audio", "аудио", "download", "downloads", "загрузки", "media", "songs",
	"песни", "sdcard", "mnt", "albums", "альбомы", "artists", "исполнители", "playlist", "playlists", "misc",
	"various", "new", "новое", "other", "разное", "sd", "storage", "emulated", "0", NULL
};

static int is_generic_dir(const char *dir, const char *root) {
	if (!strcmp(dir, root) || !strcmp(dir, "/") || !strcmp(dir, ".")) return 1;
	char *f = str_fold(path_basename(dir));
	int r = 0;
	for (int i = 0; generic_dirs[i]; i++) if (!strcmp(f, generic_dirs[i])) { r = 1; break; }
	free(f);
	return r;
}

static int is_disc_dir(const char *name) {
	char *f = str_fold(name);
	const char *pre[] = {"cd", "disc", "disk", "диск", "cd-", NULL};
	int r = 0;
	for (int i = 0; pre[i] && !r; i++) {
		size_t l = strlen(pre[i]);
		if (!strncmp(f, pre[i], l)) {
			const char *p = f + l;
			while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
			if (isdigit((unsigned char)*p)) {
				while (isdigit((unsigned char)*p)) p++;
				r = 1; /* allow suffixes like "CD1 - Live" */
			}
		}
	}
	free(f);
	return r;
}

static char *album_dir_of(const char *path) {
	char *d = path_dirname(path);
	if (is_disc_dir(path_basename(d))) {
		char *p = path_dirname(d);
		free(d);
		d = p;
	}
	return d;
}

static int digits_prefix(const char *s, int *num) {
	int i = 0, v = 0;
	while (isdigit((unsigned char)s[i]) && i < 4) v = v * 10 + (s[i++] - '0');
	if (i && num) *num = v;
	return i;
}

/* derive title (and maybe artist / track) from file name */
static void from_filename(const char *path, char **title, char **artist, int *track) {
	const char *b = path_basename(path);
	char *n = xstrdup(b);
	char *dot = strrchr(n, '.');
	if (dot && dot != n) *dot = 0;
	char *p = n;
	int num = 0, k = digits_prefix(p, &num);
	if (k && (p[k] == ' ' || p[k] == '.' || p[k] == '-' || p[k] == '_' || p[k] == ')')) {
		if (p[k] == '-' && isdigit((unsigned char)p[k + 1])) { /* 1-01 */
			int k2 = digits_prefix(p + k + 1, &num);
			k += 1 + k2;
		}
		if (!*track) *track = num;
		p += k;
		while (*p == ' ' || *p == '.' || *p == '-' || *p == '_' || *p == ')') p++;
	}
	for (char *q = p; *q; q++) if (*q == '_') *q = ' ';
	char *sep = strstr(p, " - ");
	if (sep && !*artist) {
		*sep = 0;
		*artist = xstrdup(p);
		str_trim(*artist);
		p = sep + 3;
	}
	if (!*title) { *title = xstrdup(*p ? p : n); str_trim(*title); }
	free(n);
}

static char *sort_key(const char *s) {
	char *f = str_fold(s);
	char *p = f;
	if (!strncmp(p, "the ", 4)) p += 4;
	while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
	char *r = xstrdup(*p ? p : f);
	free(f);
	return r;
}

typedef struct { char *key; int idx; } KV;
static int kv_cmp(const void *a, const void *b) { return strcmp(((const KV *)a)->key, ((const KV *)b)->key); }

static Library *g_sort_lib;
static int cmp_track_in_album(const void *a, const void *b) {
	const Track *x = &g_sort_lib->tracks[*(const int *)a], *y = &g_sort_lib->tracks[*(const int *)b];
	if (x->disc != y->disc) return x->disc - y->disc;
	if (x->track != y->track) return (x->track ? x->track : 9999) - (y->track ? y->track : 9999);
	return strcmp(x->path, y->path);
}
static int cmp_album_global(const void *a, const void *b) {
	const Album *x = &g_sort_lib->albums[*(const int *)a], *y = &g_sort_lib->albums[*(const int *)b];
	const Artist *ax = &g_sort_lib->artists[x->artist_idx], *ay = &g_sort_lib->artists[y->artist_idx];
	if (ax->special != ay->special) return ax->special - ay->special;
	int c = strcmp(ax->sortkey, ay->sortkey);
	if (c) return c;
	if (x->year != y->year) return (x->year ? x->year : 9999) - (y->year ? y->year : 9999);
	return strcmp(x->sortkey, y->sortkey);
}
static int cmp_artist(const void *a, const void *b) {
	const Artist *x = &g_sort_lib->artists[*(const int *)a], *y = &g_sort_lib->artists[*(const int *)b];
	if (x->special != y->special) return x->special - y->special;
	return strcmp(x->sortkey, y->sortkey);
}
static char **g_title_keys;
static int cmp_title(const void *a, const void *b) {
	int c = strcmp(g_title_keys[*(const int *)a], g_title_keys[*(const int *)b]);
	return c ? c : *(const int *)a - *(const int *)b;
}

static int image_rank(const char *name) {
	char *f = str_fold(name);
	char *dot = strrchr(f, '.');
	if (dot) *dot = 0;
	int r = 50;
	const char *best[] = {"cover", "folder", "front", "album", "albumart", "albumartlarge", NULL};
	for (int i = 0; best[i]; i++) if (!strcmp(f, best[i])) r = i;
	if (r == 50) {
		if (strstr(f, "front") || strstr(f, "cover") || strstr(f, "обложка")) r = 10;
		else if (strstr(f, "albumartsmall")) r = 40;
		else if (strstr(f, "back") || strstr(f, "cd") || strstr(f, "disc") || strstr(f, "inlay") || strstr(f, "inside")) r = 45;
	}
	free(f);
	return r;
}

#define UNKNOWN_ARTIST "Неизвестный исполнитель"
#define VARIOUS_ARTISTS "Разные исполнители"
#define NO_ALBUM "Без альбома"

Library *lib_build(RecSet *rs, const char *root) {
	Library *l = xcalloc(1, sizeof *l);
	g_sort_lib = l;
	int n = rs->nrecs;
	l->tracks = xcalloc(n ? n : 1, sizeof(Track));
	l->ntracks = n;
	char **adirs = xcalloc(n ? n : 1, sizeof(char *));
	KV *keys = xcalloc(n ? n : 1, sizeof(KV));
	for (int i = 0; i < n; i++) {
		Rec *r = &rs->recs[i];
		Track *t = &l->tracks[i];
		t->path = xstrdup(r->path);
		t->title = r->m.title ? xstrdup(r->m.title) : NULL;
		t->artist = r->m.artist ? xstrdup(r->m.artist) : NULL;
		t->album = r->m.album ? xstrdup(r->m.album) : NULL;
		t->albumartist = r->m.albumartist ? xstrdup(r->m.albumartist) : NULL;
		t->year = r->m.year; t->track = r->m.track; t->disc = r->m.disc;
		t->dur_ms = r->m.duration_ms; t->format = r->m.format; t->art = r->m.art;
		if (!t->title) from_filename(t->path, &t->title, &t->artist, &t->track);
		adirs[i] = album_dir_of(t->path);
		if (!t->album) {
			if (is_generic_dir(adirs[i], root)) t->album = xstrdup(NO_ALBUM);
			else t->album = xstrdup(path_basename(adirs[i]));
		}
		char *fa = str_fold(t->album);
		size_t kl = strlen(fa) + strlen(adirs[i]) + 2;
		keys[i].key = xmalloc(kl);
		snprintf(keys[i].key, kl, "%s\x01%s", fa, adirs[i]);
		keys[i].idx = i;
		free(fa);
	}
	qsort(keys, n, sizeof(KV), kv_cmp);

	/* dir images lookup sorted by dir */
	KV *imgk = xcalloc(rs->nimgs ? rs->nimgs : 1, sizeof(KV));
	for (int i = 0; i < rs->nimgs; i++) { imgk[i].key = rs->imgs[i].dir; imgk[i].idx = i; }
	qsort(imgk, rs->nimgs, sizeof(KV), kv_cmp);

	l->albums = xcalloc(n ? n : 1, sizeof(Album));
	for (int i = 0; i < n;) {
		int j = i;
		while (j < n && !strcmp(keys[j].key, keys[i].key)) j++;
		Album *a = &l->albums[l->nalbums];
		int ai = l->nalbums++;
		a->ntracks = j - i;
		a->tracks = xmalloc(a->ntracks * sizeof(int));
		a->art_track = -1;
		Track *t0 = &l->tracks[keys[i].idx];
		a->name = xstrdup(t0->album);
		/* album artist */
		const char *aa = NULL, *art0 = NULL;
		int same_artist = 1, any_artist = 0;
		for (int k = i; k < j; k++) {
			Track *t = &l->tracks[keys[k].idx];
			a->tracks[k - i] = keys[k].idx;
			t->album_idx = ai;
			a->dur_ms += t->dur_ms;
			if (t->year > a->year) a->year = t->year;
			if (t->albumartist && !aa) aa = t->albumartist;
			if (t->artist) {
				any_artist = 1;
				if (!art0) art0 = t->artist;
				else if (str_icmp_utf8(art0, t->artist)) same_artist = 0;
			}
			if (t->art && a->art_track < 0) a->art_track = keys[k].idx;
		}
		if (aa) a->artist = xstrdup(aa);
		else if (any_artist && same_artist) a->artist = xstrdup(art0);
		else if (any_artist) a->artist = xstrdup(VARIOUS_ARTISTS);
		else {
			char *ad = adirs[keys[i].idx];
			char *parent = path_dirname(ad);
			if (strcmp(t0->album, NO_ALBUM) && !is_generic_dir(parent, root)) a->artist = xstrdup(path_basename(parent));
			else a->artist = xstrdup(UNKNOWN_ARTIST);
			free(parent);
		}
		for (int k = i; k < j; k++) {
			Track *t = &l->tracks[keys[k].idx];
			if (!t->artist) t->artist = xstrdup(a->artist);
		}
		/* folder cover */
		const char *ad = adirs[keys[i].idx];
		int best = 1000;
		for (int k = i; k < j && best > 0; k++) {
			char *td = path_dirname(l->tracks[keys[k].idx].path);
			const char *dirs[2] = { td, ad };
			for (int d = 0; d < 2; d++) {
				KV q = { (char *)dirs[d], 0 };
				KV *hit = bsearch(&q, imgk, rs->nimgs, sizeof(KV), kv_cmp);
				if (!hit) continue;
				/* scan neighbours with equal key */
				KV *lo = hit, *hi = hit;
				while (lo > imgk && !strcmp((lo - 1)->key, q.key)) lo--;
				while (hi < imgk + rs->nimgs - 1 && !strcmp((hi + 1)->key, q.key)) hi++;
				for (KV *e = lo; e <= hi; e++) {
					const char *ip = rs->imgs[e->idx].image;
					int rk = image_rank(path_basename(ip)) + (d == 1 && strcmp(td, ad) ? 1 : 0);
					if (rk < best) { best = rk; free(a->cover_path); a->cover_path = xstrdup(ip); }
				}
			}
			free(td);
		}
		if (best >= 40 && a->art_track >= 0) { free(a->cover_path); a->cover_path = NULL; } /* prefer embedded over random pictures */
		qsort(a->tracks, a->ntracks, sizeof(int), cmp_track_in_album);
		a->sortkey = sort_key(a->name);
		i = j;
	}
	for (int i = 0; i < n; i++) { free(keys[i].key); free(adirs[i]); }
	free(keys); free(adirs); free(imgk);

	/* artists */
	KV *ak = xcalloc(l->nalbums ? l->nalbums : 1, sizeof(KV));
	for (int i = 0; i < l->nalbums; i++) { ak[i].key = str_fold(l->albums[i].artist); ak[i].idx = i; }
	qsort(ak, l->nalbums, sizeof(KV), kv_cmp);
	l->artists = xcalloc(l->nalbums ? l->nalbums : 1, sizeof(Artist));
	for (int i = 0; i < l->nalbums;) {
		int j = i;
		while (j < l->nalbums && !strcmp(ak[j].key, ak[i].key)) j++;
		Artist *ar = &l->artists[l->nartists];
		int ri = l->nartists++;
		ar->name = xstrdup(l->albums[ak[i].idx].artist);
		ar->nalbums = j - i;
		ar->albums = xmalloc(ar->nalbums * sizeof(int));
		for (int k = i; k < j; k++) {
			ar->albums[k - i] = ak[k].idx;
			l->albums[ak[k].idx].artist_idx = ri;
			ar->ntracks += l->albums[ak[k].idx].ntracks;
		}
		ar->sortkey = sort_key(ar->name);
		ar->special = !strcmp(ar->name, VARIOUS_ARTISTS) ? 1 : !strcmp(ar->name, UNKNOWN_ARTIST) ? 2 : 0;
		i = j;
	}
	for (int i = 0; i < l->nalbums; i++) free(ak[i].key);
	free(ak);

	/* sort artists (rebuild index mapping) */
	int *ord = xmalloc((l->nartists ? l->nartists : 1) * sizeof(int));
	for (int i = 0; i < l->nartists; i++) ord[i] = i;
	qsort(ord, l->nartists, sizeof(int), cmp_artist);
	Artist *sorted = xmalloc((l->nartists ? l->nartists : 1) * sizeof(Artist));
	int *remap = xmalloc((l->nartists ? l->nartists : 1) * sizeof(int));
	for (int i = 0; i < l->nartists; i++) { sorted[i] = l->artists[ord[i]]; remap[ord[i]] = i; }
	free(l->artists);
	l->artists = sorted;
	for (int i = 0; i < l->nalbums; i++) l->albums[i].artist_idx = remap[l->albums[i].artist_idx];
	free(ord); free(remap);

	/* album order */
	l->albums_sorted = xmalloc((l->nalbums ? l->nalbums : 1) * sizeof(int));
	for (int i = 0; i < l->nalbums; i++) l->albums_sorted[i] = i;
	qsort(l->albums_sorted, l->nalbums, sizeof(int), cmp_album_global);
	for (int i = 0; i < l->nartists; i++)
		qsort(l->artists[i].albums, l->artists[i].nalbums, sizeof(int), cmp_album_global);

	/* tracks by title */
	g_title_keys = xmalloc((n ? n : 1) * sizeof(char *));
	for (int i = 0; i < n; i++) g_title_keys[i] = sort_key(l->tracks[i].title);
	l->tracks_by_title = xmalloc((n ? n : 1) * sizeof(int));
	for (int i = 0; i < n; i++) l->tracks_by_title[i] = i;
	qsort(l->tracks_by_title, n, sizeof(int), cmp_title);
	for (int i = 0; i < n; i++) free(g_title_keys[i]);
	free(g_title_keys);

	/* path hash */
	l->hcap = 16;
	while (l->hcap < n * 2) l->hcap <<= 1;
	l->hash = xmalloc(l->hcap * sizeof(int));
	for (int i = 0; i < l->hcap; i++) l->hash[i] = -1;
	for (int i = 0; i < n; i++) {
		uint32_t h = hash_str(l->tracks[i].path) & (l->hcap - 1);
		while (l->hash[h] >= 0) h = (h + 1) & (l->hcap - 1);
		l->hash[h] = i;
	}
	return l;
}

int lib_find(Library *l, const char *path) {
	if (!l || !path || !l->hcap) return -1;
	uint32_t h = hash_str(path) & (l->hcap - 1);
	while (l->hash[h] >= 0) {
		if (!strcmp(l->tracks[l->hash[h]].path, path)) return l->hash[h];
		h = (h + 1) & (l->hcap - 1);
	}
	return -1;
}

void lib_free(Library *l) {
	if (!l) return;
	for (int i = 0; i < l->ntracks; i++) {
		Track *t = &l->tracks[i];
		free(t->path); free(t->title); free(t->artist); free(t->album); free(t->albumartist);
	}
	for (int i = 0; i < l->nalbums; i++) {
		Album *a = &l->albums[i];
		free(a->name); free(a->artist); free(a->tracks); free(a->cover_path); free(a->sortkey);
	}
	for (int i = 0; i < l->nartists; i++) { free(l->artists[i].name); free(l->artists[i].albums); free(l->artists[i].sortkey); }
	free(l->tracks); free(l->albums); free(l->artists); free(l->tracks_by_title); free(l->albums_sorted); free(l->hash);
	free(l);
}

/* ---------- scanner ---------- */

static const char *audio_ext[] = {".mp3", ".flac", ".ogg", ".oga", ".wav", NULL};
static const char *image_ext[] = {".jpg", ".jpeg", ".png", ".bmp", NULL};
static const char *default_skip_top[] = {
	"Roms", "Bios", "Saves", "Cheats", "Emus", "Tools", "Apps", "Overlays", "Shaders", "Collections",
	"Imgs", "Images", "Themes", "Android", "System Volume Information", "LOST.DIR", "$RECYCLE.BIN", "Recycled", NULL
};

typedef struct {
	char root[512], cache[512], skipf[512];
	int full;
	ScanStatus *st;
	char **skip; int nskip;
	char **paths; int npaths, cappaths;
	RecSet imgs;
} ScanCtx;

static pthread_t g_thread;
static int g_thread_live;

static int has_ext(const char *name, const char **exts) {
	for (int i = 0; exts[i]; i++) if (str_iends_ascii(name, exts[i])) return 1;
	return 0;
}

static int skip_dir(ScanCtx *c, const char *full, const char *name, int depth) {
	if (name[0] == '.') return 1;
	if (depth == 0) for (int i = 0; default_skip_top[i]; i++) if (str_ieq_ascii(name, default_skip_top[i])) return 1;
	for (int i = 0; i < c->nskip; i++) {
		if (c->skip[i][0] == '/') { if (!strcmp(c->skip[i], full)) return 1; }
		else if (!str_icmp_utf8(c->skip[i], name)) return 1;
	}
	return 0;
}

static void walk(ScanCtx *c, const char *dir, int depth) {
	if (depth > 16) return;
	DIR *d = opendir(dir);
	if (!d) return;
	snprintf(c->st->current, sizeof c->st->current, "%s", dir);
	struct dirent *e;
	char path[4096];
	char **subdirs = NULL;
	int nsub = 0, capsub = 0;
	while ((e = readdir(d))) {
		const char *nm = e->d_name;
		if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;
		snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", nm);
		int type = e->d_type;
		if (type == DT_LNK) continue;
		if (type == DT_UNKNOWN) {
			DIR *t = opendir(path);
			if (t) { closedir(t); type = DT_DIR; } else type = DT_REG;
		}
		if (type == DT_DIR) {
			if (skip_dir(c, path, nm, depth)) continue;
			if (nsub == capsub) { capsub = capsub * 2 + 16; subdirs = xrealloc(subdirs, capsub * sizeof(char *)); }
			subdirs[nsub++] = xstrdup(path);
		} else if (type == DT_REG) {
			if (nm[0] == '.') continue; /* macOS "._" files */
			if (has_ext(nm, audio_ext)) {
				if (c->npaths == c->cappaths) { c->cappaths = c->cappaths * 2 + 256; c->paths = xrealloc(c->paths, c->cappaths * sizeof(char *)); }
				c->paths[c->npaths++] = xstrdup(path);
				c->st->found = c->npaths;
			} else if (has_ext(nm, image_ext)) {
				rs_add_img(&c->imgs, dir, path);
			}
		}
	}
	closedir(d);
	for (int i = 0; i < nsub; i++) { walk(c, subdirs[i], depth + 1); free(subdirs[i]); }
	free(subdirs);
}

static int cmp_strp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int cmp_rec(const void *a, const void *b) { return strcmp(((const Rec *)a)->path, ((const Rec *)b)->path); }

static void *scan_thread(void *arg) {
	ScanCtx *c = arg;
	ScanStatus *st = c->st;
	/* skip list */
	size_t n;
	char *sk = (char *)read_file(c->skipf, &n);
	if (sk) {
		char *save = NULL;
		for (char *line = strtok_r(sk, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
			str_trim(line);
			if (!*line || *line == '#') continue;
			c->skip = xrealloc(c->skip, (c->nskip + 1) * sizeof(char *));
			c->skip[c->nskip++] = xstrdup(line);
		}
		free(sk);
	}
	walk(c, c->root, 0);
	if (c->npaths) qsort(c->paths, c->npaths, sizeof(char *), cmp_strp);

	RecSet old = {0};
	if (!c->full) recset_load(&old, c->cache);
	if (old.nrecs) qsort(old.recs, old.nrecs, sizeof(Rec), cmp_rec);

	RecSet *out = xcalloc(1, sizeof *out);
	int changed = c->full;
	/* count new */
	int newc = 0;
	for (int i = 0; i < c->npaths; i++) {
		Rec q = { c->paths[i] };
		if (!old.nrecs || !bsearch(&q, old.recs, old.nrecs, sizeof(Rec), cmp_rec)) newc++;
	}
	st->to_parse = newc;
	if (newc) changed = 1;
	if (old.nrecs != c->npaths - newc) changed = 1; /* something was removed */
	int parsed = 0;
	for (int i = 0; i < c->npaths; i++) {
		Rec q = { c->paths[i] };
		Rec *hit = old.nrecs ? bsearch(&q, old.recs, old.nrecs, sizeof(Rec), cmp_rec) : NULL;
		if (hit) {
			/* move the cached tags; old path stays for further bsearch calls */
			Rec r;
			r.path = c->paths[i];
			r.m = hit->m;
			memset(&hit->m, 0, sizeof hit->m);
			rs_add(out, r);
			continue;
		}
		snprintf(st->current, sizeof st->current, "%s", path_basename(c->paths[i]));
		Rec r;
		memset(&r, 0, sizeof r);
		if (tags_read(c->paths[i], &r.m)) { r.path = c->paths[i]; rs_add(out, r); }
		else free(c->paths[i]);
		st->parsed = ++parsed;
	}
	free(c->paths);
	/* compare image sets to detect changes in covers */
	if (!changed) {
		if (old.nimgs != c->imgs.nimgs) changed = 1;
		else for (int i = 0; i < old.nimgs; i++)
			if (strcmp(old.imgs[i].image, c->imgs.imgs[i].image)) { changed = 1; break; }
	}
	out->imgs = c->imgs.imgs; out->nimgs = c->imgs.nimgs; out->capimgs = c->imgs.capimgs;
	recset_free(&old);
	if (changed) recset_save(out, c->cache);
	for (int i = 0; i < c->nskip; i++) free(c->skip[i]);
	free(c->skip);
	st->result = out;
	st->changed = changed;
	__sync_synchronize();
	st->done = 1;
	st->running = 0;
	free(c);
	return NULL;
}

void scan_start(const char *root, const char *cache_path, const char *skip_file, int full, ScanStatus *st) {
	scan_join();
	ScanCtx *c = xcalloc(1, sizeof *c);
	snprintf(c->root, sizeof c->root, "%s", root);
	snprintf(c->cache, sizeof c->cache, "%s", cache_path);
	snprintf(c->skipf, sizeof c->skipf, "%s", skip_file ? skip_file : "");
	c->full = full;
	c->st = st;
	memset(st, 0, sizeof *st);
	st->running = 1;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 1 << 20);
	if (pthread_create(&g_thread, &at, scan_thread, c) == 0) g_thread_live = 1;
	else scan_thread(c);
	pthread_attr_destroy(&at);
}

void scan_join(void) {
	if (g_thread_live) { pthread_join(g_thread, NULL); g_thread_live = 0; }
}
