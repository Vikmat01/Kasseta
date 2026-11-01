#ifndef COVERS_H
#define COVERS_H
#include "gfx.h"
#include "library.h"

void covers_init(void);
void covers_set_library(Library *l);  /* call after swapping libraries (drops cache) */
/* returns cached square image of given size, or NULL (then a load is queued).
 * *missing is set to 1 if the album has no usable cover. */
Image *covers_get(int album, int size, int *missing);
int covers_poll(void);                /* returns 1 if new images became available */
void covers_shutdown(void);
#endif
