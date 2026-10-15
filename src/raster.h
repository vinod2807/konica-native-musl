#ifndef RASTER_H
#define RASTER_H
/*
 * Minimal CUPS raster reader — no libcups, no libppd, pure libc. Builds on musl.
 *
 * Formats (sync word, host byte order; the byte-reversed word means swap fields):
 *   "RaSt"  v1  420-byte header, uncompressed
 *   "RaS2"  v2  1796-byte header, RLE-compressed pixels
 *   "RaS3"  v3  1796-byte header, uncompressed pixels
 * PAGE FRAMING: the 4-byte sync word appears ONCE, at the start of the stream. Pages 2..n
 * follow directly as header + pixels, with no sync word. Measured: 2 pages of 64x32 8-bit
 * v3 = 4 + 2*(1796 + 2048) = 7692 bytes; 3 pages of 1403 px bytes at v3 = 9601 bytes.
 * Do not add a per-page sync read: it would silently corrupt every multi-page job while
 * single-page tests keep passing. Verified against libcups 2.4 with tests/libcups_dump.c
 * (see tests/check-reader.sh).
 */
#include <stdint.h>
#include <stdio.h>

#define RAS_HDR_V2_SIZE 1796
#define RAS_HDR_V1_SIZE 420

typedef struct {
    uint32_t duplex, tumble, num_copies, orientation;
    uint32_t hw_res[2], page_size[2];            /* dpi; points */
    uint32_t width, height;
    uint32_t bits_per_color, bits_per_pixel, bytes_per_line;
    uint32_t color_order, color_space, compression, num_colors;
    char media_type[64];
    char page_size_name[64];                     /* v2/v3 headers only */
    uint8_t raw[RAS_HDR_V2_SIZE];                /* unmodified header, for byte-exact replay */
} ras_hdr_t;

typedef struct ras ras_t;

ras_t *ras_open(FILE *in);                       /* reads the sync word; NULL on error */
int    ras_next_page(ras_t *r, ras_hdr_t *h);    /* 1 = page read, 0 = end of stream, -1 = error */
int    ras_read_line(ras_t *r, uint8_t *line);   /* 1 = ok (line is h->bytes_per_line), 0 = end of page, -1 = error */
void   ras_close(ras_t *r);

#endif
