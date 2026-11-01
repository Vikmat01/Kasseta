#include "gfx.h"
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

Canvas *cv_new(int w, int h) {
	Canvas *c = xcalloc(1, sizeof *c);
	c->w = w; c->h = h; c->stride = w;
	c->px = xcalloc((size_t)w * h, 4);
	c->own = 1;
	cv_noclip(c);
	return c;
}
void cv_free(Canvas *c) {
	if (!c) return;
	if (c->own) free(c->px);
	free(c);
}
void cv_clip(Canvas *c, int x, int y, int w, int h) {
	c->cx0 = MAX(0, x); c->cy0 = MAX(0, y);
	c->cx1 = MIN(c->w, x + w); c->cy1 = MIN(c->h, y + h);
}
void cv_noclip(Canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
void cv_clear(Canvas *c, uint32_t color) {
	for (int y = 0; y < c->h; y++) {
		uint32_t *p = c->px + (size_t)y * c->stride;
		for (int x = 0; x < c->w; x++) p[x] = color;
	}
}

/* blend color (straight alpha in color) with extra coverage a (0..255) */
static inline void blend_px(uint32_t *d, uint32_t c, int a) {
	a = (a * ((c >> 24) + 1)) >> 8;
	if (a <= 0) return;
	uint32_t dv = *d;
	if (a >= 255) { *d = c | 0xFF000000u; return; }
	uint32_t rb = (((c & 0xFF00FF) * a) + ((dv & 0xFF00FF) * (255 - a))) >> 8;
	uint32_t g = (((c & 0x00FF00) * a) + ((dv & 0x00FF00) * (255 - a))) >> 8;
	uint32_t da = dv >> 24;
	uint32_t oa = a + ((da * (255 - a)) >> 8);
	*d = (oa << 24) | (rb & 0xFF00FF) | (g & 0x00FF00);
}

void fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t color) {
	int x0 = MAX(x, c->cx0), y0 = MAX(y, c->cy0), x1 = MIN(x + w, c->cx1), y1 = MIN(y + h, c->cy1);
	if (x0 >= x1 || y0 >= y1) return;
	int opaque = (color >> 24) == 255;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		if (opaque) for (int xx = x0; xx < x1; xx++) p[xx] = color;
		else for (int xx = x0; xx < x1; xx++) blend_px(p + xx, color, 255);
	}
}

static inline int cov(float d) { /* d: signed distance, negative inside */
	float v = 0.5f - d;
	if (v <= 0) return 0;
	if (v >= 1) return 255;
	return (int)(v * 255);
}

static float sd_rrect(float px, float py, float hx, float hy, float r) {
	float qx = fabsf(px) - hx + r, qy = fabsf(py) - hy + r;
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy;
	if (in > 0) in = 0;
	return sqrtf(ox * ox + oy * oy) + in - r;
}

static void bbox(Canvas *c, float x0, float y0, float x1, float y1, int *ix0, int *iy0, int *ix1, int *iy1) {
	*ix0 = MAX(c->cx0, (int)floorf(x0) - 1);
	*iy0 = MAX(c->cy0, (int)floorf(y0) - 1);
	*ix1 = MIN(c->cx1, (int)ceilf(x1) + 1);
	*iy1 = MIN(c->cy1, (int)ceilf(y1) + 1);
}

void fill_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t color) {
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	int x0, y0, x1, y1;
	bbox(c, x, y, x + w, y + h, &x0, &y0, &x1, &y1);
	float cx = x + w / 2, cy = y + h / 2, hx = w / 2, hy = h / 2;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		float py = yy + 0.5f - cy;
		int inner_row = fabsf(py) < hy - r - 1;
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f - cx;
			int a;
			if (inner_row && fabsf(px) < hx - 1) a = 255;
			else if (fabsf(py) < hy - r - 1 && fabsf(px) < hx - r - 1) a = 255;
			else a = cov(sd_rrect(px, py, hx, hy, r));
			if (a) blend_px(p + xx, color, a);
		}
	}
}

void stroke_rrect(Canvas *c, float x, float y, float w, float h, float r, float t, uint32_t color) {
	int x0, y0, x1, y1;
	bbox(c, x, y, x + w, y + h, &x0, &y0, &x1, &y1);
	float cx = x + w / 2, cy = y + h / 2, hx = w / 2, hy = h / 2;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		float py = yy + 0.5f - cy;
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f - cx;
			if (fabsf(py) < hy - r - t - 1 && fabsf(px) < hx - t - 1) {
				int skip = (int)(cx + hx - t - 2);
				if (skip > xx) xx = skip;
				continue;
			}
			float d = sd_rrect(px, py, hx, hy, r);
			float dd = fabsf(d + t / 2) - t / 2;
			int a = cov(dd);
			if (a) blend_px(p + xx, color, a);
		}
	}
}

void fill_circle(Canvas *c, float cx, float cy, float r, uint32_t color) {
	if (r <= 0) return;
	int x0, y0, x1, y1;
	bbox(c, cx - r, cy - r, cx + r, cy + r, &x0, &y0, &x1, &y1);
	float rin = r - 1.0f;
	float rin2 = rin > 0 ? rin * rin : 0;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		float dy = yy + 0.5f - cy;
		for (int xx = x0; xx < x1; xx++) {
			float dx = xx + 0.5f - cx;
			float d2 = dx * dx + dy * dy;
			int a = d2 < rin2 ? 255 : cov(sqrtf(d2) - r);
			if (a) blend_px(p + xx, color, a);
		}
	}
}

void fill_ring(Canvas *c, float cx, float cy, float r0, float r1, uint32_t color) {
	int x0, y0, x1, y1;
	bbox(c, cx - r1, cy - r1, cx + r1, cy + r1, &x0, &y0, &x1, &y1);
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		float dy = yy + 0.5f - cy;
		for (int xx = x0; xx < x1; xx++) {
			float dx = xx + 0.5f - cx;
			float d = sqrtf(dx * dx + dy * dy);
			float sd = fmaxf(r0 - d, d - r1);
			int a = cov(sd);
			if (a) blend_px(p + xx, color, a);
		}
	}
}

void fill_poly(Canvas *c, const float *xy, int n, uint32_t color) {
	float minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
	float area = 0;
	for (int i = 0; i < n; i++) {
		float x = xy[2 * i], y = xy[2 * i + 1];
		minx = fminf(minx, x); maxx = fmaxf(maxx, x); miny = fminf(miny, y); maxy = fmaxf(maxy, y);
		int j = (i + 1) % n;
		area += x * xy[2 * j + 1] - xy[2 * j] * y;
	}
	float sgn = area > 0 ? 1 : -1;
	float nx[16], ny[16], nd[16];
	if (n > 16) n = 16;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		float ex = xy[2 * j] - xy[2 * i], ey = xy[2 * j + 1] - xy[2 * i + 1];
		float l = sqrtf(ex * ex + ey * ey);
		if (l < 1e-6f) l = 1e-6f;
		/* outward normal */
		nx[i] = sgn * ey / l; ny[i] = -sgn * ex / l;
		nd[i] = nx[i] * xy[2 * i] + ny[i] * xy[2 * i + 1];
	}
	int x0, y0, x1, y1;
	bbox(c, minx, miny, maxx, maxy, &x0, &y0, &x1, &y1);
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		float py = yy + 0.5f;
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f, d = -1e9f;
			for (int i = 0; i < n; i++) {
				float v = nx[i] * px + ny[i] * py - nd[i];
				if (v > d) d = v;
			}
			int a = cov(d);
			if (a) blend_px(p + xx, color, a);
		}
	}
}

void draw_line(Canvas *c, float ax, float ay, float bx, float by, float w, uint32_t color) {
	float r = w / 2;
	int x0, y0, x1, y1;
	bbox(c, fminf(ax, bx) - r, fminf(ay, by) - r, fmaxf(ax, bx) + r, fmaxf(ay, by) + r, &x0, &y0, &x1, &y1);
	float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
	if (l2 < 1e-6f) l2 = 1e-6f;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f - ax, py = yy + 0.5f - ay;
			float t = (px * vx + py * vy) / l2;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			float dx = px - vx * t, dy = py - vy * t;
			int a = cov(sqrtf(dx * dx + dy * dy) - r);
			if (a) blend_px(p + xx, color, a);
		}
	}
}

void fill_vgrad(Canvas *c, int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
	for (int i = 0; i < h; i++) fill_rect(c, x, y + i, w, 1, mix_color(c0, c1, h > 1 ? (float)i / (h - 1) : 0));
}

void fill_rrect_vgrad(Canvas *c, float x, float y, float w, float h, float r, uint32_t c0, uint32_t c1) {
	int x0, y0, x1, y1;
	bbox(c, x, y, x + w, y + h, &x0, &y0, &x1, &y1);
	float cx = x + w / 2, cy = y + h / 2, hx = w / 2, hy = h / 2;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = c->px + (size_t)yy * c->stride;
		float py = yy + 0.5f - cy;
		uint32_t col = mix_color(c0, c1, CLAMP((yy - y) / h, 0.f, 1.f));
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f - cx;
			int a = (fabsf(py) < hy - r - 1 && fabsf(px) < hx - 1) ? 255 : cov(sd_rrect(px, py, hx, hy, r));
			if (a) blend_px(p + xx, col, a);
		}
	}
}

void blit(Canvas *c, const Image *img, int x, int y) {
	int x0 = MAX(x, c->cx0), y0 = MAX(y, c->cy0), x1 = MIN(x + img->w, c->cx1), y1 = MIN(y + img->h, c->cy1);
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *d = c->px + (size_t)yy * c->stride;
		const uint32_t *s = img->px + (size_t)(yy - y) * img->stride - x;
		for (int xx = x0; xx < x1; xx++) {
			uint32_t v = s[xx];
			if ((v >> 24) == 255) d[xx] = v;
			else if (v >> 24) blend_px(d + xx, v, 255);
		}
	}
}

void blit_opaque(Canvas *c, const Image *img, int x, int y) {
	int x0 = MAX(x, c->cx0), y0 = MAX(y, c->cy0), x1 = MIN(x + img->w, c->cx1), y1 = MIN(y + img->h, c->cy1);
	if (x0 >= x1) return;
	for (int yy = y0; yy < y1; yy++)
		memcpy(c->px + (size_t)yy * c->stride + x0, img->px + (size_t)(yy - y) * img->stride + (x0 - x), (x1 - x0) * 4);
}

void blit_rounded(Canvas *c, const Image *img, int x, int y, float r, int alpha) {
	int x0 = MAX(x, c->cx0), y0 = MAX(y, c->cy0), x1 = MIN(x + img->w, c->cx1), y1 = MIN(y + img->h, c->cy1);
	float hx = img->w / 2.f, hy = img->h / 2.f;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *d = c->px + (size_t)yy * c->stride;
		const uint32_t *s = img->px + (size_t)(yy - y) * img->stride - x;
		float py = yy - y + 0.5f - hy;
		int edge_row = fabsf(py) > hy - r - 1;
		for (int xx = x0; xx < x1; xx++) {
			int a = alpha;
			if (edge_row) {
				float px = xx - x + 0.5f - hx;
				if (fabsf(px) > hx - r - 1) a = (cov(sd_rrect(px, py, hx, hy, r)) * alpha) >> 8;
			}
			uint32_t v = s[xx];
			if (a >= 255 && (v >> 24) == 255) d[xx] = v;
			else if (a) blend_px(d + xx, v, a);
		}
	}
}

void blit_scaled(Canvas *c, const Image *img, float x, float y, float w, float h, float r) {
	int x0, y0, x1, y1;
	bbox(c, x, y, x + w, y + h, &x0, &y0, &x1, &y1);
	float sx = img->w / w, sy = img->h / h;
	float hx = w / 2, hy = h / 2;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *d = c->px + (size_t)yy * c->stride;
		float fy = (yy + 0.5f - y) * sy - 0.5f;
		int iy = (int)floorf(fy);
		float ty = fy - iy;
		int iy0 = CLAMP(iy, 0, img->h - 1), iy1 = CLAMP(iy + 1, 0, img->h - 1);
		for (int xx = x0; xx < x1; xx++) {
			float px = xx + 0.5f - x - hx, py = yy + 0.5f - y - hy;
			int a = cov(sd_rrect(px, py, hx, hy, r));
			if (!a) continue;
			float fx = (xx + 0.5f - x) * sx - 0.5f;
			int ix = (int)floorf(fx);
			float tx = fx - ix;
			int ix0 = CLAMP(ix, 0, img->w - 1), ix1 = CLAMP(ix + 1, 0, img->w - 1);
			uint32_t p00 = img->px[iy0 * img->stride + ix0], p01 = img->px[iy0 * img->stride + ix1];
			uint32_t p10 = img->px[iy1 * img->stride + ix0], p11 = img->px[iy1 * img->stride + ix1];
			uint32_t top = mix_color(p00, p01, tx), bot = mix_color(p10, p11, tx);
			blend_px(d + xx, mix_color(top, bot, ty), a);
		}
	}
}

/* ---------- images ---------- */

static Image *img_from_rgba(const uint8_t *rgba, int w, int h) {
	Image *im = cv_new(w, h);
	for (int i = 0; i < w * h; i++) {
		const uint8_t *s = rgba + i * 4;
		im->px[i] = (uint32_t)s[3] << 24 | s[0] << 16 | s[1] << 8 | s[2];
	}
	return im;
}

/* area-average downscale by integer factor first (fast + good quality) */
static Image *img_box_shrink(Image *src, int f) {
	int w = src->w / f, h = src->h / f;
	if (w < 1 || h < 1 || f < 2) return NULL;
	Image *o = cv_new(w, h);
	int n = f * f;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t a = 0, r = 0, g = 0, b = 0;
			for (int j = 0; j < f; j++) {
				const uint32_t *s = src->px + (size_t)(y * f + j) * src->stride + x * f;
				for (int i = 0; i < f; i++) {
					uint32_t v = s[i];
					a += v >> 24; r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255;
				}
			}
			o->px[y * w + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
		}
	}
	return o;
}

Image *img_load_mem(const uint8_t *data, size_t len, int max_side) {
	int w, h, ch;
	uint8_t *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
	if (!rgba) return NULL;
	Image *im = img_from_rgba(rgba, w, h);
	stbi_image_free(rgba);
	if (max_side > 0) {
		int f = MIN(w, h) / max_side;
		if (f >= 2) {
			Image *s = img_box_shrink(im, f);
			if (s) { cv_free(im); im = s; }
		}
	}
	return im;
}

Image *img_load_file(const char *path, int max_side) {
	size_t n;
	uint8_t *d = read_file(path, &n);
	if (!d) return NULL;
	Image *im = img_load_mem(d, n, max_side);
	free(d);
	return im;
}

Image *img_square(const Image *src, int size) {
	int side = MIN(src->w, src->h);
	int ox = (src->w - side) / 2, oy = (src->h - side) / 2;
	Image *o = cv_new(size, size);
	float scale = (float)side / size;
	if (scale <= 1.0f) {
		Image tmp = *src;
		tmp.px = src->px + oy * src->stride + ox;
		tmp.w = side; tmp.h = side;
		tmp.own = 0;
		cv_noclip(o);
		blit_scaled(o, &tmp, 0, 0, size, size, 0);
		return o;
	}
	/* area sampling */
	for (int y = 0; y < size; y++) {
		float sy0 = y * scale, sy1 = (y + 1) * scale;
		for (int x = 0; x < size; x++) {
			float sx0 = x * scale, sx1 = (x + 1) * scale;
			float a = 0, r = 0, g = 0, b = 0, wsum = 0;
			for (int yy = (int)sy0; yy < (int)ceilf(sy1) && yy < side; yy++) {
				float wy = fminf(sy1, yy + 1) - fmaxf(sy0, yy);
				const uint32_t *row = src->px + (size_t)(yy + oy) * src->stride + ox;
				for (int xx = (int)sx0; xx < (int)ceilf(sx1) && xx < side; xx++) {
					float wx = fminf(sx1, xx + 1) - fmaxf(sx0, xx);
					float wt = wx * wy;
					uint32_t v = row[xx];
					a += (v >> 24) * wt; r += ((v >> 16) & 255) * wt; g += ((v >> 8) & 255) * wt; b += (v & 255) * wt;
					wsum += wt;
				}
			}
			if (wsum <= 0) wsum = 1;
			o->px[y * size + x] = (uint32_t)(a / wsum + .5f) << 24 | (uint32_t)(r / wsum + .5f) << 16 |
			                      (uint32_t)(g / wsum + .5f) << 8 | (uint32_t)(b / wsum + .5f);
		}
	}
	return o;
}

Image *img_copy(const Image *src) {
	Image *o = cv_new(src->w, src->h);
	for (int y = 0; y < src->h; y++) memcpy(o->px + y * o->stride, src->px + y * src->stride, src->w * 4);
	return o;
}

/* ---------- text ---------- */

typedef struct Glyph {
	uint32_t cp;
	int w, h, xoff, yoff;
	float adv;
	uint8_t *bm;
	struct Glyph *next;
} Glyph;

struct FontSize {
	Font *font;
	int px;
	float scale, fb_scale;
	int ascent, lineh;
	Glyph *tab[256];
	struct FontSize *next;
};

struct Font {
	stbtt_fontinfo info;
	uint8_t *data;
	Font *fallback;
	FontSize *sizes;
};

Font *font_load(const char *path, Font *fallback) {
	size_t n;
	uint8_t *d = read_file(path, &n);
	if (!d) return NULL;
	Font *f = xcalloc(1, sizeof *f);
	f->data = d;
	f->fallback = fallback;
	if (!stbtt_InitFont(&f->info, d, stbtt_GetFontOffsetForIndex(d, 0))) { free(d); free(f); return NULL; }
	return f;
}

FontSize *font_at(Font *f, int px) {
	for (FontSize *s = f->sizes; s; s = s->next) if (s->px == px) return s;
	FontSize *s = xcalloc(1, sizeof *s);
	s->font = f;
	s->px = px;
	s->scale = stbtt_ScaleForMappingEmToPixels(&f->info, px);
	if (f->fallback) s->fb_scale = stbtt_ScaleForMappingEmToPixels(&f->fallback->info, px);
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
	s->ascent = (int)roundf(asc * s->scale);
	s->lineh = (int)roundf((asc - desc) * s->scale);
	s->next = f->sizes;
	f->sizes = s;
	return s;
}

int fs_height(FontSize *fs) { return fs->lineh; }
int fs_ascent(FontSize *fs) { return fs->ascent; }

static Glyph *get_glyph(FontSize *fs, uint32_t cp) {
	uint32_t hsh = (cp * 2654435761u) >> 24;
	for (Glyph *g = fs->tab[hsh]; g; g = g->next) if (g->cp == cp) return g;
	Glyph *g = xcalloc(1, sizeof *g);
	g->cp = cp;
	stbtt_fontinfo *info = &fs->font->info;
	float sc = fs->scale;
	int gi = stbtt_FindGlyphIndex(info, cp);
	if (!gi && fs->font->fallback) {
		int gi2 = stbtt_FindGlyphIndex(&fs->font->fallback->info, cp);
		if (gi2) { info = &fs->font->fallback->info; gi = gi2; sc = fs->fb_scale; }
	}
	int adv, lsb;
	stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
	g->adv = adv * sc;
	g->bm = stbtt_GetGlyphBitmap(info, sc, sc, gi, &g->w, &g->h, &g->xoff, &g->yoff);
	g->next = fs->tab[hsh];
	fs->tab[hsh] = g;
	return g;
}

int text_width(FontSize *fs, const char *s) {
	float w = 0;
	uint32_t c;
	while ((c = utf8_next(&s))) w += get_glyph(fs, c)->adv;
	return (int)ceilf(w);
}

static float draw_glyph(Canvas *c, FontSize *fs, float x, int base, uint32_t color, Glyph *g) {
	int gx = (int)floorf(x + 0.5f) + g->xoff, gy = base + g->yoff;
	for (int j = 0; j < g->h; j++) {
		int yy = gy + j;
		if (yy < c->cy0 || yy >= c->cy1) continue;
		uint32_t *p = c->px + (size_t)yy * c->stride;
		const uint8_t *b = g->bm + j * g->w;
		for (int i = 0; i < g->w; i++) {
			int xx = gx + i;
			if (xx < c->cx0 || xx >= c->cx1 || !b[i]) continue;
			blend_px(p + xx, color, b[i]);
		}
	}
	(void)fs;
	return g->adv;
}

int draw_text(Canvas *c, FontSize *fs, int x, int y, uint32_t color, const char *s, int maxw) {
	int base = y + fs->ascent;
	float cx = x;
	if (maxw > 0 && text_width(fs, s) > maxw) {
		Glyph *ell = get_glyph(fs, 0x2026);
		float lim = maxw - ell->adv;
		uint32_t ch;
		const char *p = s;
		while ((ch = utf8_next(&p))) {
			Glyph *g = get_glyph(fs, ch);
			if (cx - x + g->adv > lim) break;
			cx += draw_glyph(c, fs, cx, base, color, g);
		}
		/* strip trailing space before ellipsis */
		cx += draw_glyph(c, fs, cx, base, color, ell);
		return (int)(cx - x);
	}
	uint32_t ch;
	while ((ch = utf8_next(&s))) cx += draw_glyph(c, fs, cx, base, color, get_glyph(fs, ch));
	return (int)(cx - x);
}

int draw_text_center(Canvas *c, FontSize *fs, int cx, int y, uint32_t color, const char *s, int maxw) {
	int w = text_width(fs, s);
	if (maxw > 0 && w > maxw) w = maxw;
	return draw_text(c, fs, cx - w / 2, y, color, s, maxw);
}

int draw_text_right(Canvas *c, FontSize *fs, int rx, int y, uint32_t color, const char *s) {
	int w = text_width(fs, s);
	return draw_text(c, fs, rx - w, y, color, s, 0);
}

int draw_text_wrap(Canvas *c, FontSize *fs, int x, int y, int w, int max_lines, int gap, uint32_t color, const char *s) {
	int lines = 0;
	const char *p = s;
	while (*p && lines < max_lines) {
		while (*p == ' ') p++;
		/* find longest prefix ending at a space that fits */
		const char *q = p, *best = NULL;
		float lw = 0;
		while (*q) {
			const char *nq = q;
			uint32_t ch = utf8_next(&nq);
			lw += get_glyph(fs, ch)->adv;
			if (lw > w) break;
			if (ch == ' ') best = q;
			q = nq;
		}
		const char *end;
		if (!*q) end = q;
		else if (best && best > p) end = best;
		else end = q > p ? q : p + 1;
		char *line = xstrndup(p, end - p);
		if (lines == max_lines - 1 && *end) {
			char *rest = xstrdup(p);
			draw_text(c, fs, x, y + lines * (fs->lineh + gap), color, rest, w);
			free(rest);
		} else draw_text(c, fs, x, y + lines * (fs->lineh + gap), color, line, 0);
		free(line);
		lines++;
		p = end;
	}
	return lines;
}
