/* Tag readers: ID3v1/ID3v2.2-2.4, FLAC + Vorbis comments, Ogg Vorbis, RIFF WAVE INFO.
 * Also computes duration cheaply without decoding. */
#include "tags.h"
#include <ctype.h>

static const uint16_t cp1251_hi[64] = {
	0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
	0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0020,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
	0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
	0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
};

static int valid_utf8_multibyte(const uint8_t *s, size_t n) {
	int multi = 0;
	for (size_t i = 0; i < n;) {
		uint8_t c = s[i];
		int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (k < 0 || (k == 1 && c < 0xC2)) return 0;
		if (i + k >= n + (k ? 0 : 1) && k) return 0;
		for (int j = 1; j <= k; j++) if (i + j >= n || (s[i + j] & 0xC0) != 0x80) return 0;
		if (k) multi = 1;
		i += k + 1;
	}
	return multi;
}

/* 8-bit text of unknown code page: UTF-8 if valid, cp1251 if it looks Russian, else latin-1 */
char *text_8bit_to_utf8(const uint8_t *s, size_t n) {
	while (n && s[n - 1] == 0) n--;
	for (size_t i = 0; i < n; i++) if (!s[i]) { n = i; break; }
	if (valid_utf8_multibyte(s, n)) return xstrndup((const char *)s, n);
	int cyr = 0, ascii_letters = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] >= 0xC0 || s[i] == 0xA8 || s[i] == 0xB8) cyr++;
		else if (isalpha(s[i])) ascii_letters++;
	}
	int use1251 = cyr >= 2 && cyr * 2 >= ascii_letters;
	char *o = xmalloc(n * 3 + 1), *p = o;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = s[i];
		if (c >= 0x80) {
			if (use1251) c = c >= 0xC0 ? 0x410 + (c - 0xC0) : cp1251_hi[c - 0x80];
		}
		p += utf8_put(p, c);
	}
	*p = 0;
	return o;
}

static char *utf16_to_utf8(const uint8_t *s, size_t n, int be) {
	if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { be = 0; s += 2; n -= 2; }
	else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { be = 1; s += 2; n -= 2; }
	char *o = xmalloc(n * 2 + 4), *p = o;
	for (size_t i = 0; i + 1 < n; i += 2) {
		uint32_t c = be ? (s[i] << 8 | s[i + 1]) : (s[i + 1] << 8 | s[i]);
		if (!c) break;
		if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
			uint32_t d = be ? (s[i + 2] << 8 | s[i + 3]) : (s[i + 3] << 8 | s[i + 2]);
			if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
		}
		p += utf8_put(p, c);
	}
	*p = 0;
	return o;
}

static char *id3_text(const uint8_t *d, size_t n) {
	if (!n) return NULL;
	int enc = d[0];
	d++; n--;
	char *r;
	if (enc == 1) r = utf16_to_utf8(d, n, 0);
	else if (enc == 2) r = utf16_to_utf8(d, n, 1);
	else if (enc == 3) {
		size_t k = 0;
		while (k < n && d[k]) k++;
		r = xstrndup((const char *)d, k);
	} else r = text_8bit_to_utf8(d, n);
	str_trim(r);
	if (!*r) { free(r); return NULL; }
	return r;
}

static void set_str(char **dst, char *v) {
	if (!v) return;
	if (*dst || !*v) { free(v); return; }
	*dst = v;
}

static int parse_int_prefix(const char *s) {
	while (*s && !isdigit((unsigned char)*s)) s++;
	return atoi(s);
}

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[3] << 24 | b[2] << 16 | b[1] << 8 | b[0]; }
static uint32_t syncsafe(const uint8_t *b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }

static size_t deunsync(uint8_t *b, size_t n) {
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		b[o++] = b[i];
		if (b[i] == 0xFF && i + 1 < n && b[i + 1] == 0x00) i++;
	}
	return o;
}

static void apply_field(Meta *m, const char *key, char *val) {
	if (!val) return;
	if (!strcmp(key, "title")) set_str(&m->title, val);
	else if (!strcmp(key, "artist")) set_str(&m->artist, val);
	else if (!strcmp(key, "album")) set_str(&m->album, val);
	else if (!strcmp(key, "albumartist")) set_str(&m->albumartist, val);
	else {
		if (!strcmp(key, "track") && !m->track) m->track = parse_int_prefix(val);
		else if (!strcmp(key, "disc") && !m->disc) m->disc = parse_int_prefix(val);
		else if (!strcmp(key, "year") && !m->year) {
			int y = parse_int_prefix(val);
			if (y > 1000 && y < 3000) m->year = y;
		}
		free(val);
	}
}

/* ---------- ID3v2 ---------- */

typedef struct { const uint8_t *data; size_t len; int pictype; } Pic;

/* parse id3v2 tag in buf (whole tag incl. header). Returns tag total size. */
static void id3v2_parse(uint8_t *tag, size_t tagsize, int ver, int flags, Meta *m, Pic *pic) {
	size_t pos = 0;
	if (flags & 0x80 && ver < 4) tagsize = deunsync(tag, tagsize);
	if (flags & 0x40) {
		if (ver == 3 && tagsize >= 4) pos = 4 + be32(tag);
		else if (ver == 4 && tagsize >= 4) pos = syncsafe(tag);
	}
	int idlen = ver == 2 ? 3 : 4, hdrlen = ver == 2 ? 6 : 10;
	while (pos + hdrlen <= tagsize) {
		uint8_t *h = tag + pos;
		if (!h[0]) break;
		char id[5] = {0};
		memcpy(id, h, idlen);
		size_t fsz;
		int fflags = 0;
		if (ver == 2) fsz = h[3] << 16 | h[4] << 8 | h[5];
		else if (ver == 4) {
			fsz = (h[4] | h[5] | h[6] | h[7]) & 0x80 ? be32(h + 4) : syncsafe(h + 4);
			fflags = h[8] << 8 | h[9];
		} else { fsz = be32(h + 4); fflags = h[8] << 8 | h[9]; }
		pos += hdrlen;
		if (fsz > tagsize - pos) break;
		uint8_t *d = tag + pos;
		size_t n = fsz;
		pos += fsz;
		if (ver == 4) {
			if (fflags & 0x000C) continue; /* compressed / encrypted */
			if (fflags & 0x0001) { if (n < 4) continue; d += 4; n -= 4; }
			if (fflags & 0x0002) n = deunsync(d, n);
		} else if (ver == 3) {
			if (fflags & 0x00C0) continue;
			if (fflags & 0x0020) { if (!n) continue; d++; n--; }
		}
		const char *key = NULL;
		if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) key = "title";
		else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) key = "artist";
		else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) key = "albumartist";
		else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) key = "album";
		else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) key = "track";
		else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) key = "disc";
		else if (!strcmp(id, "TYER") || !strcmp(id, "TYE") || !strcmp(id, "TDRC") || !strcmp(id, "TORY") || !strcmp(id, "TDOR")) key = "year";
		if (key) { apply_field(m, key, id3_text(d, n)); continue; }
		if (!strcmp(id, "APIC") || !strcmp(id, "PIC")) {
			m->art = ART_ID3;
			if (!pic || n < 4) continue;
			int enc = d[0];
			size_t i = 1;
			if (ver == 2) i += 3;
			else { while (i < n && d[i]) i++; i++; }
			if (i >= n) continue;
			int ptype = d[i++];
			if (enc == 1 || enc == 2) {
				while (i + 1 < n && (d[i] || d[i + 1])) i += 2;
				i += 2;
			} else { while (i < n && d[i]) i++; i++; }
			if (i >= n) continue;
			if (!pic->data || (ptype == 3 && pic->pictype != 3)) {
				pic->data = d + i; pic->len = n - i; pic->pictype = ptype;
			}
		}
	}
}

/* reads ID3v2 at file start; returns size to skip (0 if none). buf receives tag (caller frees) if wanted */
static long id3v2_read(FILE *f, Meta *m, uint8_t **tagbuf, Pic *pic) {
	uint8_t h[10];
	fseek(f, 0, SEEK_SET);
	if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return 0;
	int ver = h[3], flags = h[5];
	size_t sz = syncsafe(h + 6);
	long total = 10 + sz + ((flags & 0x10) ? 10 : 0);
	if (ver < 2 || ver > 4 || sz > 64 * 1024 * 1024) return total;
	uint8_t *tag = xmalloc(sz + 1);
	if (fread(tag, 1, sz, f) != sz) { free(tag); return total; }
	id3v2_parse(tag, sz, ver, flags, m, pic);
	if (tagbuf) *tagbuf = tag; else free(tag);
	return total;
}

static void id3v1_read(FILE *f, Meta *m, long fsize) {
	uint8_t t[128];
	if (fsize < 128) return;
	fseek(f, fsize - 128, SEEK_SET);
	if (fread(t, 1, 128, f) != 128 || memcmp(t, "TAG", 3)) return;
	char *s;
	s = text_8bit_to_utf8(t + 3, 30); str_trim(s); if (*s) set_str(&m->title, s); else free(s);
	s = text_8bit_to_utf8(t + 33, 30); str_trim(s); if (*s) set_str(&m->artist, s); else free(s);
	s = text_8bit_to_utf8(t + 63, 30); str_trim(s); if (*s) set_str(&m->album, s); else free(s);
	if (!m->year) { char y[5] = {0}; memcpy(y, t + 93, 4); int v = atoi(y); if (v > 1000 && v < 3000) m->year = v; }
	if (!m->track && t[125] == 0 && t[126]) m->track = t[126];
}

/* ---------- MP3 duration ---------- */
static const int br_v1l3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const int br_v2l3[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
static const int br_v1l2[16] = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
static const int sr_tab[3] = {44100, 48000, 32000};

static int mp3_header_ok(const uint8_t *b) {
	if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return 0;
	int ver = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3, bri = b[2] >> 4, sri = (b[2] >> 2) & 3;
	return ver != 1 && layer != 0 && bri != 0 && bri != 15 && sri != 3;
}

static int mp3_duration(FILE *f, long start, long end) {
	uint8_t buf[16384];
	fseek(f, start, SEEK_SET);
	size_t n = fread(buf, 1, sizeof buf, f);
	for (size_t i = 0; i + 200 < n; i++) {
		if (!mp3_header_ok(buf + i)) continue;
		const uint8_t *h = buf + i;
		int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
		int v1 = ver == 3;
		int sr = sr_tab[(h[2] >> 2) & 3] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
		int bri = h[2] >> 4;
		int br = layer == 1 ? (v1 ? br_v1l3[bri] : br_v2l3[bri]) : layer == 2 ? (v1 ? br_v1l2[bri] : br_v2l3[bri]) : br_v1l2[bri];
		int spf = layer == 3 ? 384 : (layer == 1 && !v1) ? 576 : 1152;
		int mono = (h[3] >> 6) == 3;
		/* verify there's a second frame header right after this one */
		int flen = layer == 3 ? (12 * br * 1000 / sr + ((h[2] >> 1) & 1)) * 4 : spf / 8 * br * 1000 / sr + ((h[2] >> 1) & 1);
		if (flen < 24) continue;
		if (i + flen + 4 < n && !mp3_header_ok(buf + i + flen)) continue;
		int xo = layer == 1 ? (v1 ? (mono ? 21 : 36) : (mono ? 13 : 21)) : 36;
		const uint8_t *x = h + xo;
		if (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) {
			uint32_t fl = be32(x + 4);
			if (fl & 1) {
				uint32_t frames = be32(x + 8);
				if (frames) return (int)((double)frames * spf * 1000.0 / sr);
			}
		}
		if (!memcmp(h + 36, "VBRI", 4)) {
			uint32_t frames = be32(h + 36 + 14);
			if (frames) return (int)((double)frames * spf * 1000.0 / sr);
		}
		long bytes = end - (start + (long)i);
		if (br > 0 && bytes > 0) return (int)((double)bytes * 8.0 / br);
		return 0;
	}
	return 0;
}

/* ---------- Vorbis comments ---------- */
static void vorbis_comments(const uint8_t *d, size_t n, Meta *m, int *has_pic) {
	if (n < 8) return;
	size_t p = 4 + le32(d);
	if (p + 4 > n) return;
	uint32_t cnt = le32(d + p);
	p += 4;
	for (uint32_t i = 0; i < cnt && p + 4 <= n; i++) {
		uint32_t len = le32(d + p);
		p += 4;
		if (len > n - p) break;
		const char *c = (const char *)d + p;
		const char *eq = memchr(c, '=', len);
		if (eq) {
			size_t kl = eq - c;
			char key[32];
			if (kl < sizeof key) {
				for (size_t k = 0; k < kl; k++) key[k] = toupper((unsigned char)c[k]);
				key[kl] = 0;
				const char *f = NULL;
				if (!strcmp(key, "TITLE")) f = "title";
				else if (!strcmp(key, "ARTIST")) f = "artist";
				else if (!strcmp(key, "ALBUM")) f = "album";
				else if (!strcmp(key, "ALBUMARTIST") || !strcmp(key, "ALBUM ARTIST") || !strcmp(key, "ALBUM_ARTIST")) f = "albumartist";
				else if (!strcmp(key, "TRACKNUMBER")) f = "track";
				else if (!strcmp(key, "DISCNUMBER")) f = "disc";
				else if (!strcmp(key, "DATE") || !strcmp(key, "YEAR") || !strcmp(key, "ORIGINALDATE")) f = "year";
				else if (!strcmp(key, "METADATA_BLOCK_PICTURE") || !strcmp(key, "COVERART")) { if (has_pic) *has_pic = 1; }
				if (f) {
					char *v = xstrndup(eq + 1, len - kl - 1);
					str_trim(v);
					if (*v) apply_field(m, f, v); else free(v);
				}
			}
		}
		p += len;
	}
}

static int b64v(int c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
static uint8_t *b64_decode(const char *s, size_t n, size_t *out) {
	uint8_t *o = xmalloc(n * 3 / 4 + 4);
	size_t k = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (size_t i = 0; i < n; i++) {
		int v = b64v((unsigned char)s[i]);
		if (v < 0) continue;
		acc = acc << 6 | v;
		bits += 6;
		if (bits >= 8) { bits -= 8; o[k++] = (acc >> bits) & 0xFF; }
	}
	*out = k;
	return o;
}

/* FLAC PICTURE block body -> image bytes */
static int flac_picture(const uint8_t *d, size_t n, const uint8_t **img, size_t *len, int *ptype) {
	if (n < 32) return 0;
	*ptype = be32(d);
	size_t p = 4;
	uint32_t l = be32(d + p); p += 4 + l;
	if (p + 4 > n) return 0;
	l = be32(d + p); p += 4 + l;
	if (p + 20 > n) return 0;
	p += 16;
	l = be32(d + p); p += 4;
	if (l > n - p) return 0;
	*img = d + p;
	*len = l;
	return 1;
}

/* ---------- FLAC ---------- */
static int flac_read(FILE *f, long start, Meta *m, uint8_t **art, size_t *artlen) {
	uint8_t h[4];
	fseek(f, start, SEEK_SET);
	if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return 0;
	int best = -1;
	for (int guard = 0; guard < 256; guard++) {
		if (fread(h, 1, 4, f) != 4) break;
		int last = h[0] & 0x80, type = h[0] & 0x7F;
		uint32_t len = h[1] << 16 | h[2] << 8 | h[3];
		if (type == 0 || type == 4 || (type == 6 && art)) {
			uint8_t *d = xmalloc(len + 1);
			if (fread(d, 1, len, f) != len) { free(d); break; }
			if (type == 0 && len >= 18) {
				uint32_t sr = d[10] << 12 | d[11] << 4 | d[12] >> 4;
				uint64_t total = ((uint64_t)(d[13] & 0x0F) << 32) | be32(d + 14);
				if (sr) m->duration_ms = (int)(total * 1000 / sr);
			} else if (type == 4) vorbis_comments(d, len, m, NULL);
			else if (type == 6) {
				const uint8_t *img; size_t il; int pt;
				if (flac_picture(d, len, &img, &il, &pt) && (best < 0 || (pt == 3 && best != 3))) {
					free(*art);
					*art = xmalloc(il);
					memcpy(*art, img, il);
					*artlen = il;
					best = pt;
				}
			}
			free(d);
		} else {
			if (type == 6) m->art = ART_FLAC;
			fseek(f, len, SEEK_CUR);
		}
		if (type == 6) m->art = ART_FLAC;
		if (last) break;
	}
	return 1;
}

/* ---------- Ogg ---------- */
typedef struct { FILE *f; uint32_t serial; int have_serial; } OggR;

/* read next packet (up to maxlen); returns malloc'd packet or NULL */
static uint8_t *ogg_packet(OggR *r, size_t *plen, size_t maxlen, uint64_t *granule) {
	Str pk = {0};
	for (int pages = 0; pages < 4096; pages++) {
		uint8_t h[27], segs[255];
		if (fread(h, 1, 27, r->f) != 27 || memcmp(h, "OggS", 4)) { free(pk.s); return NULL; }
		int ns = h[26];
		if (fread(segs, 1, ns, r->f) != (size_t)ns) { free(pk.s); return NULL; }
		if (granule) *granule = (uint64_t)le32(h + 6) | (uint64_t)le32(h + 10) << 32;
		uint32_t serial = le32(h + 14);
		if (!r->have_serial) { r->serial = serial; r->have_serial = 1; }
		if (serial != r->serial) {
			int tot = 0;
			for (int i = 0; i < ns; i++) tot += segs[i];
			fseek(r->f, tot, SEEK_CUR);
			continue;
		}
		int done = 0;
		for (int i = 0; i < ns; i++) {
			uint8_t tmp[255];
			if (fread(tmp, 1, segs[i], r->f) != segs[i]) { free(pk.s); return NULL; }
			if (!done && pk.len + segs[i] <= maxlen) str_addn(&pk, (char *)tmp, segs[i]);
			if (segs[i] < 255 && !done) {
				done = 1;
				/* rest of the page belongs to next packets: rewind to keep it simple */
				int rest = 0;
				for (int j = i + 1; j < ns; j++) rest += segs[j];
				if (rest) {
					/* we only need the first two packets in order; the comment packet
					 * normally starts on its own page, so skipping is acceptable */
					fseek(r->f, rest, SEEK_CUR);
				}
				break;
			}
		}
		if (done) {
			if (!pk.s) str_addn(&pk, "", 0);
			*plen = pk.len;
			return (uint8_t *)pk.s;
		}
	}
	free(pk.s);
	return NULL;
}

static int ogg_read(FILE *f, long start, long fsize, Meta *m, uint8_t **art, size_t *artlen) {
	OggR r = { f, 0, 0 };
	fseek(f, start, SEEK_SET);
	size_t n;
	uint8_t *p = ogg_packet(&r, &n, 1 << 16, NULL);
	if (!p) return 0;
	if (n < 30 || memcmp(p, "\x01vorbis", 7)) { free(p); return 0; } /* Opus etc. are not supported */
	uint32_t rate = le32(p + 12);
	free(p);
	p = ogg_packet(&r, &n, 32 << 20, NULL);
	if (p && n > 7 && !memcmp(p, "\x03vorbis", 7)) {
		int has_pic = 0;
		vorbis_comments(p + 7, n - 7, m, &has_pic);
		if (has_pic) m->art = ART_OGG;
		if (has_pic && art) {
			/* find METADATA_BLOCK_PICTURE / COVERART value */
			const uint8_t *d = p + 7;
			size_t dn = n - 7, q = 4 + le32(d);
			uint32_t cnt = q + 4 <= dn ? le32(d + q) : 0;
			q += 4;
			for (uint32_t i = 0; i < cnt && q + 4 <= dn; i++) {
				uint32_t len = le32(d + q); q += 4;
				if (len > dn - q) break;
				const char *c = (const char *)d + q;
				if (len > 23 && !strncasecmp(c, "METADATA_BLOCK_PICTURE=", 23)) {
					size_t bl; uint8_t *blk = b64_decode(c + 23, len - 23, &bl);
					const uint8_t *img; size_t il; int pt;
					if (flac_picture(blk, bl, &img, &il, &pt)) {
						*art = xmalloc(il); memcpy(*art, img, il); *artlen = il;
						free(blk);
						break;
					}
					free(blk);
				} else if (len > 9 && !strncasecmp(c, "COVERART=", 9)) {
					*art = b64_decode(c + 9, len - 9, artlen);
					break;
				}
				q += len;
			}
		}
	}
	free(p);
	/* duration: last page granule */
	if (rate && !art) {
		long tail = fsize > 65536 ? 65536 : fsize;
		uint8_t *b = xmalloc(tail);
		fseek(f, fsize - tail, SEEK_SET);
		size_t got = fread(b, 1, tail, f);
		for (long i = (long)got - 27; i >= 0; i--) {
			if (b[i] == 'O' && !memcmp(b + i, "OggS", 4) && le32(b + i + 14) == r.serial) {
				uint64_t g = (uint64_t)le32(b + i + 6) | (uint64_t)le32(b + i + 10) << 32;
				if (g != (uint64_t)-1) { m->duration_ms = (int)(g * 1000 / rate); break; }
			}
		}
		free(b);
	}
	return 1;
}

/* ---------- WAV ---------- */
static int wav_read(FILE *f, long start, Meta *m) {
	uint8_t h[12];
	fseek(f, start, SEEK_SET);
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
	uint32_t rate = 0, balign = 0;
	for (int guard = 0; guard < 64; guard++) {
		uint8_t c[8];
		if (fread(c, 1, 8, f) != 8) break;
		uint32_t len = le32(c + 4);
		if (!memcmp(c, "fmt ", 4) && len >= 16) {
			uint8_t fm[16];
			if (fread(fm, 1, 16, f) != 16) break;
			rate = le32(fm + 4);
			balign = fm[12] | fm[13] << 8;
			fseek(f, len - 16 + (len & 1), SEEK_CUR);
		} else if (!memcmp(c, "data", 4)) {
			if (rate && balign) m->duration_ms = (int)((uint64_t)len / balign * 1000 / rate);
			fseek(f, len + (len & 1), SEEK_CUR);
		} else if (!memcmp(c, "LIST", 4) && len >= 4 && len < (1 << 20)) {
			uint8_t *d = xmalloc(len);
			if (fread(d, 1, len, f) != len) { free(d); break; }
			if (!memcmp(d, "INFO", 4)) {
				for (uint32_t p = 4; p + 8 <= len;) {
					uint32_t sl = le32(d + p + 4);
					if (sl > len - p - 8) break;
					const char *k = NULL;
					if (!memcmp(d + p, "INAM", 4)) k = "title";
					else if (!memcmp(d + p, "IART", 4)) k = "artist";
					else if (!memcmp(d + p, "IPRD", 4)) k = "album";
					else if (!memcmp(d + p, "ITRK", 4)) k = "track";
					else if (!memcmp(d + p, "ICRD", 4)) k = "year";
					if (k) { char *v = text_8bit_to_utf8(d + p + 8, sl); str_trim(v); if (*v) apply_field(m, k, v); else free(v); }
					p += 8 + sl + (sl & 1);
				}
			}
			free(d);
			if (len & 1) fseek(f, 1, SEEK_CUR);
		} else {
			if (fseek(f, len + (len & 1), SEEK_CUR)) break;
		}
	}
	return rate != 0;
}

/* ---------- entry points ---------- */

static int sniff(FILE *f, long start) {
	uint8_t b[12] = {0};
	fseek(f, start, SEEK_SET);
	size_t n = fread(b, 1, 12, f);
	if (n >= 4 && !memcmp(b, "fLaC", 4)) return FMT_FLAC;
	if (n >= 4 && !memcmp(b, "OggS", 4)) return FMT_OGG;
	if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) return FMT_WAV;
	return FMT_UNKNOWN;
}

int tags_read(const char *path, Meta *m) {
	memset(m, 0, sizeof *m);
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	long fsize = file_size(f);
	long start = id3v2_read(f, m, NULL, NULL);
	int fmt = sniff(f, start);
	int ok = 0;
	if (fmt == FMT_FLAC) ok = flac_read(f, start, m, NULL, NULL);
	else if (fmt == FMT_OGG) ok = ogg_read(f, start, fsize, m, NULL, NULL);
	else if (fmt == FMT_WAV) ok = wav_read(f, start, m);
	else {
		long end = fsize;
		uint8_t t[3];
		if (fsize > 128) { fseek(f, fsize - 128, SEEK_SET); if (fread(t, 1, 3, f) == 3 && !memcmp(t, "TAG", 3)) end -= 128; }
		m->duration_ms = mp3_duration(f, start, end);
		ok = m->duration_ms > 0 || str_iends_ascii(path, ".mp3");
		if (ok) { fmt = FMT_MP3; id3v1_read(f, m, fsize); }
	}
	m->format = fmt;
	fclose(f);
	if (!ok) meta_free(m);
	return ok;
}

void meta_free(Meta *m) {
	free(m->title); free(m->artist); free(m->album); free(m->albumartist);
	memset(m, 0, sizeof *m);
}

uint8_t *tags_load_art(const char *path, int kind, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	Meta m;
	memset(&m, 0, sizeof m);
	uint8_t *res = NULL;
	*len = 0;
	if (kind == ART_ID3) {
		uint8_t *tag = NULL;
		Pic pic = {0};
		id3v2_read(f, &m, &tag, &pic);
		if (pic.data && pic.len) { res = xmalloc(pic.len); memcpy(res, pic.data, pic.len); *len = pic.len; }
		free(tag);
	} else {
		long start = 0;
		uint8_t h[10];
		if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) start = 10 + syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);
		if (kind == ART_FLAC) flac_read(f, start, &m, &res, len);
		else if (kind == ART_OGG) ogg_read(f, start, file_size(f), &m, &res, len);
	}
	meta_free(&m);
	fclose(f);
	return res;
}
