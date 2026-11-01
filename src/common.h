#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "Кассета"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

uint64_t now_ms(void);
void sleep_ms(int ms);

/* UTF-8 helpers */
uint32_t utf8_next(const char **s);           /* returns 0 at end */
int utf8_put(char *out, uint32_t cp);          /* returns bytes written (max 4) */
uint32_t cp_fold(uint32_t cp);                 /* lowercase Latin/Cyrillic, ё->е */
char *str_fold(const char *s);                 /* casefolded, malloc'd */
int str_icmp_utf8(const char *a, const char *b);
int str_ieq_ascii(const char *a, const char *b);
int str_iends_ascii(const char *s, const char *suffix);
void str_trim(char *s);
uint32_t hash_str(const char *s);

/* read whole file / part of file */
uint8_t *read_file(const char *path, size_t *len);
uint8_t *read_file_range(const char *path, long off, size_t len);
long file_size(FILE *f);

const char *path_basename(const char *p);
char *path_dirname(const char *p); /* malloc'd */

/* simple growing string builder */
typedef struct { char *s; size_t len, cap; } Str;
void str_add(Str *b, const char *s);
void str_addn(Str *b, const char *s, size_t n);
void str_addc(Str *b, char c);

#endif
