#include "common.h"
#include <time.h>
#include <ctype.h>

void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}
void *xcalloc(size_t n, size_t s) {
	void *p = calloc(n ? n : 1, s ? s : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}
void *xrealloc(void *p, size_t n) {
	p = realloc(p, n ? n : 1);
	if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
	return p;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) {
	char *d = xmalloc(n + 1);
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void sleep_ms(int ms) {
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

uint32_t utf8_next(const char **ps) {
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = *s;
	if (!c) return 0;
	int n = 0;
	if (c < 0x80) n = 0;
	else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
	else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
	else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
	else { *ps += 1; return 0xFFFD; }
	s++;
	for (int i = 0; i < n; i++) {
		if ((s[i] & 0xC0) != 0x80) { *ps = (const char *)s + i; return 0xFFFD; }
		c = (c << 6) | (s[i] & 0x3F);
	}
	*ps = (const char *)s + n;
	return c;
}

int utf8_put(char *o, uint32_t c) {
	if (c < 0x80) { o[0] = c; return 1; }
	if (c < 0x800) { o[0] = 0xC0 | (c >> 6); o[1] = 0x80 | (c & 0x3F); return 2; }
	if (c < 0x10000) { o[0] = 0xE0 | (c >> 12); o[1] = 0x80 | ((c >> 6) & 0x3F); o[2] = 0x80 | (c & 0x3F); return 3; }
	o[0] = 0xF0 | (c >> 18); o[1] = 0x80 | ((c >> 12) & 0x3F); o[2] = 0x80 | ((c >> 6) & 0x3F); o[3] = 0x80 | (c & 0x3F);
	return 4;
}

uint32_t cp_fold(uint32_t c) {
	if (c >= 'A' && c <= 'Z') return c + 32;
	if (c >= 0x410 && c <= 0x42F) return c + 32;      /* А-Я */
	if (c >= 0x400 && c <= 0x40F) return c + 80;      /* Ѐ-Џ */
	if (c == 0x451 || c == 0x401) return 0x435;       /* ё -> е */
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
	return c;
}

char *str_fold(const char *s) {
	size_t n = strlen(s);
	char *o = xmalloc(n * 2 + 4), *p = o;
	uint32_t c;
	while ((c = utf8_next(&s))) p += utf8_put(p, cp_fold(c));
	*p = 0;
	return o;
}

/* order: digits/latin/cyrillic by code point after folding; leading "the " ignored */
int str_icmp_utf8(const char *a, const char *b) {
	for (;;) {
		uint32_t ca = cp_fold(utf8_next(&a));
		uint32_t cb = cp_fold(utf8_next(&b));
		if (ca != cb || !ca) return (int)ca - (int)cb;
	}
}

int str_ieq_ascii(const char *a, const char *b) {
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
		a++; b++;
	}
	return *a == *b;
}

int str_iends_ascii(const char *s, const char *suf) {
	size_t n = strlen(s), m = strlen(suf);
	return n >= m && str_ieq_ascii(s + n - m, suf);
}

void str_trim(char *s) {
	size_t n = strlen(s);
	while (n && (unsigned char)s[n - 1] <= ' ') s[--n] = 0;
	size_t i = 0;
	while (s[i] && (unsigned char)s[i] <= ' ') i++;
	if (i) memmove(s, s + i, n - i + 1);
}

uint32_t hash_str(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
	return h;
}

long file_size(FILE *f) {
	long cur = ftell(f);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, cur, SEEK_SET);
	return n;
}

uint8_t *read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	long n = file_size(f);
	if (n < 0) { fclose(f); return NULL; }
	uint8_t *b = xmalloc(n + 1);
	size_t r = fread(b, 1, n, f);
	fclose(f);
	b[r] = 0;
	if (len) *len = r;
	return b;
}

uint8_t *read_file_range(const char *path, long off, size_t len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, off, SEEK_SET)) { fclose(f); return NULL; }
	uint8_t *b = xmalloc(len + 1);
	size_t r = fread(b, 1, len, f);
	fclose(f);
	if (r != len) { free(b); return NULL; }
	return b;
}

const char *path_basename(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}
char *path_dirname(const char *p) {
	const char *s = strrchr(p, '/');
	if (!s) return xstrdup(".");
	if (s == p) return xstrdup("/");
	return xstrndup(p, s - p);
}

void str_addn(Str *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2 + 64;
		b->s = xrealloc(b->s, b->cap);
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = 0;
}
void str_add(Str *b, const char *s) { str_addn(b, s, strlen(s)); }
void str_addc(Str *b, char c) { str_addn(b, &c, 1); }
