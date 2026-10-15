#ifndef HALFTONE_H
#define HALFTONE_H
/*
 * The device halftone screen (the "COPA" .hft that ships in the printer package).
 *
 * The .hft is a small header plus a table of planes; plane 0 is the threshold
 * screen the 600x600 path uses (96x96 bytes, values 1..255).  We load plane 0
 * from the installed file rather than embedding it: the screen is vendor data
 * and is already present wherever this filter actually runs.
 *
 * Per-pixel rule recovered from the vendor filter (HalftoneImage1c1b):
 *     dot = ( tone[x] >= screen[y % H][x % W] )
 * where `tone` is the source row after the CMM neighbourhood stage (see
 * acmm.h): tone[x] = max(0, Phi(src[x],src[x+1],src[x+2]) - 10).  For uniform
 * input Phi(g,g,g) = 255 - g, so tone[x] = 245 - g, which is the flat-tone
 * approximation the kit uses when no LUT dump is supplied.  See
 * docs/REVERSE-ENGINEERING.md.
 */
#include <stdint.h>

typedef struct halftone halftone_t;

/* NULL if the file is missing or not a usable .hft. */
halftone_t *halftone_load(const char *path);
void        halftone_free(halftone_t *ht);
int         halftone_width(const halftone_t *ht);
int         halftone_height(const halftone_t *ht);

/* Convert one row of `width` tone values (0..255; negative = never a dot) into
 * 1-bit output, MSB first, ceil(width/8) bytes, setting a dot where
 * tone[x] >= screen[y % H][x % W]. */
void halftone_row(const halftone_t *ht, const int *tone, int width, int y,
                  uint8_t *out);

#endif
