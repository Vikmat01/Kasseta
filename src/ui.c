#include "ui.h"
#include "library.h"
#include "covers.h"
#include "audio.h"
#include <math.h>
#include <ctype.h>

/* ---------- theme ---------- */
#define C_BG      HEXC(0x000000)
#define C_SURF    HEXC(0x101012)
#define C_SURF2   HEXC(0x19191c)
#define C_SEL     HEXC(0x1d1d21)
#define C_LINE    HEXC(0x26262b)
#define C_TEXT    HEXC(0xf3f3f3)
#define C_TEXT2   HEXC(0x9b9ba1)
#define C_TEXT3   HEXC(0x5d5d63)
#define C_ACCENT  HEXC(0xff8a3d)
#define C_ACCENT2 HEXC(0xffb37a)

enum { SCR_LIBRARY, SCR_ARTIST, SCR_ALBUM, SCR_PLAYER };
enum { TAB_ARTISTS, TAB_ALBUMS, TAB_TRACKS, NTABS };
enum { VIEW_CASSETTE, VIEW_CD };
enum { REP_OFF, REP_ALL, REP_ONE };

typedef struct {
	int id, arg, sel;
	float scroll, target;
	char *anchor; /* track path used to re-find the item after a rescan */
} Screen;

typedef struct { int sel; float scroll, target; } TabState;

enum {
	MI_VIEW, MI_SHUFFLE, MI_REPEAT, MI_RESCAN, MI_FULLSCAN, MI_SCREEN, MI_QUIT, MI_COUNT
};

struct App {
	Canvas *cv;
	int W, H;
	float S;
	char datadir[512], root[512], skipfile[512], cachefile[600], statefile[600];
	Font *f_reg, *f_semi, *f_bold, *f_hand;

	Library *lib;
	RecSet boot;
	ScanStatus scan;
	int scan_visible;   /* first scan with empty library */

	Screen stack[8];
	int depth;
	TabState tabs[NTABS];
	int tab;

	int view, shuffle, repeat;
	char **queue; int qlen;
	int *order; int qpos;
	int cur_track;      /* library index of current track, -1 */
	char *cur_path;

	int menu_open, menu_sel;
	char toast[160];
	float toast_t;

	float t;            /* seconds since start */
	float reel_l, reel_r, disc_angle, disc_speed;
	int dirty, quit, screen_off;
	float save_timer;
	int last_sec;

	/* now-playing caches */
	Canvas *cass_layer; char *cass_key; int cass_w;
	Image *ph_img; int ph_album, ph_size;
	uint8_t *disc_zone, *disc_alpha, *disc_light; int disc_d;
};

#define SC(v) ((int)((v) * A->S + 0.5f))

static App *A; /* single instance */

/* ---------- small helpers ---------- */

static const char *plural(int n, const char *one, const char *few, const char *many) {
	int a = n % 100, b = n % 10;
	if (a >= 11 && a <= 14) return many;
	if (b == 1) return one;
	if (b >= 2 && b <= 4) return few;
	return many;
}

static void fmt_time(int ms, char *buf) {
	if (ms < 0) ms = 0;
	int s = ms / 1000;
	if (s >= 3600) sprintf(buf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	else sprintf(buf, "%d:%02d", s / 60, s % 60);
}

static void fmt_count(char *buf, int n, const char *one, const char *few, const char *many) {
	sprintf(buf, "%d %s", n, plural(n, one, few, many));
}

static FontSize *F(Font *f, int px) { return font_at(f, SC(px)); }

static void toast(const char *msg) {
	snprintf(A->toast, sizeof A->toast, "%s", msg);
	A->toast_t = 2.2f;
	A->dirty = 1;
}

static Screen *top(void) { return &A->stack[A->depth - 1]; }

static void push(int id, int arg, const char *anchor) {
	if (A->depth >= 8) return;
	Screen *s = &A->stack[A->depth++];
	memset(s, 0, sizeof *s);
	s->id = id; s->arg = arg;
	s->anchor = anchor ? xstrdup(anchor) : NULL;
	A->dirty = 1;
}

static void pop(void) {
	if (A->depth <= 1) return;
	free(top()->anchor);
	A->depth--;
	A->dirty = 1;
}

static uint32_t hue_color(uint32_t h, float s, float v) {
	float hh = (h % 360) / 60.0f;
	int i = (int)hh;
	float f = hh - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	float r, g, b;
	switch (i) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	return RGB((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static void first_letter(const char *s, char *out) {
	const char *p = s;
	uint32_t c;
	while ((c = utf8_next(&p))) {
		if (c > 127 || isalnum((int)c)) {
			if (c >= 'a' && c <= 'z') c -= 32;
			if (c >= 0x430 && c <= 0x44F) c -= 32;
			out[utf8_put(out, c)] = 0;
			return;
		}
	}
	strcpy(out, "♪");
}

/* placeholder art for albums without a cover */
static void draw_placeholder(Canvas *c, const char *name, float x, float y, float sz, float r) {
	uint32_t h = hash_str(name ? name : "");
	uint32_t c0 = hue_color(h % 360, 0.45f, 0.30f), c1 = hue_color((h % 360 + 40) % 360, 0.5f, 0.12f);
	fill_rrect_vgrad(c, x, y, sz, sz, r, c0, c1);
	/* tiny vinyl motif */
	float cx = x + sz * 0.5f, cy = y + sz * 0.5f;
	fill_ring(c, cx, cy, sz * 0.30f, sz * 0.31f, RGBA(255, 255, 255, 22));
	fill_ring(c, cx, cy, sz * 0.22f, sz * 0.225f, RGBA(255, 255, 255, 16));
	char L[8];
	first_letter(name ? name : "?", L);
	int px = (int)(sz * 0.36f);
	if (px >= 8) {
		FontSize *fs = font_at(A->f_bold, px);
		int tw = text_width(fs, L);
		draw_text(c, fs, (int)(cx - tw / 2.0f), (int)(cy - fs_height(fs) / 2.0f), RGBA(255, 255, 255, 200), L, 0);
	}
}

static void draw_cover(int album, float x, float y, int size, float r) {
	int missing = 0;
	Image *img = covers_get(album, size, &missing);
	if (img) blit_rounded(A->cv, img, (int)x, (int)y, r, 255);
	else if (missing) draw_placeholder(A->cv, album >= 0 && A->lib ? A->lib->albums[album].name : "", x, y, size, r);
	else fill_rrect(A->cv, x, y, size, size, r, C_SURF2);
}

/* returns an Image of the cover (or a generated placeholder) at size; may be NULL while loading */
static Image *cover_image(int album, int size) {
	int missing = 0;
	Image *img = covers_get(album, size, &missing);
	if (img) return img;
	if (!missing) return NULL;
	if (!A->ph_img || A->ph_album != album || A->ph_size != size) {
		cv_free(A->ph_img);
		A->ph_img = cv_new(size, size);
		cv_clear(A->ph_img, C_BG);
		draw_placeholder(A->ph_img, album >= 0 && A->lib ? A->lib->albums[album].name : "", 0, 0, size, 0);
		A->ph_album = album; A->ph_size = size;
	}
	return A->ph_img;
}

static void ensure_visible(float *target, float item_top, float item_h, float view_h, float total_h, float margin) {
	if (item_top - margin < *target) *target = item_top - margin;
	if (item_top + item_h + margin > *target + view_h) *target = item_top + item_h + margin - view_h;
	float mx = total_h - view_h;
	if (mx < 0) mx = 0;
	*target = CLAMP(*target, 0, mx);
}

/* ---------- queue / playback ---------- */

static Track *cur_track(void) {
	if (!A->lib || A->cur_track < 0 || A->cur_track >= A->lib->ntracks) return NULL;
	return &A->lib->tracks[A->cur_track];
}

static void rebuild_order(int keep_current) {
	int cur = (A->order && A->qlen && A->qpos < A->qlen) ? A->order[A->qpos] : 0;
	free(A->order);
	A->order = xmalloc((A->qlen ? A->qlen : 1) * sizeof(int));
	for (int i = 0; i < A->qlen; i++) A->order[i] = i;
	if (A->shuffle && A->qlen > 1) {
		for (int i = A->qlen - 1; i > 0; i--) {
			int j = rand() % (i + 1);
			int t = A->order[i]; A->order[i] = A->order[j]; A->order[j] = t;
		}
		if (keep_current) {
			for (int i = 0; i < A->qlen; i++) if (A->order[i] == cur) { A->order[i] = A->order[0]; A->order[0] = cur; break; }
			A->qpos = 0;
		}
	} else if (keep_current) A->qpos = cur;
}

static int load_current(int start_ms, int play) {
	if (!A->qlen) return 0;
	const char *path = A->queue[A->order[A->qpos]];
	free(A->cur_path);
	A->cur_path = xstrdup(path);
	A->cur_track = lib_find(A->lib, path);
	Track *t = cur_track();
	plat_audio_lock();
	int ok = player_load(path, t ? t->dur_ms : 0, start_ms);
	if (ok) player_set_playing(play);
	plat_audio_unlock();
	A->dirty = 1;
	if (!ok) toast("Не удалось открыть файл");
	return ok;
}

static void set_queue(const int *tracks, int n, int start, int shuffle_all) {
	for (int i = 0; i < A->qlen; i++) free(A->queue[i]);
	free(A->queue);
	A->qlen = n;
	A->queue = xmalloc((n ? n : 1) * sizeof(char *));
	for (int i = 0; i < n; i++) A->queue[i] = xstrdup(A->lib->tracks[tracks[i]].path);
	if (shuffle_all) A->shuffle = 1;
	free(A->order); A->order = NULL;
	A->qpos = start;
	A->order = xmalloc((n ? n : 1) * sizeof(int));
	for (int i = 0; i < n; i++) A->order[i] = i;
	A->qpos = CLAMP(start, 0, n - 1);
	if (A->shuffle) {
		if (shuffle_all) A->qpos = rand() % (n ? n : 1);
		rebuild_order(1);
	}
	load_current(0, 1);
}

static void next_track(int manual) {
	if (!A->qlen) return;
	if (!manual && A->repeat == REP_ONE) { load_current(0, 1); return; }
	if (A->qpos + 1 < A->qlen) A->qpos++;
	else if (A->repeat == REP_ALL || manual) {
		A->qpos = 0;
		if (A->shuffle) rebuild_order(0);
	} else {
		/* end of queue: stop on first track */
		A->qpos = 0;
		load_current(0, 0);
		return;
	}
	load_current(0, 1);
}

static void prev_track(void) {
	if (!A->qlen) return;
	plat_audio_lock();
	int pos = player_pos_ms();
	plat_audio_unlock();
	if (pos > 3000) {
		plat_audio_lock();
		player_seek_ms(0);
		plat_audio_unlock();
		return;
	}
	A->qpos = A->qpos > 0 ? A->qpos - 1 : A->qlen - 1;
	load_current(0, 1);
}

static void toggle_play(void) {
	if (!A->qlen) return;
	plat_audio_lock();
	if (!player_has_track()) { plat_audio_unlock(); load_current(0, 1); return; }
	player_set_playing(!player_is_playing());
	plat_audio_unlock();
	A->dirty = 1;
}

static void seek_rel(int ms) {
	plat_audio_lock();
	if (player_has_track()) player_seek_ms(player_pos_ms() + ms);
	plat_audio_unlock();
	A->dirty = 1;
}

static void set_shuffle(int on) {
	A->shuffle = on;
	rebuild_order(1);
	toast(on ? "Перемешивание включено" : "Перемешивание выключено");
}

static void cycle_repeat(void) {
	A->repeat = (A->repeat + 1) % 3;
	toast(A->repeat == REP_OFF ? "Повтор выключен" : A->repeat == REP_ALL ? "Повтор очереди" : "Повтор трека");
}

static int *album_tracks_of_artist(int ai, int *n) {
	Artist *ar = &A->lib->artists[ai];
	int *v = xmalloc((ar->ntracks ? ar->ntracks : 1) * sizeof(int));
	int k = 0;
	for (int i = 0; i < ar->nalbums; i++) {
		Album *al = &A->lib->albums[ar->albums[i]];
		for (int j = 0; j < al->ntracks; j++) v[k++] = al->tracks[j];
	}
	*n = k;
	return v;
}

/* ---------- state ---------- */

void app_save(App *a) {
	FILE *f = fopen(a->statefile, "w");
	if (!f) return;
	plat_audio_lock();
	int pos = player_pos_ms();
	plat_audio_unlock();
	fprintf(f, "view=%d\nshuffle=%d\nrepeat=%d\ntab=%d\nqpos=%d\npos=%d\n", a->view, a->shuffle, a->repeat, a->tab, a->qpos, pos);
	for (int i = 0; i < a->qlen; i++) fprintf(f, "o=%d\n", a->order[i]);
	for (int i = 0; i < a->qlen; i++) fprintf(f, "q=%s\n", a->queue[i]);
	fclose(f);
}

static void load_state(void) {
	size_t n;
	char *d = (char *)read_file(A->statefile, &n);
	if (!d) return;
	int pos = 0, no = 0, cap = 0, ocap = 0;
	int *ord = NULL;
	char *save = NULL;
	for (char *l = strtok_r(d, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(l, "view=", 5)) A->view = atoi(l + 5) ? 1 : 0;
		else if (!strncmp(l, "shuffle=", 8)) A->shuffle = atoi(l + 8) ? 1 : 0;
		else if (!strncmp(l, "repeat=", 7)) A->repeat = CLAMP(atoi(l + 7), 0, 2);
		else if (!strncmp(l, "tab=", 4)) A->tab = CLAMP(atoi(l + 4), 0, NTABS - 1);
		else if (!strncmp(l, "qpos=", 5)) A->qpos = atoi(l + 5);
		else if (!strncmp(l, "pos=", 4)) pos = atoi(l + 4);
		else if (!strncmp(l, "o=", 2)) {
			if (no == ocap) { ocap = ocap * 2 + 64; ord = xrealloc(ord, ocap * sizeof(int)); }
			ord[no++] = atoi(l + 2);
		} else if (!strncmp(l, "q=", 2)) {
			if (A->qlen == cap) { cap = cap * 2 + 64; A->queue = xrealloc(A->queue, cap * sizeof(char *)); }
			A->queue[A->qlen++] = xstrdup(l + 2);
		}
	}
	free(d);
	A->order = xmalloc((A->qlen ? A->qlen : 1) * sizeof(int));
	int valid = no == A->qlen;
	for (int i = 0; i < A->qlen; i++) {
		A->order[i] = valid ? ord[i] : i;
		if (A->order[i] < 0 || A->order[i] >= A->qlen) valid = 0;
	}
	if (!valid) for (int i = 0; i < A->qlen; i++) A->order[i] = i;
	free(ord);
	if (A->qlen) {
		A->qpos = CLAMP(A->qpos, 0, A->qlen - 1);
		FILE *t = fopen(A->queue[A->order[A->qpos]], "rb");
		if (t) { fclose(t); load_current(pos, 0); }
		else { A->cur_path = xstrdup(A->queue[A->order[A->qpos]]); A->cur_track = lib_find(A->lib, A->cur_path); }
	}
}

/* ---------- library swap ---------- */

static void install_library(Library *nl) {
	Library *old = A->lib;
	/* remember anchors */
	for (int i = 1; i < A->depth; i++) {
		Screen *s = &A->stack[i];
		if (!s->anchor && old) {
			if (s->id == SCR_ALBUM && s->arg >= 0 && s->arg < old->nalbums) s->anchor = xstrdup(old->tracks[old->albums[s->arg].tracks[0]].path);
			if (s->id == SCR_ARTIST && s->arg >= 0 && s->arg < old->nartists) {
				Album *al = &old->albums[old->artists[s->arg].albums[0]];
				s->anchor = xstrdup(old->tracks[al->tracks[0]].path);
			}
		}
	}
	A->lib = nl;
	covers_set_library(nl);
	lib_free(old);
	int keep = 1;
	for (int i = 1; i < A->depth; i++) {
		Screen *s = &A->stack[i];
		if (s->id == SCR_PLAYER) continue;
		int ti = s->anchor ? lib_find(nl, s->anchor) : -1;
		if (ti < 0) { keep = i; break; }
		int al = nl->tracks[ti].album_idx;
		s->arg = s->id == SCR_ALBUM ? al : nl->albums[al].artist_idx;
		keep = i + 1;
	}
	if (keep < A->depth) {
		/* item vanished: fall back to the root, keep player screen on top if it was there */
		int had_player = top()->id == SCR_PLAYER;
		while (A->depth > 1) pop();
		if (had_player) push(SCR_PLAYER, 0, NULL);
	}
	for (int i = 0; i < NTABS; i++) A->tabs[i].sel = MAX(0, A->tabs[i].sel);
	A->cur_track = lib_find(nl, A->cur_path);
	cv_free(A->ph_img); A->ph_img = NULL;
	free(A->cass_key); A->cass_key = NULL;
	A->dirty = 1;
}

/* ---------- creation ---------- */

static Font *load_font(const char *dir, const char *name, Font *fb) {
	char p[700];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	Font *f = font_load(p, fb);
	if (!f) { fprintf(stderr, "font missing: %s\n", p); exit(1); }
	return f;
}

App *app_create(const AppConfig *cfg) {
	A = xcalloc(1, sizeof *A);
	A->W = cfg->w; A->H = cfg->h;
	A->S = fminf(cfg->h / 768.0f, cfg->w / 1024.0f);
	A->cv = cv_new(A->W, A->H);
	snprintf(A->datadir, sizeof A->datadir, "%s", cfg->datadir);
	snprintf(A->root, sizeof A->root, "%s", cfg->root);
	snprintf(A->skipfile, sizeof A->skipfile, "%s", cfg->skipfile ? cfg->skipfile : "");
	snprintf(A->cachefile, sizeof A->cachefile, "%s/library.cache", cfg->datadir);
	snprintf(A->statefile, sizeof A->statefile, "%s/state.txt", cfg->datadir);
	Font *fb = load_font(cfg->resdir, "DejaVuSans.ttf", NULL);
	A->f_reg = load_font(cfg->resdir, "Golos-Regular.ttf", fb);
	A->f_semi = load_font(cfg->resdir, "Golos-SemiBold.ttf", fb);
	A->f_bold = load_font(cfg->resdir, "Golos-Bold.ttf", fb);
	A->f_hand = load_font(cfg->resdir, "Caveat-SemiBold.ttf", fb);
	A->cur_track = -1;
	A->tab = TAB_ALBUMS;
	srand((unsigned)now_ms());
	covers_init();
	recset_load(&A->boot, A->cachefile);
	A->lib = lib_build(&A->boot, A->root);
	recset_free(&A->boot);
	covers_set_library(A->lib);
	push(SCR_LIBRARY, 0, NULL);
	load_state();
	A->scan_visible = A->lib->ntracks == 0;
	scan_start(A->root, A->cachefile, A->skipfile, 0, &A->scan);
	A->dirty = 1;
	A->disc_speed = 0;
	return A;
}

void app_destroy(App *a) {
	app_save(a);
	scan_join();
	covers_shutdown();
	plat_audio_lock();
	player_unload();
	plat_audio_unlock();
}

Canvas *app_canvas(App *a) { return a->cv; }
int app_wants_quit(App *a) { return a->quit; }
int app_screen_off(App *a) { return a->screen_off; }
void app_force_redraw(App *a) { a->dirty = 1; }

/* ---------- layout metrics ---------- */

typedef struct { int top, bottom; } Viewport;

static int has_mini(void) { return A->qlen > 0 && top()->id != SCR_PLAYER; }
static int header_h(void) { return SC(84); }
static int footer_h(void) { return SC(50); }
static int mini_h(void) { return SC(76); }

static Viewport content_vp(void) {
	Viewport v = { header_h(), A->H - footer_h() - (has_mini() ? mini_h() : 0) };
	return v;
}

typedef struct { int cols, margin, gap, cell_w, cell_h; } Grid;
static Grid grid_metrics(void) {
	Grid g;
	g.margin = SC(32);
	g.gap = SC(22);
	g.cols = MAX(3, (A->W - 2 * g.margin + g.gap) / (SC(214) + g.gap));
	g.cell_w = (A->W - 2 * g.margin - g.gap * (g.cols - 1)) / g.cols;
	g.cell_h = g.cell_w + SC(64);
	return g;
}

/* ---------- common chrome ---------- */

static void draw_badge(int x, int cy, const char *label) {
	FontSize *fs = F(A->f_bold, 15);
	int tw = text_width(fs, label);
	int h = SC(26);
	int w = MAX(h, tw + SC(14));
	fill_rrect(A->cv, x, cy - h / 2, w, h, h / 2.0f, HEXC(0x2b2b30));
	draw_text(A->cv, fs, x + (w - tw) / 2, cy - fs_height(fs) / 2, C_TEXT, label, 0);
}

static int badge_w(const char *label) {
	FontSize *fs = F(A->f_bold, 15);
	return MAX(SC(26), text_width(fs, label) + SC(14));
}

typedef struct { const char *btn, *label; } Hint;

static void draw_footer(const Hint *hints, int n) {
	int y = A->H - footer_h();
	fill_rect(A->cv, 0, y, A->W, footer_h(), C_BG);
	fill_rect(A->cv, 0, y, A->W, 1, C_LINE);
	FontSize *fs = F(A->f_reg, 17);
	int cy = y + footer_h() / 2;
	int x = SC(24);
	for (int i = 0; i < n; i++) {
		int need = badge_w(hints[i].btn) + SC(8) + text_width(fs, hints[i].label) + SC(22);
		if (x + need > A->W - SC(10)) break;
		draw_badge(x, cy, hints[i].btn);
		x += badge_w(hints[i].btn) + SC(8);
		x += draw_text(A->cv, fs, x, cy - fs_height(fs) / 2, C_TEXT2, hints[i].label, 0);
		x += SC(22);
	}
}

static void draw_play_icon(float cx, float cy, float s, int playing, uint32_t col) {
	if (playing) {
		float w = s * 0.28f, h = s * 0.9f;
		fill_rrect(A->cv, cx - s * 0.36f, cy - h / 2, w, h, w * 0.3f, col);
		fill_rrect(A->cv, cx + s * 0.08f, cy - h / 2, w, h, w * 0.3f, col);
	} else {
		float pts[6] = { cx - s * 0.32f, cy - s * 0.46f, cx + s * 0.48f, cy, cx - s * 0.32f, cy + s * 0.46f };
		fill_poly(A->cv, pts, 3, col);
	}
}

static void draw_eq(float x, float y, float h, uint32_t col, int animate) {
	float bw = h * 0.22f, gap = h * 0.14f;
	for (int i = 0; i < 3; i++) {
		float k = animate ? 0.35f + 0.65f * fabsf(sinf(A->t * (5.0f + i * 1.7f) + i * 1.3f)) : 0.35f + 0.2f * i;
		float bh = h * k;
		fill_rrect(A->cv, x + i * (bw + gap), y + h - bh, bw, bh, bw * 0.4f, col);
	}
}

static void draw_mini_player(void) {
	if (!has_mini()) return;
	int y = A->H - footer_h() - mini_h();
	fill_rect(A->cv, 0, y, A->W, mini_h(), HEXC(0x0c0c0e));
	fill_rect(A->cv, 0, y, A->W, 1, C_LINE);
	plat_audio_lock();
	int pos = player_pos_ms(), len = player_len_ms(), playing = player_is_playing();
	plat_audio_unlock();
	float p = len > 0 ? CLAMP((float)pos / len, 0.f, 1.f) : 0;
	fill_rect(A->cv, 0, y, (int)(A->W * p), SC(3), C_ACCENT);
	Track *t = cur_track();
	int cs = SC(52);
	int cx = SC(24), cy = y + (mini_h() - cs) / 2;
	if (t) draw_cover(t->album_idx, cx, cy, cs, SC(6));
	else draw_placeholder(A->cv, "?", cx, cy, cs, SC(6));
	int tx = cx + cs + SC(16);
	char tm[64], a[16], b[16];
	fmt_time(pos, a); fmt_time(len, b);
	snprintf(tm, sizeof tm, "%s / %s", a, b);
	FontSize *ft = F(A->f_reg, 17);
	int tmw = text_width(ft, tm);
	int right = A->W - SC(24);
	draw_text(A->cv, ft, right - tmw, y + (mini_h() - fs_height(ft)) / 2, C_TEXT2, tm, 0);
	float ic = SC(26);
	draw_play_icon(right - tmw - SC(30), y + mini_h() / 2.0f, ic, playing, C_TEXT);
	int maxw = right - tmw - SC(60) - tx;
	const char *title = t ? t->title : path_basename(A->cur_path ? A->cur_path : "");
	draw_text(A->cv, F(A->f_semi, 20), tx, cy + SC(2), C_TEXT, title, maxw);
	if (t) draw_text(A->cv, F(A->f_reg, 16), tx, cy + SC(29), C_TEXT2, t->artist, maxw);
}

static void draw_scrollbar(Viewport v, float scroll, float total) {
	float vh = v.bottom - v.top;
	if (total <= vh + 1) return;
	float h = fmaxf(SC(40), vh * vh / total);
	float y = v.top + (vh - h) * (scroll / (total - vh));
	fill_rrect(A->cv, A->W - SC(8), y, SC(4), h, SC(2), HEXC(0x3a3a40));
}

static void draw_back_header(const char *title, const char *sub) {
	int h = header_h();
	float cx = SC(36), cy = h / 2.0f;
	draw_line(A->cv, cx + SC(7), cy - SC(12), cx - SC(5), cy, SC(3.5f), C_TEXT2);
	draw_line(A->cv, cx - SC(5), cy, cx + SC(7), cy + SC(12), SC(3.5f), C_TEXT2);
	FontSize *fs = F(A->f_bold, 30);
	FontSize *fsub = F(A->f_reg, 18);
	int subw = sub ? text_width(fsub, sub) : 0;
	draw_text(A->cv, fs, SC(62), (h - fs_height(fs)) / 2, C_TEXT, title, A->W - SC(62) - subw - SC(56));
	if (sub) draw_text(A->cv, fsub, A->W - SC(32) - subw, (h - fs_height(fsub)) / 2, C_TEXT2, sub, 0);
}

/* ---------- library tabs ---------- */

static const char *tab_names[NTABS] = { "Исполнители", "Альбомы", "Треки" };

static void draw_tabs(void) {
	int h = header_h();
	FontSize *fs = F(A->f_semi, 27);
	int x = SC(32);
	int y = (h - fs_height(fs)) / 2 - SC(2);
	for (int i = 0; i < NTABS; i++) {
		int w = text_width(fs, tab_names[i]);
		int on = i == A->tab;
		draw_text(A->cv, fs, x, y, on ? C_TEXT : C_TEXT3, tab_names[i], 0);
		if (on) fill_rrect(A->cv, x, y + fs_height(fs) + SC(6), w, SC(4), SC(2), C_ACCENT);
		x += w + SC(34);
	}
	char sub[64];
	fmt_count(sub, A->lib->ntracks, "трек", "трека", "треков");
	FontSize *fsub = F(A->f_reg, 18);
	int bw = badge_w("L1") + badge_w("R1") + SC(8);
	int sw = text_width(fsub, sub);
	int rx = A->W - SC(32);
	draw_badge(rx - bw, h / 2, "L1");
	draw_badge(rx - badge_w("R1"), h / 2, "R1");
	if (x + sw < rx - bw - SC(20))
		draw_text(A->cv, fsub, rx - bw - SC(20) - sw, (h - fs_height(fsub)) / 2, C_TEXT3, sub, 0);
	if (A->scan.running && !A->scan_visible) {
		/* small activity dot while background rescan runs */
		float a = 0.5f + 0.5f * sinf(A->t * 4);
		fill_circle(A->cv, rx - bw - SC(8), SC(18), SC(4), with_alpha(C_ACCENT, (int)(80 + 150 * a)));
	}
}

static int tab_count(int tab) {
	if (tab == TAB_ARTISTS) return A->lib->nartists;
	if (tab == TAB_ALBUMS) return A->lib->nalbums;
	return A->lib->ntracks;
}

static void draw_selection_bg(float x, float y, float w, float h) {
	fill_rrect(A->cv, x, y, w, h, SC(12), C_SEL);
	fill_rrect(A->cv, x, y + h * 0.22f, SC(4), h * 0.56f, SC(2), C_ACCENT);
}

static void draw_artist_list(TabState *ts) {
	Viewport v = content_vp();
	int rh = SC(76);
	cv_clip(A->cv, 0, v.top, A->W, v.bottom - v.top);
	int first = MAX(0, (int)(ts->scroll / rh) - 1);
	int cur_artist = cur_track() ? A->lib->albums[cur_track()->album_idx].artist_idx : -1;
	for (int i = first; i < A->lib->nartists; i++) {
		float y = v.top + i * rh - ts->scroll + SC(6);
		if (y > v.bottom) break;
		Artist *ar = &A->lib->artists[i];
		int sel = i == ts->sel;
		if (sel) draw_selection_bg(SC(18), y, A->W - SC(36), rh - SC(6));
		int av = SC(54);
		float ay = y + (rh - SC(6) - av) / 2.0f;
		draw_cover(ar->albums[0], SC(34), ay, av, av / 2.0f);
		char sub[96], a1[40], a2[40];
		fmt_count(a1, ar->nalbums, "альбом", "альбома", "альбомов");
		fmt_count(a2, ar->ntracks, "трек", "трека", "треков");
		snprintf(sub, sizeof sub, "%s · %s", a1, a2);
		int tx = SC(34) + av + SC(18);
		int maxw = A->W - tx - SC(70);
		draw_text(A->cv, F(A->f_semi, 23), tx, ay - SC(1), sel ? C_TEXT : HEXC(0xe2e2e2), ar->name, maxw);
		draw_text(A->cv, F(A->f_reg, 17), tx, ay + SC(29), C_TEXT2, sub, maxw);
		if (i == cur_artist) draw_eq(A->W - SC(64), ay + av / 2.0f - SC(9), SC(18), C_ACCENT, player_is_playing());
	}
	cv_noclip(A->cv);
	draw_scrollbar(v, ts->scroll, (float)A->lib->nartists * rh + SC(12));
}

static void draw_album_grid(TabState *ts, const int *albums, int n, int top_offset) {
	Viewport v = content_vp();
	v.top += top_offset;
	Grid g = grid_metrics();
	cv_clip(A->cv, 0, v.top, A->W, v.bottom - v.top);
	int rows = (n + g.cols - 1) / g.cols;
	int cs = g.cell_w;
	FontSize *ft = F(A->f_semi, 19), *fa = F(A->f_reg, 16);
	int cur_album = cur_track() ? cur_track()->album_idx : -1;
	for (int r = MAX(0, (int)(ts->scroll / (g.cell_h + g.gap)) - 1); r < rows; r++) {
		float y = v.top + SC(14) + r * (g.cell_h + g.gap) - ts->scroll;
		if (y > v.bottom) break;
		for (int c = 0; c < g.cols; c++) {
			int i = r * g.cols + c;
			if (i >= n) break;
			Album *al = &A->lib->albums[albums[i]];
			float x = g.margin + c * (g.cell_w + g.gap);
			int sel = i == ts->sel;
			if (sel) fill_rrect(A->cv, x - SC(8), y - SC(8), cs + SC(16), g.cell_h + SC(10), SC(16), C_SEL);
			draw_cover(albums[i], x, y, cs, SC(10));
			if (sel) stroke_rrect(A->cv, x - SC(3), y - SC(3), cs + SC(6), cs + SC(6), SC(12), SC(3), C_ACCENT);
			if (albums[i] == cur_album) {
				fill_circle(A->cv, x + cs - SC(22), y + SC(22), SC(16), RGBA(0, 0, 0, 190));
				draw_eq(x + cs - SC(31), y + SC(13), SC(18), C_ACCENT, player_is_playing());
			}
			draw_text(A->cv, ft, x, y + cs + SC(10), sel ? C_TEXT : HEXC(0xe6e6e6), al->name, cs);
			char sub[200];
			if (al->year) snprintf(sub, sizeof sub, "%s · %d", al->artist, al->year);
			else snprintf(sub, sizeof sub, "%s", al->artist);
			draw_text(A->cv, fa, x, y + cs + SC(36), C_TEXT2, sub, cs);
		}
	}
	cv_noclip(A->cv);
	draw_scrollbar(v, ts->scroll, rows * (float)(g.cell_h + g.gap) + SC(20));
}

static void draw_track_row(int ti, float y, float rh, int sel, int show_cover, int show_number, int various, float x0, float x1) {
	Track *t = &A->lib->tracks[ti];
	if (sel) draw_selection_bg(x0, y, x1 - x0, rh);
	int playing_this = ti == A->cur_track;
	uint32_t tc = playing_this ? C_ACCENT : sel ? C_TEXT : HEXC(0xe2e2e2);
	float x = x0 + SC(16);
	if (show_cover) {
		int cs = (int)(rh - SC(16));
		draw_cover(t->album_idx, x, y + SC(8), cs, SC(6));
		if (playing_this) {
			fill_rrect(A->cv, x, y + SC(8), cs, cs, SC(6), RGBA(0, 0, 0, 150));
			draw_eq(x + cs / 2.0f - SC(9), y + rh / 2 - SC(9), SC(18), C_ACCENT, player_is_playing());
		}
		x += cs + SC(16);
	}
	if (show_number) {
		char num[16];
		if (t->track) sprintf(num, "%d", t->track); else strcpy(num, "–");
		FontSize *fn = F(A->f_reg, 19);
		if (playing_this) draw_eq(x + SC(8), y + rh / 2 - SC(9), SC(18), C_ACCENT, player_is_playing());
		else draw_text_right(A->cv, fn, (int)x + SC(30), (int)(y + (rh - fs_height(fn)) / 2), C_TEXT3, num);
		x += SC(48);
	}
	char dur[16];
	fmt_time(t->dur_ms, dur);
	FontSize *fd = F(A->f_reg, 17);
	int dw = t->dur_ms > 0 ? text_width(fd, dur) : 0;
	if (dw) draw_text(A->cv, fd, (int)(x1 - SC(18) - dw), (int)(y + (rh - fs_height(fd)) / 2), C_TEXT2, dur, 0);
	int maxw = (int)(x1 - SC(36) - dw - x);
	FontSize *ft = F(A->f_semi, 21), *fs = F(A->f_reg, 16);
	if (show_cover || various) {
		char sub[300];
		if (show_cover) snprintf(sub, sizeof sub, "%s · %s", t->artist, t->album);
		else snprintf(sub, sizeof sub, "%s", t->artist);
		float th = fs_height(ft) + fs_height(fs);
		float ty = y + (rh - th) / 2 - SC(1);
		draw_text(A->cv, ft, (int)x, (int)ty, tc, t->title, maxw);
		draw_text(A->cv, fs, (int)x, (int)(ty + fs_height(ft)), C_TEXT2, sub, maxw);
	} else {
		draw_text(A->cv, ft, (int)x, (int)(y + (rh - fs_height(ft)) / 2), tc, t->title, maxw);
	}
}

static void draw_track_list(TabState *ts) {
	Viewport v = content_vp();
	int rh = SC(70);
	cv_clip(A->cv, 0, v.top, A->W, v.bottom - v.top);
	for (int i = MAX(0, (int)(ts->scroll / rh) - 1); i < A->lib->ntracks; i++) {
		float y = v.top + SC(6) + i * rh - ts->scroll;
		if (y > v.bottom) break;
		draw_track_row(A->lib->tracks_by_title[i], y, rh - SC(4), i == ts->sel, 1, 0, 0, SC(18), A->W - SC(18));
	}
	cv_noclip(A->cv);
	draw_scrollbar(v, ts->scroll, (float)A->lib->ntracks * rh + SC(12));
}

static void draw_empty(const char *title, const char *line1, const char *line2) {
	Viewport v = content_vp();
	int cy = (v.top + v.bottom) / 2;
	FontSize *ft = F(A->f_bold, 30), *fs = F(A->f_reg, 19);
	/* cassette-ish glyph */
	float w = SC(120), h = SC(76);
	float x = A->W / 2.0f - w / 2, y = cy - SC(150);
	stroke_rrect(A->cv, x, y, w, h, SC(10), SC(3), C_TEXT3);
	fill_ring(A->cv, x + w * 0.3f, y + h * 0.45f, SC(9), SC(12), C_TEXT3);
	fill_ring(A->cv, x + w * 0.7f, y + h * 0.45f, SC(9), SC(12), C_TEXT3);
	draw_text_center(A->cv, ft, A->W / 2, cy - SC(46), C_TEXT, title, A->W - SC(80));
	if (line1) draw_text_center(A->cv, fs, A->W / 2, cy + SC(4), C_TEXT2, line1, A->W - SC(80));
	if (line2) draw_text_center(A->cv, fs, A->W / 2, cy + SC(34), C_TEXT2, line2, A->W - SC(80));
}

static void draw_scanning(void) {
	cv_clear(A->cv, C_BG);
	int cy = A->H / 2;
	FontSize *ft = F(A->f_bold, 32), *fs = F(A->f_reg, 19);
	/* spinning reel */
	float rx = A->W / 2.0f, ry = cy - SC(120), R = SC(44);
	fill_circle(A->cv, rx, ry, R, HEXC(0x1c1c20));
	fill_circle(A->cv, rx, ry, R * 0.42f, HEXC(0xdadada));
	fill_circle(A->cv, rx, ry, R * 0.26f, C_BG);
	for (int i = 0; i < 6; i++) {
		float a = A->t * 3 + i * (float)M_PI / 3;
		draw_line(A->cv, rx + cosf(a) * R * 0.14f, ry + sinf(a) * R * 0.14f, rx + cosf(a) * R * 0.27f, ry + sinf(a) * R * 0.27f, SC(4), HEXC(0xdadada));
	}
	fill_ring(A->cv, rx, ry, R * 0.55f, R * 0.95f, with_alpha(C_ACCENT, 60));
	draw_text_center(A->cv, ft, A->W / 2, cy - SC(40), C_TEXT, "Ищу музыку на карте памяти", A->W - SC(60));
	char buf[160];
	if (A->scan.to_parse) snprintf(buf, sizeof buf, "Читаю теги: %d из %d", A->scan.parsed, A->scan.to_parse);
	else fmt_count(buf, A->scan.found, "файл найден", "файла найдено", "файлов найдено");
	draw_text_center(A->cv, fs, A->W / 2, cy + SC(14), C_TEXT2, buf, A->W - SC(60));
	char cur[256];
	snprintf(cur, sizeof cur, "%s", A->scan.current);
	draw_text_center(A->cv, F(A->f_reg, 16), A->W / 2, cy + SC(46), C_TEXT3, cur, A->W - SC(80));
	Hint h[] = { {"MENU", "Выход"} };
	draw_footer(h, 1);
}

static void draw_library(void) {
	draw_tabs();
	TabState *ts = &A->tabs[A->tab];
	if (!tab_count(A->tab)) {
		if (A->scan.running) draw_empty("Обновляю библиотеку…", "Подождите немного", NULL);
		else draw_empty("Музыка не найдена", "Скопируйте MP3, FLAC, OGG или WAV в любую папку на карте", "и выберите «Обновить библиотеку» в меню (START)");
	} else if (A->tab == TAB_ARTISTS) draw_artist_list(ts);
	else if (A->tab == TAB_ALBUMS) draw_album_grid(ts, A->lib->albums_sorted, A->lib->nalbums, 0);
	else draw_track_list(ts);
	draw_mini_player();
	Hint h[] = { {"A", A->tab == TAB_TRACKS ? "Играть" : "Открыть"}, {"Y", "Перемешать"}, {"X", "Плеер"}, {"SELECT", "Пауза"}, {"START", "Меню"} };
	if (!tab_count(A->tab)) { Hint e[] = { {"START", "Меню"}, {"MENU", "Выход"} }; draw_footer(e, 2); }
	else draw_footer(h, A->qlen ? 5 : 2);
}

static void draw_artist_screen(Screen *s) {
	Artist *ar = &A->lib->artists[s->arg];
	char sub[96], a1[40], a2[40];
	fmt_count(a1, ar->nalbums, "альбом", "альбома", "альбомов");
	fmt_count(a2, ar->ntracks, "трек", "трека", "треков");
	snprintf(sub, sizeof sub, "%s · %s", a1, a2);
	draw_back_header(ar->name, sub);
	TabState ts = { s->sel, s->scroll, s->target };
	draw_album_grid(&ts, ar->albums, ar->nalbums, 0);
	draw_mini_player();
	Hint h[] = { {"A", "Открыть"}, {"B", "Назад"}, {"Y", "Перемешать все"}, {"X", "Плеер"}, {"START", "Меню"} };
	draw_footer(h, 5);
}

static int album_various(Album *al) {
	for (int i = 1; i < al->ntracks; i++)
		if (str_icmp_utf8(A->lib->tracks[al->tracks[i]].artist, A->lib->tracks[al->tracks[0]].artist)) return 1;
	return 0;
}

static void album_layout(int *cover, int *lx, int *list_x) {
	*cover = MIN(SC(300), (A->W * 3) / 10);
	*lx = SC(32);
	*list_x = *lx + *cover + SC(32);
}

static void draw_album_screen(Screen *s) {
	Album *al = &A->lib->albums[s->arg];
	int cover, lx, list_x;
	album_layout(&cover, &lx, &list_x);
	Viewport v = content_vp();
	v.top = SC(28);
	float cy = v.top;
	draw_cover(s->arg, lx, cy, cover, SC(12));
	float ty = cy + cover + SC(20);
	int lines = draw_text_wrap(A->cv, F(A->f_bold, 27), lx, (int)ty, cover, 3, SC(2), C_TEXT, al->name);
	ty += lines * (fs_height(F(A->f_bold, 27)) + SC(2)) + SC(6);
	draw_text_wrap(A->cv, F(A->f_semi, 20), lx, (int)ty, cover, 2, 0, C_ACCENT, al->artist);
	ty += fs_height(F(A->f_semi, 20)) + SC(10);
	char meta[160], cnt[40], dur[40];
	fmt_count(cnt, al->ntracks, "трек", "трека", "треков");
	int mins = MAX(1, (al->dur_ms + 30000) / 60000);
	if (mins >= 60) sprintf(dur, "%d ч %d мин", mins / 60, mins % 60); else sprintf(dur, "%d мин", mins);
	if (al->year) snprintf(meta, sizeof meta, "%d · %s · %s", al->year, cnt, dur);
	else snprintf(meta, sizeof meta, "%s · %s", cnt, dur);
	draw_text(A->cv, F(A->f_reg, 17), lx, (int)ty, C_TEXT2, meta, cover);
	/* back affordance */
	/* track list */
	int rh = SC(62);
	int various = album_various(al);
	if (various) rh = SC(70);
	Viewport lv = content_vp();
	lv.top = SC(20);
	cv_clip(A->cv, list_x - SC(10), lv.top, A->W - list_x + SC(10), lv.bottom - lv.top);
	int disc = -1, multi_disc = al->ntracks && A->lib->tracks[al->tracks[0]].disc != A->lib->tracks[al->tracks[al->ntracks - 1]].disc;
	for (int i = MAX(0, (int)(s->scroll / rh) - 1); i < al->ntracks; i++) {
		float y = lv.top + i * rh - s->scroll;
		if (y > lv.bottom) break;
		Track *t = &A->lib->tracks[al->tracks[i]];
		if (multi_disc && t->disc != disc && i > 0 && t->track <= 1) {
			fill_rect(A->cv, list_x + SC(16), (int)y, A->W - list_x - SC(50), 1, C_LINE);
		}
		disc = t->disc;
		draw_track_row(al->tracks[i], y, rh - SC(4), i == s->sel, 0, 1, various, list_x - SC(10), A->W - SC(18));
	}
	cv_noclip(A->cv);
	draw_scrollbar(lv, s->scroll, (float)al->ntracks * rh);
	draw_mini_player();
	Hint h[] = { {"A", "Играть"}, {"B", "Назад"}, {"Y", "Перемешать"}, {"X", "Плеер"}, {"START", "Меню"} };
	draw_footer(h, 5);
}

/* ---------- cassette ---------- */

#define CW 100.4f
#define CH 63.8f
#define REEL_L 29.2f
#define REEL_R 71.2f
#define REEL_Y 28.4f
#define HOLE_R 6.0f
#define HUB_R 4.1f
#define PACK_MIN 4.6f
#define PACK_MAX 15.5f
#define WIN_X0 36.6f
#define WIN_X1 63.8f
#define WIN_Y0 21.2f
#define WIN_Y1 35.6f

static void cassette_static(Canvas *c, float u, const char *title, const char *sub) {
	cv_clear(c, C_BG);
	float w = CW * u, h = CH * u;
	/* shell */
	fill_rrect_vgrad(c, 0, 0, w, h, 3.6f * u, HEXC(0x34343a), HEXC(0x1f1f23));
	stroke_rrect(c, 0.5f, 0.5f, w - 1, h - 1, 3.6f * u, 1.2f, HEXC(0x4a4a52));
	stroke_rrect(c, 1.6f * u, 1.6f * u, w - 3.2f * u, h - 3.2f * u, 2.4f * u, 1.0f, RGBA(255, 255, 255, 14));
	/* bottom trapezoid */
	float tr[8] = { 20.5f * u, h - 0.2f * u, 25.5f * u, 47.6f * u, 74.9f * u, 47.6f * u, 79.9f * u, h - 0.2f * u };
	fill_poly(c, tr, 4, HEXC(0x26262b));
	draw_line(c, 25.5f * u, 47.6f * u, 74.9f * u, 47.6f * u, 1.2f, RGBA(255, 255, 255, 26));
	draw_line(c, 20.5f * u, h - 0.4f * u, 25.5f * u, 47.6f * u, 1.0f, RGBA(255, 255, 255, 14));
	draw_line(c, 79.9f * u, h - 0.4f * u, 74.9f * u, 47.6f * u, 1.0f, RGBA(0, 0, 0, 90));
	fill_circle(c, 31.0f * u, 56.6f * u, 2.0f * u, HEXC(0x0c0c0e));
	fill_circle(c, 69.4f * u, 56.6f * u, 2.0f * u, HEXC(0x0c0c0e));
	fill_rrect(c, 40.0f * u, 55.0f * u, 3.0f * u, 3.0f * u, 0.5f * u, HEXC(0x0c0c0e));
	fill_rrect(c, 57.4f * u, 55.0f * u, 3.0f * u, 3.0f * u, 0.5f * u, HEXC(0x0c0c0e));
	fill_rrect(c, 46.2f * u, 53.6f * u, 8.0f * u, 5.6f * u, 0.8f * u, HEXC(0x141417));
	/* screws */
	float sx[5] = { 4.2f, CW - 4.2f, 4.2f, CW - 4.2f, CW / 2 };
	float sy[5] = { 4.2f, 4.2f, CH - 4.2f, CH - 4.2f, 50.6f };
	for (int i = 0; i < 5; i++) {
		fill_circle(c, sx[i] * u, sy[i] * u, 1.7f * u, HEXC(0x55555c));
		fill_circle(c, sx[i] * u, sy[i] * u, 1.2f * u, HEXC(0x3e3e44));
		draw_line(c, (sx[i] - 0.9f) * u, (sy[i] - 0.5f) * u, (sx[i] + 0.9f) * u, (sy[i] + 0.5f) * u, 0.45f * u, HEXC(0x1a1a1d));
	}
	/* label */
	float lx0 = 7.4f * u, ly0 = 4.0f * u, lx1 = (CW - 7.4f) * u, ly1 = 44.6f * u;
	fill_rrect(c, lx0, ly0, lx1 - lx0, ly1 - ly0, 1.6f * u, HEXC(0xece4d2));
	/* accent band */
	float by0 = 17.2f * u, by1 = 39.4f * u;
	fill_rect(c, (int)lx0, (int)by0, (int)(lx1 - lx0 + 1), (int)(by1 - by0), C_ACCENT);
	fill_rect(c, (int)lx0, (int)(by0 + 1.4f * u), (int)(lx1 - lx0 + 1), (int)(0.6f * u + 1), HEXC(0xffd0a8));
	fill_rect(c, (int)lx0, (int)(by1 - 2.0f * u), (int)(lx1 - lx0 + 1), (int)(0.6f * u + 1), HEXC(0xc85a1e));
	/* ruled lines on paper */
	for (int i = 0; i < 2; i++) fill_rect(c, (int)(16.5f * u), (int)((11.0f + i * 4.4f) * u), (int)((lx1 - 18.5f * u) - 16.5f * u), 1, HEXC(0xc9c1b4));
	/* side A badge */
	fill_circle(c, 11.6f * u, 10.0f * u, 3.0f * u, HEXC(0x2a2a30));
	FontSize *fa = font_at(A->f_bold, (int)(4.0f * u));
	draw_text_center(c, fa, (int)(11.6f * u), (int)(10.0f * u - fs_height(fa) / 2.0f), HEXC(0xece4d2), "A", 0);
	FontSize *fsm = font_at(A->f_bold, (int)(2.6f * u));
	draw_text_right(c, fsm, (int)(lx1 - 2.0f * u), (int)(6.2f * u), HEXC(0x7c3a12), "C·90");
	/* handwritten title */
	FontSize *fh = font_at(A->f_hand, (int)(7.4f * u));
	draw_text(c, fh, (int)(16.8f * u), (int)(15.2f * u - fs_ascent(fh)), HEXC(0x1e2a66), title, (int)(lx1 - 22.0f * u - 16.8f * u));
	/* bottom strip */
	FontSize *fb = font_at(A->f_semi, (int)(2.7f * u));
	draw_text_center(c, fb, (int)(w / 2), (int)(40.4f * u), HEXC(0x5b5345), sub, (int)(lx1 - lx0 - 8 * u));
	/* holes & window frames (dynamic contents drawn later) */
	fill_circle(c, REEL_L * u, REEL_Y * u, (HOLE_R + 0.7f) * u, HEXC(0xb85216));
	fill_circle(c, REEL_R * u, REEL_Y * u, (HOLE_R + 0.7f) * u, HEXC(0xb85216));
	fill_rrect(c, (WIN_X0 - 0.7f) * u, (WIN_Y0 - 0.7f) * u, (WIN_X1 - WIN_X0 + 1.4f) * u, (WIN_Y1 - WIN_Y0 + 1.4f) * u, 1.8f * u, HEXC(0xb85216));
}

static void draw_reel_hub(Canvas *c, float cx, float cy, float u, float angle, float pack_r) {
	fill_circle(c, cx, cy, HOLE_R * u, HEXC(0x121214));
	float pr = fminf(pack_r, HOLE_R - 0.25f);
	if (pr > HUB_R) {
		fill_circle(c, cx, cy, pr * u, HEXC(0x3b2618));
		fill_ring(c, cx, cy, (pr - 0.35f) * u, pr * u, HEXC(0x4b3222));
	}
	fill_circle(c, cx, cy, HUB_R * u, HEXC(0xe9e9e6));
	fill_circle(c, cx, cy, 2.75f * u, HEXC(0x121214));
	for (int i = 0; i < 6; i++) {
		float a = angle + i * (float)M_PI / 3;
		float ca = cosf(a), sa = sinf(a);
		float r0 = 1.55f * u, r1 = 2.9f * u, hw = 0.45f * u;
		float pts[8] = {
			cx + ca * r0 - sa * hw, cy + sa * r0 + ca * hw,
			cx + ca * r1 - sa * hw, cy + sa * r1 + ca * hw,
			cx + ca * r1 + sa * hw, cy + sa * r1 - ca * hw,
			cx + ca * r0 + sa * hw, cy + sa * r0 - ca * hw };
		fill_poly(c, pts, 4, HEXC(0xe9e9e6));
	}
	/* small spokes ring marks give the rotation more visibility */
	for (int i = 0; i < 3; i++) {
		float a = angle + i * 2 * (float)M_PI / 3 + 0.5f;
		fill_circle(c, cx + cosf(a) * 3.45f * u, cy + sinf(a) * 3.45f * u, 0.32f * u, HEXC(0xa9a9a4));
	}
	fill_ring(c, cx, cy, (HOLE_R - 0.25f) * u, HOLE_R * u, RGBA(0, 0, 0, 160));
}

static void draw_cassette(float x, float y, float w, float progress) {
	float u = w / CW;
	Track *t = cur_track();
	const char *title = t ? t->title : (A->cur_path ? path_basename(A->cur_path) : "—");
	char sub[300];
	if (t) snprintf(sub, sizeof sub, "%s  ·  %s", t->artist, t->album);
	else snprintf(sub, sizeof sub, " ");
	char key[400];
	snprintf(key, sizeof key, "%s|%s|%d", title, sub, (int)w);
	if (!A->cass_layer || !A->cass_key || strcmp(key, A->cass_key)) {
		cv_free(A->cass_layer);
		A->cass_layer = cv_new((int)ceilf(CW * u) + 1, (int)ceilf(CH * u) + 1);
		cassette_static(A->cass_layer, u, title, sub);
		free(A->cass_key);
		A->cass_key = xstrdup(key);
	}
	blit_opaque(A->cv, A->cass_layer, (int)x, (int)y);
	x = (int)x; y = (int)y;
	float p = CLAMP(progress, 0.f, 1.f);
	float a2 = PACK_MIN * PACK_MIN, b2 = PACK_MAX * PACK_MAX;
	float rl = sqrtf(a2 + (1 - p) * (b2 - a2)), rr = sqrtf(a2 + p * (b2 - a2));
	/* window */
	Canvas *c = A->cv;
	float wx0 = x + WIN_X0 * u, wy0 = y + WIN_Y0 * u, wx1 = x + WIN_X1 * u, wy1 = y + WIN_Y1 * u;
	fill_rrect(c, wx0, wy0, wx1 - wx0, wy1 - wy0, 1.4f * u, HEXC(0x121214));
	cv_clip(c, (int)wx0 + 1, (int)wy0 + 1, (int)(wx1 - wx0) - 2, (int)(wy1 - wy0) - 2);
	fill_circle(c, x + REEL_L * u, y + REEL_Y * u, rl * u, HEXC(0x3b2618));
	fill_circle(c, x + REEL_R * u, y + REEL_Y * u, rr * u, HEXC(0x3b2618));
	fill_ring(c, x + REEL_L * u, y + REEL_Y * u, (rl - 0.4f) * u, rl * u, HEXC(0x55392a));
	fill_ring(c, x + REEL_R * u, y + REEL_Y * u, (rr - 0.4f) * u, rr * u, HEXC(0x55392a));
	/* tape path along the bottom of the window */
	draw_line(c, wx0, wy1 - 1.2f * u, wx1, wy1 - 1.2f * u, 0.5f * u, HEXC(0x2a1a10));
	/* glass reflection */
	float g1[8] = { wx0 + 3 * u, wy0, wx0 + 7 * u, wy0, wx0 + 3 * u, wy1, wx0 - 1 * u, wy1 };
	fill_poly(c, g1, 4, RGBA(255, 255, 255, 14));
	cv_noclip(c);
	stroke_rrect(c, wx0, wy0, wx1 - wx0, wy1 - wy0, 1.4f * u, 1.2f, RGBA(0, 0, 0, 140));
	/* hubs */
	draw_reel_hub(c, x + REEL_L * u, y + REEL_Y * u, u, A->reel_l, rl);
	draw_reel_hub(c, x + REEL_R * u, y + REEL_Y * u, u, A->reel_r, rr);
}

/* ---------- CD ---------- */

static void disc_tables(int D) {
	if (A->disc_d == D && A->disc_zone) return;
	free(A->disc_zone); free(A->disc_alpha); free(A->disc_light);
	A->disc_d = D;
	A->disc_zone = xmalloc(D * D);
	A->disc_alpha = xmalloc(D * D);
	A->disc_light = xmalloc(D * D);
	float R = D / 2.0f;
	for (int j = 0; j < D; j++) {
		for (int i = 0; i < D; i++) {
			int k = j * D + i;
			float dx = i + 0.5f - R, dy = j + 0.5f - R;
			float r = sqrtf(dx * dx + dy * dy);
			float rn = r / R;
			float outer = CLAMP(R - r + 0.5f, 0.f, 1.f);
			float hole = CLAMP(r - 0.125f * R + 0.5f, 0.f, 1.f);
			A->disc_alpha[k] = (uint8_t)(outer * hole * 255);
			int z;
			if (rn > 1.0f) z = 0;
			else if (rn < 0.125f) z = 3;
			else if (rn < 0.30f) z = (rn > 0.255f && rn < 0.27f) ? 5 : 2;
			else if (rn > 0.982f) z = 4;
			else z = 1;
			A->disc_zone[k] = z;
			float phi = atan2f(dy, dx);
			float lobe = powf(fmaxf(0.f, cosf(2 * (phi + 0.75f))), 10.0f);
			float lobe2 = powf(fmaxf(0.f, cosf(2 * (phi - 0.35f))), 24.0f) * 0.5f;
			float l = (lobe + lobe2) * 46 * fminf(1.f, rn * 1.6f) + 4 * sinf(r * 0.7f) * (z == 1);
			A->disc_light[k] = (uint8_t)CLAMP(l, 0.f, 255.f);
		}
	}
}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w) { /* w 0..256 */
	uint32_t rb = ((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8;
	uint32_t g = ((a & 0x00FF00) * (256 - w) + (b & 0x00FF00) * w) >> 8;
	return (rb & 0xFF00FF) | (g & 0x00FF00);
}
static inline uint32_t add_light(uint32_t c, int l) {
	int r = ((c >> 16) & 255) + l, g = ((c >> 8) & 255) + l, b = (c & 255) + l;
	return 0xFF000000u | (MIN(r, 255) << 16) | (MIN(g, 255) << 8) | MIN(b, 255);
}

/* rotating disc; pixels left of clip_x0 are hidden by the sleeve and skipped */
static void draw_disc(Image *src, float cx, float cy, int D, float angle, int clip_x0) {
	disc_tables(D);
	Canvas *c = A->cv;
	int x0 = (int)(cx - D / 2.0f), y0 = (int)(cy - D / 2.0f);
	float R = D / 2.0f;
	float scale = src ? (float)src->w / D : 1;
	int32_t ca = (int32_t)(cosf(angle) * scale * 65536), sa = (int32_t)(sinf(angle) * scale * 65536);
	int i_start = MAX(0, MAX(clip_x0, c->cx0) - x0);
	int i_end = MIN(D, c->cx1 - x0);
	const uint32_t ring = HEXC(0xc4c4ca);
	int sw = src ? src->w : 0, sh = src ? src->h : 0;
	for (int j = 0; j < D; j++) {
		int yy = y0 + j;
		if (yy < c->cy0 || yy >= c->cy1) continue;
		uint32_t *row = c->px + (size_t)yy * c->stride;
		float dy = j + 0.5f - R, dx = i_start + 0.5f - R;
		/* source position in 16.16 (rotate by -angle) */
		int32_t sx = (int32_t)((cosf(angle) * dx + sinf(angle) * dy) * scale * 65536 + R * scale * 65536 - 32768);
		int32_t sy = (int32_t)((-sinf(angle) * dx + cosf(angle) * dy) * scale * 65536 + R * scale * 65536 - 32768);
		const uint8_t *zr = A->disc_zone + j * D, *ar = A->disc_alpha + j * D, *lr = A->disc_light + j * D;
		for (int i = i_start; i < i_end; i++, sx += ca, sy -= sa) {
			int z = zr[i], a = ar[i];
			if (!a || z == 0 || z == 3) continue;
			uint32_t col;
			if (z == 4) col = HEXC(0xa3a3aa);
			else if (z == 5) col = HEXC(0x8d8d94);
			else if (!src) col = HEXC(0x2a2a30);
			else {
				int ix = sx >> 16, iy = sy >> 16;
				uint32_t wx = (sx >> 8) & 255, wy = (sy >> 8) & 255;
				if (ix < 0) { ix = 0; wx = 0; } else if (ix > sw - 2) { ix = sw - 2; wx = 255; }
				if (iy < 0) { iy = 0; wy = 0; } else if (iy > sh - 2) { iy = sh - 2; wy = 255; }
				const uint32_t *p = src->px + iy * src->stride + ix;
				col = lerp_px(lerp_px(p[0], p[1], wx), lerp_px(p[src->stride], p[src->stride + 1], wx), wy);
				if (z == 2) col = lerp_px(col, ring, 200);
			}
			int l = lr[i];
			if (l) col = add_light(col, l);
			uint32_t *d = row + x0 + i;
			*d = a >= 255 ? (col | 0xFF000000u) : (lerp_px(*d, col, a + 1) | 0xFF000000u);
		}
	}
}

static Canvas *cd_layer;
static int cd_layer_album = -2, cd_layer_size, cd_layer_ready;

static void draw_cd(float vx, float vy, float vw, float vh, int album) {
	int sleeve = (int)fminf(vh * 0.94f, vw * 0.58f);
	int D = (int)(sleeve * 0.94f);
	float gw = sleeve + D * 0.55f;
	int gx = (int)(vx + (vw - gw) / 2);
	int sy = (int)(vy + (vh - sleeve) / 2);
	float dcx = gx + sleeve + D * 0.05f, dcy = sy + sleeve / 2.0f;
	Image *disc_src = album >= 0 ? cover_image(album, D) : NULL;
	draw_disc(disc_src, dcx, dcy, D, A->disc_angle, gx + sleeve - SC(8));
	/* sleeve layer (cached) */
	Image *sl = album >= 0 ? cover_image(album, sleeve) : NULL;
	int ready = sl != NULL;
	if (!cd_layer || cd_layer_album != album || cd_layer_size != sleeve || cd_layer_ready != ready) {
		cv_free(cd_layer);
		int pad = SC(16);
		cd_layer = cv_new(sleeve + pad, sleeve + pad);
		for (int i = 0; i < 4; i++)
			fill_rrect(cd_layer, SC(2) + i * SC(2), SC(3) + i * SC(2), sleeve, sleeve, SC(8), RGBA(0, 0, 0, 70));
		if (sl) blit_rounded(cd_layer, sl, 0, 0, SC(6), 255);
		else fill_rrect(cd_layer, 0, 0, sleeve, sleeve, SC(6), C_SURF2);
		stroke_rrect(cd_layer, 0, 0, sleeve, sleeve, SC(6), 1.2f, RGBA(255, 255, 255, 26));
		float gl[6] = { 1, 1, sleeve * 0.55f, 1, 1, sleeve * 0.55f };
		fill_poly(cd_layer, gl, 3, RGBA(255, 255, 255, 12));
		cd_layer_album = album; cd_layer_size = sleeve; cd_layer_ready = ready;
	}
	blit(A->cv, cd_layer, gx, sy);
}

/* ---------- now playing ---------- */

static void draw_player(void) {
	plat_audio_lock();
	int pos = player_pos_ms(), len = player_len_ms(), playing = player_is_playing();
	plat_audio_unlock();
	Track *t = cur_track();
	int album = t ? t->album_idx : -1;
	/* top line */
	FontSize *fsm = F(A->f_semi, 17);
	char q[96];
	if (A->qlen) snprintf(q, sizeof q, "СЕЙЧАС ИГРАЕТ  ·  %d из %d", A->qpos + 1, A->qlen);
	else snprintf(q, sizeof q, "СЕЙЧАС ИГРАЕТ");
	int ty = SC(20);
	float cx = SC(30), cyy = ty + fs_height(fsm) / 2.0f;
	draw_line(A->cv, cx + SC(5), cyy - SC(8), cx - SC(3), cyy, SC(3), C_TEXT3);
	draw_line(A->cv, cx - SC(3), cyy, cx + SC(5), cyy + SC(8), SC(3), C_TEXT3);
	draw_text(A->cv, fsm, SC(48), ty, C_TEXT3, q, A->W / 2);
	const char *rep = A->repeat == REP_OFF ? "ПОВТОР: НЕТ" : A->repeat == REP_ALL ? "ПОВТОР: ВСЕ" : "ПОВТОР: ТРЕК";
	int rx = A->W - SC(30);
	int w1 = text_width(fsm, rep);
	draw_text(A->cv, fsm, rx - w1, ty, A->repeat ? C_ACCENT : C_TEXT3, rep, 0);
	const char *sh = "ПЕРЕМЕШИВАНИЕ";
	int w2 = text_width(fsm, sh);
	draw_text(A->cv, fsm, rx - w1 - SC(26) - w2, ty, A->shuffle ? C_ACCENT : C_TEXT3, sh, 0);

	/* visual */
	float vy = SC(60), vh = SC(448);
	float prog = len > 0 ? (float)pos / len : 0;
	if (A->view == VIEW_CASSETTE) {
		float cwid = fminf(vh * CW / CH, A->W - SC(80));
		float ch = cwid * CH / CW;
		draw_cassette((A->W - cwid) / 2, vy + (vh - ch) / 2, cwid, prog);
	} else {
		draw_cd(SC(40), vy, A->W - SC(80), vh, album);
	}

	/* info */
	float iy = vy + vh + SC(18);
	const char *title = t ? t->title : (A->cur_path ? path_basename(A->cur_path) : "Ничего не играет");
	FontSize *ft = F(A->f_bold, 34), *fa = F(A->f_semi, 22);
	draw_text_center(A->cv, ft, A->W / 2, (int)iy, C_TEXT, title, A->W - SC(100));
	iy += fs_height(ft) + SC(2);
	if (t) {
		char sub[400];
		snprintf(sub, sizeof sub, "%s  —  %s", t->artist, t->album);
		draw_text_center(A->cv, fa, A->W / 2, (int)iy, C_ACCENT2, sub, A->W - SC(100));
	}
	iy += fs_height(fa) + SC(22);
	/* progress row */
	int px0 = SC(150), px1 = A->W - SC(110);
	char a[16], b[16];
	fmt_time(pos, a); fmt_time(len, b);
	FontSize *fti = F(A->f_reg, 18);
	float by = iy + fs_height(fti) / 2.0f;
	fill_circle(A->cv, SC(66), by, SC(26), playing ? C_ACCENT : HEXC(0x2b2b30));
	draw_play_icon(SC(66) + (playing ? 0 : SC(2)), by, SC(22), playing, playing ? C_BG : C_TEXT);
	draw_text_right(A->cv, fti, px0 - SC(14), (int)iy, C_TEXT2, a);
	draw_text(A->cv, fti, px1 + SC(14), (int)iy, C_TEXT2, b, 0);
	fill_rrect(A->cv, px0, by - SC(3), px1 - px0, SC(6), SC(3), HEXC(0x2a2a2f));
	float pw = (px1 - px0) * CLAMP(prog, 0.f, 1.f);
	if (pw > 0) fill_rrect(A->cv, px0, by - SC(3), fmaxf(pw, SC(6)), SC(6), SC(3), C_ACCENT);
	fill_circle(A->cv, px0 + pw, by, SC(9), C_TEXT);

	Hint h[] = { {"A", playing ? "Пауза" : "Играть"}, {"L1 R1", "Трек"}, {"◀ ▶", "Перемотка"},
	             {"Y", A->view == VIEW_CASSETTE ? "CD" : "Кассета"}, {"X", "Перемеш."}, {"SELECT", "Повтор"}, {"B", "Назад"} };
	draw_footer(h, 7);
}

/* ---------- menu overlay ---------- */

static const char *menu_label(int i, char *buf) {
	switch (i) {
	case MI_VIEW: sprintf(buf, "Вид плеера: %s", A->view == VIEW_CASSETTE ? "кассета" : "CD"); break;
	case MI_SHUFFLE: sprintf(buf, "Перемешивание: %s", A->shuffle ? "вкл" : "выкл"); break;
	case MI_REPEAT: sprintf(buf, "Повтор: %s", A->repeat == REP_OFF ? "выкл" : A->repeat == REP_ALL ? "вся очередь" : "один трек"); break;
	case MI_RESCAN: strcpy(buf, "Обновить библиотеку"); break;
	case MI_FULLSCAN: strcpy(buf, "Пересканировать всё заново"); break;
	case MI_SCREEN: strcpy(buf, "Выключить экран (музыка играет)"); break;
	case MI_QUIT: strcpy(buf, "Выход"); break;
	}
	return buf;
}

static void draw_menu(void) {
	fill_rect(A->cv, 0, 0, A->W, A->H, RGBA(0, 0, 0, 170));
	int rh = SC(54);
	int w = SC(560), h = MI_COUNT * rh + SC(96);
	int x = (A->W - w) / 2, y = (A->H - h) / 2;
	fill_rrect(A->cv, x, y, w, h, SC(18), HEXC(0x141417));
	stroke_rrect(A->cv, x, y, w, h, SC(18), 1.2f, HEXC(0x2c2c32));
	draw_text(A->cv, F(A->f_bold, 26), x + SC(28), y + SC(22), C_TEXT, "Меню", 0);
	char buf[128];
	FontSize *fs = F(A->f_reg, 21);
	for (int i = 0; i < MI_COUNT; i++) {
		int iy = y + SC(74) + i * rh;
		int sel = i == A->menu_sel;
		if (sel) draw_selection_bg(x + SC(12), iy, w - SC(24), rh - SC(4));
		draw_text(A->cv, fs, x + SC(32), iy + (rh - SC(4) - fs_height(fs)) / 2, sel ? C_TEXT : HEXC(0xd0d0d4), menu_label(i, buf), w - SC(60));
		if (i <= MI_REPEAT && sel) {
			FontSize *fa = F(A->f_semi, 18);
			draw_text_right(A->cv, fa, x + w - SC(28), iy + (rh - SC(4) - fs_height(fa)) / 2, C_TEXT3, "◀ ▶");
		}
	}
}

static void draw_toast(void) {
	if (A->toast_t <= 0) return;
	FontSize *fs = F(A->f_semi, 19);
	int tw = text_width(fs, A->toast);
	int w = tw + SC(44), h = SC(46);
	int x = (A->W - w) / 2, y = A->H - footer_h() - h - SC(18) - (has_mini() ? mini_h() : 0);
	int al = (int)(255 * CLAMP(A->toast_t * 3, 0.f, 1.f));
	fill_rrect(A->cv, x, y, w, h, h / 2.0f, with_alpha(HEXC(0x2a2a30), al));
	draw_text(A->cv, fs, x + SC(22), y + (h - fs_height(fs)) / 2, with_alpha(C_TEXT, al), A->toast, 0);
}

static void validate_stack(void) {
	while (A->depth > 1) {
		Screen *s = top();
		if (s->id == SCR_ARTIST && (s->arg < 0 || s->arg >= A->lib->nartists)) pop();
		else if (s->id == SCR_ALBUM && (s->arg < 0 || s->arg >= A->lib->nalbums)) pop();
		else break;
	}
}

static void render(void) {
	if (A->scan_visible) { draw_scanning(); return; }
	validate_stack();
	cv_clear(A->cv, C_BG);
	Screen *s = top();
	switch (s->id) {
	case SCR_LIBRARY: draw_library(); break;
	case SCR_ARTIST: draw_artist_screen(s); break;
	case SCR_ALBUM: draw_album_screen(s); break;
	case SCR_PLAYER: draw_player(); break;
	}
	if (A->menu_open) draw_menu();
	draw_toast();
}

/* ---------- input ---------- */

static void list_nav(int *sel, int n, int btn, int page) {
	int s = *sel;
	if (btn == BTN_UP) s--;
	else if (btn == BTN_DOWN) s++;
	else if (btn == BTN_LEFT) s -= page;
	else if (btn == BTN_RIGHT) s += page;
	*sel = CLAMP(s, 0, MAX(0, n - 1));
}

static void grid_nav(int *sel, int n, int cols, int btn) {
	int s = *sel;
	if (btn == BTN_LEFT) s--;
	else if (btn == BTN_RIGHT) s++;
	else if (btn == BTN_UP) { if (s - cols >= 0) s -= cols; }
	else if (btn == BTN_DOWN) { if (s + cols < n) s += cols; else if ((s / cols) < (n - 1) / cols) s = n - 1; }
	*sel = CLAMP(s, 0, MAX(0, n - 1));
}

static void validate_stack(void);
static void update_scroll_targets(void) {
	validate_stack();
	Screen *s = top();
	Viewport v = content_vp();
	float vh = v.bottom - v.top;
	if (s->id == SCR_LIBRARY) {
		TabState *ts = &A->tabs[A->tab];
		int n = tab_count(A->tab);
		ts->sel = CLAMP(ts->sel, 0, MAX(0, n - 1));
		if (A->tab == TAB_ALBUMS) {
			Grid g = grid_metrics();
			int rows = (n + g.cols - 1) / g.cols;
			ensure_visible(&ts->target, (ts->sel / g.cols) * (float)(g.cell_h + g.gap), g.cell_h + SC(14), vh, rows * (float)(g.cell_h + g.gap) + SC(20), SC(10));
		} else {
			int rh = A->tab == TAB_ARTISTS ? SC(76) : SC(70);
			ensure_visible(&ts->target, ts->sel * (float)rh, rh + SC(6), vh, n * (float)rh + SC(12), rh * 0.6f);
		}
	} else if (s->id == SCR_ARTIST) {
		Artist *ar = &A->lib->artists[s->arg];
		Grid g = grid_metrics();
		int rows = (ar->nalbums + g.cols - 1) / g.cols;
		s->sel = CLAMP(s->sel, 0, MAX(0, ar->nalbums - 1));
		ensure_visible(&s->target, (s->sel / g.cols) * (float)(g.cell_h + g.gap), g.cell_h + SC(14), vh, rows * (float)(g.cell_h + g.gap) + SC(20), SC(10));
	} else if (s->id == SCR_ALBUM) {
		Album *al = &A->lib->albums[s->arg];
		int rh = album_various(al) ? SC(70) : SC(62);
		Viewport lv = content_vp();
		lv.top = SC(20);
		s->sel = CLAMP(s->sel, 0, MAX(0, al->ntracks - 1));
		ensure_visible(&s->target, s->sel * (float)rh, rh, lv.bottom - lv.top, al->ntracks * (float)rh, rh * 0.6f);
	}
}

static void open_player(void) {
	if (!A->qlen) { toast("Сначала выберите музыку"); return; }
	if (top()->id != SCR_PLAYER) push(SCR_PLAYER, 0, NULL);
}

static void screen_toggle(void) {
	A->screen_off = !A->screen_off;
	plat_set_screen(!A->screen_off);
	A->dirty = 1;
}

static void menu_activate(int btn) {
	int dir = btn == BTN_LEFT ? -1 : 1;
	switch (A->menu_sel) {
	case MI_VIEW: A->view = !A->view; break;
	case MI_SHUFFLE: set_shuffle(!A->shuffle); break;
	case MI_REPEAT: A->repeat = (A->repeat + 3 + dir) % 3; break;
	case MI_RESCAN:
	case MI_FULLSCAN:
		if (btn != BTN_A) return;
		if (A->scan.running) { toast("Сканирование уже идёт"); break; }
		scan_start(A->root, A->cachefile, A->skipfile, A->menu_sel == MI_FULLSCAN, &A->scan);
		A->menu_open = 0;
		toast("Обновляю библиотеку…");
		break;
	case MI_SCREEN: if (btn == BTN_A) { A->menu_open = 0; screen_toggle(); } break;
	case MI_QUIT: if (btn == BTN_A) A->quit = 1; break;
	}
	A->dirty = 1;
}

void app_input(App *a, int btn, int pressed, int repeat) {
	(void)repeat;
	if (!pressed) return;
	A->dirty = 1;
	if (btn == BTN_MENU) { a->quit = 1; return; }
	if (btn == BTN_POWER) { screen_toggle(); return; }
	if (A->screen_off) {
		/* blind controls */
		if (btn == BTN_A || btn == BTN_SELECT) toggle_play();
		else if (btn == BTN_R1 || btn == BTN_R2) next_track(1);
		else if (btn == BTN_L1 || btn == BTN_L2) prev_track();
		else if (btn == BTN_B || btn == BTN_START) screen_toggle();
		return;
	}
	if (A->scan_visible) return;
	if (A->menu_open) {
		if (btn == BTN_UP) A->menu_sel = (A->menu_sel + MI_COUNT - 1) % MI_COUNT;
		else if (btn == BTN_DOWN) A->menu_sel = (A->menu_sel + 1) % MI_COUNT;
		else if (btn == BTN_A || btn == BTN_LEFT || btn == BTN_RIGHT) menu_activate(btn);
		else if (btn == BTN_B || btn == BTN_START) A->menu_open = 0;
		return;
	}
	if (btn == BTN_START) { A->menu_open = 1; A->menu_sel = 0; return; }
	validate_stack();
	if (btn == BTN_L2) { prev_track(); return; }
	if (btn == BTN_R2) { next_track(1); return; }
	Screen *s = top();
	Library *L = A->lib;
	switch (s->id) {
	case SCR_LIBRARY: {
		TabState *ts = &A->tabs[A->tab];
		int n = tab_count(A->tab);
		if (btn == BTN_L1) { A->tab = (A->tab + NTABS - 1) % NTABS; break; }
		if (btn == BTN_R1) { A->tab = (A->tab + 1) % NTABS; break; }
		if (btn == BTN_X) { open_player(); break; }
		if (btn == BTN_SELECT) { toggle_play(); break; }
		if (!n) break;
		if (btn >= BTN_UP && btn <= BTN_RIGHT) {
			if (A->tab == TAB_ALBUMS) grid_nav(&ts->sel, n, grid_metrics().cols, btn);
			else list_nav(&ts->sel, n, btn, 6);
		} else if (btn == BTN_A) {
			if (A->tab == TAB_ARTISTS) push(SCR_ARTIST, ts->sel, NULL);
			else if (A->tab == TAB_ALBUMS) push(SCR_ALBUM, L->albums_sorted[ts->sel], NULL);
			else { set_queue(L->tracks_by_title, L->ntracks, ts->sel, 0); push(SCR_PLAYER, 0, NULL); }
		} else if (btn == BTN_Y) {
			if (A->tab == TAB_ARTISTS) {
				int k; int *v = album_tracks_of_artist(ts->sel, &k);
				set_queue(v, k, 0, 1); free(v);
			} else if (A->tab == TAB_ALBUMS) {
				Album *al = &L->albums[L->albums_sorted[ts->sel]];
				set_queue(al->tracks, al->ntracks, 0, 1);
			} else set_queue(L->tracks_by_title, L->ntracks, 0, 1);
			push(SCR_PLAYER, 0, NULL);
		}
		break;
	}
	case SCR_ARTIST: {
		Artist *ar = &L->artists[s->arg];
		if (btn == BTN_B) pop();
		else if (btn >= BTN_UP && btn <= BTN_RIGHT) grid_nav(&s->sel, ar->nalbums, grid_metrics().cols, btn);
		else if (btn == BTN_A) push(SCR_ALBUM, ar->albums[s->sel], NULL);
		else if (btn == BTN_Y) { int k; int *v = album_tracks_of_artist(s->arg, &k); set_queue(v, k, 0, 1); free(v); push(SCR_PLAYER, 0, NULL); }
		else if (btn == BTN_X) open_player();
		else if (btn == BTN_SELECT) toggle_play();
		break;
	}
	case SCR_ALBUM: {
		Album *al = &L->albums[s->arg];
		if (btn == BTN_B) pop();
		else if (btn >= BTN_UP && btn <= BTN_RIGHT) list_nav(&s->sel, al->ntracks, btn, 5);
		else if (btn == BTN_A) {
			int was_shuffle = A->shuffle;
			A->shuffle = 0; /* start exactly at the chosen track */
			set_queue(al->tracks, al->ntracks, s->sel, 0);
			A->shuffle = was_shuffle;
			if (was_shuffle) rebuild_order(1);
			push(SCR_PLAYER, 0, NULL);
		} else if (btn == BTN_Y) { set_queue(al->tracks, al->ntracks, 0, 1); push(SCR_PLAYER, 0, NULL); }
		else if (btn == BTN_X) open_player();
		else if (btn == BTN_SELECT) toggle_play();
		break;
	}
	case SCR_PLAYER:
		if (btn == BTN_B) pop();
		else if (btn == BTN_A) toggle_play();
		else if (btn == BTN_L1) prev_track();
		else if (btn == BTN_R1) next_track(1);
		else if (btn == BTN_LEFT) seek_rel(-5000);
		else if (btn == BTN_RIGHT) seek_rel(5000);
		else if (btn == BTN_UP || btn == BTN_Y) A->view = !A->view;
		else if (btn == BTN_DOWN) A->view = !A->view;
		else if (btn == BTN_X) set_shuffle(!A->shuffle);
		else if (btn == BTN_SELECT) cycle_repeat();
		break;
	}
	update_scroll_targets();
}

/* ---------- update ---------- */

int app_animating(App *a) {
	if (a->screen_off) return 0;
	if (a->scan_visible) return 1;
	if (a->toast_t > 0) return 1;
	Screen *s = top();
	if (s->id == SCR_PLAYER && (player_is_playing() || fabsf(a->disc_speed) > 0.01f)) return 1;
	if (s->id == SCR_LIBRARY) { TabState *t = &a->tabs[a->tab]; if (fabsf(t->scroll - t->target) > 0.5f) return 1; }
	else if (fabsf(s->scroll - s->target) > 0.5f) return 1;
	return 0;
}

static int step_scroll(float *s, float target, float dt) {
	if (fabsf(*s - target) < 0.5f) { if (*s != target) { *s = target; return 1; } return 0; }
	*s += (target - *s) * fminf(1.f, dt * 18);
	return 1;
}

int app_update(App *a, float dt) {
	a->t += dt;
	if (a->toast_t > 0) { a->toast_t -= dt; a->dirty = 1; }
	/* scanning */
	if (a->scan.done) {
		a->scan.done = 0;
		scan_join();
		if (a->scan.changed && a->scan.result) install_library(lib_build(a->scan.result, a->root));
		if (a->scan.result) { recset_free(a->scan.result); free(a->scan.result); a->scan.result = NULL; }
		if (a->scan_visible) { a->scan_visible = 0; if (a->lib->nalbums) a->tab = TAB_ALBUMS; }
		update_scroll_targets();
		a->dirty = 1;
	}
	if (a->scan_visible) a->dirty = 1;
	if (covers_poll()) a->dirty = 1;
	/* track end */
	plat_audio_lock();
	int ended = player_take_ended();
	int playing = player_is_playing();
	int pos = player_pos_ms();
	plat_audio_unlock();
	if (ended) next_track(0);
	if (pos / 1000 != a->last_sec) { a->last_sec = pos / 1000; if (has_mini() || top()->id == SCR_PLAYER) a->dirty = 1; }
	/* animation */
	if (playing) {
		float p = 0;
		int len = player_len_ms();
		if (len > 0) p = CLAMP((float)pos / len, 0.f, 1.f);
		float a2 = PACK_MIN * PACK_MIN, b2 = PACK_MAX * PACK_MAX;
		float rl = sqrtf(a2 + (1 - p) * (b2 - a2)), rr = sqrtf(a2 + p * (b2 - a2));
		a->reel_l -= 47.6f / rl * dt * 1.6f;
		a->reel_r -= 47.6f / rr * dt * 1.6f;
	}
	float target_speed = playing ? 4.2f : 0;
	a->disc_speed += (target_speed - a->disc_speed) * fminf(1.f, dt * 3);
	if (fabsf(a->disc_speed - target_speed) < 0.005f) a->disc_speed = target_speed;
	a->disc_angle = fmodf(a->disc_angle + a->disc_speed * dt, 2 * (float)M_PI);
	Screen *s = top();
	if (s->id == SCR_PLAYER && (playing || a->disc_speed > 0.001f)) a->dirty = 1;
	if (s->id == SCR_LIBRARY) { TabState *t = &a->tabs[a->tab]; if (step_scroll(&t->scroll, t->target, dt)) a->dirty = 1; }
	else if (step_scroll(&s->scroll, s->target, dt)) a->dirty = 1;
	/* equalizer animation in lists */
	if (playing && s->id != SCR_PLAYER) a->dirty = 1;
	/* periodic save */
	a->save_timer += dt;
	if (a->save_timer > 30) { a->save_timer = 0; app_save(a); }
	if (a->screen_off) return 0;
	if (!a->dirty) return 0;
	a->dirty = 0;
	render();
	return 1;
}

void app_debug_open(App *a, const char *what, int arg) {
	(void)a;
	if (!strcmp(what, "tab")) { while (A->depth > 1) pop(); A->tab = arg; }
	else if (!strcmp(what, "sel")) { if (top()->id == SCR_LIBRARY) A->tabs[A->tab].sel = arg; else top()->sel = arg; }
	else if (!strcmp(what, "artist")) push(SCR_ARTIST, arg, NULL);
	else if (!strcmp(what, "album")) push(SCR_ALBUM, arg, NULL);
	else if (!strcmp(what, "play_album")) { if (arg >= A->lib->nalbums) return; Album *al = &A->lib->albums[arg]; set_queue(al->tracks, al->ntracks, 0, 0); }
	else if (!strcmp(what, "player")) push(SCR_PLAYER, 0, NULL);
	else if (!strcmp(what, "view")) A->view = arg;
	else if (!strcmp(what, "menu")) { A->menu_open = arg; A->menu_sel = 0; }
	else if (!strcmp(what, "pause")) { plat_audio_lock(); player_set_playing(!arg); plat_audio_unlock(); }
	else if (!strcmp(what, "seek")) { plat_audio_lock(); player_seek_ms(arg); plat_audio_unlock(); }
	else if (!strcmp(what, "scanwait")) { while (!A->scan.done) sleep_ms(20); }
	update_scroll_targets();
	if (top()->id == SCR_LIBRARY) A->tabs[A->tab].scroll = A->tabs[A->tab].target;
	else top()->scroll = top()->target;
	A->dirty = 1;
}
