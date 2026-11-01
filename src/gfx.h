#ifndef GFX_H
#define GFX_H
#include "common.h"

/* 0xAARRGGBB */
#define RGB(r, g, b) (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))
#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define HEXC(h) (0xFF000000u | (h))

typedef struct {
	uint32_t *px;
	int w, h, stride;
	int cx0, cy0, cx1, cy1; /* clip (exclusive max) */
	int own;
} Canvas;
typedef Canvas Image;

Canvas *cv_new(int w, int h);
void cv_free(Canvas *c);
void cv_clip(Canvas *c, int x, int y, int w, int h);
void cv_noclip(Canvas *c);
void cv_clear(Canvas *c, uint32_t color);

void fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t color);
void fill_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t color);
void stroke_rrect(Canvas *c, float x, float y, float w, float h, float r, float t, uint32_t color);
void fill_circle(Canvas *c, float cx, float cy, float r, uint32_t color);
void fill_ring(Canvas *c, float cx, float cy, float r0, float r1, uint32_t color);
void fill_poly(Canvas *c, const float *xy, int n, uint32_t color); /* convex */
void draw_line(Canvas *c, float x0, float y0, float x1, float y1, float w, uint32_t color);
void fill_vgrad(Canvas *c, int x, int y, int w, int h, uint32_t c0, uint32_t c1);
void fill_rrect_vgrad(Canvas *c, float x, float y, float w, float h, float r, uint32_t c0, uint32_t c1);

void blit(Canvas *c, const Image *img, int x, int y);               /* alpha blend */
void blit_opaque(Canvas *c, const Image *img, int x, int y);
void blit_rounded(Canvas *c, const Image *img, int x, int y, float r, int alpha);
void blit_scaled(Canvas *c, const Image *img, float x, float y, float w, float h, float r);

Image *img_load_mem(const uint8_t *data, size_t len, int max_side);
Image *img_load_file(const char *path, int max_side);
Image *img_square(const Image *src, int size); /* center-crop + high quality resize */
Image *img_copy(const Image *src);

/* text */
typedef struct Font Font;
typedef struct FontSize FontSize;
Font *font_load(const char *path, Font *fallback);
FontSize *font_at(Font *f, int px);
int fs_height(FontSize *fs);   /* line height */
int fs_ascent(FontSize *fs);
int text_width(FontSize *fs, const char *s);
/* y is the top of the line box; returns drawn width. maxw>0 truncates with an ellipsis */
int draw_text(Canvas *c, FontSize *fs, int x, int y, uint32_t color, const char *s, int maxw);
int draw_text_center(Canvas *c, FontSize *fs, int cx, int y, uint32_t color, const char *s, int maxw);
int draw_text_right(Canvas *c, FontSize *fs, int rx, int y, uint32_t color, const char *s);
/* word-wrap into at most max_lines lines; returns lines drawn */
int draw_text_wrap(Canvas *c, FontSize *fs, int x, int y, int w, int max_lines, int line_gap, uint32_t color, const char *s);

static inline uint32_t mix_color(uint32_t a, uint32_t b, float t) {
	int ia = (int)(t * 256);
	uint32_t r = (((a >> 16) & 255) * (256 - ia) + ((b >> 16) & 255) * ia) >> 8;
	uint32_t g = (((a >> 8) & 255) * (256 - ia) + ((b >> 8) & 255) * ia) >> 8;
	uint32_t bl = ((a & 255) * (256 - ia) + (b & 255) * ia) >> 8;
	uint32_t al = ((a >> 24) * (256 - ia) + (b >> 24) * ia) >> 8;
	return al << 24 | r << 16 | g << 8 | bl;
}
static inline uint32_t with_alpha(uint32_t c, int a) { return (c & 0xFFFFFF) | ((uint32_t)a << 24); }

#endif
