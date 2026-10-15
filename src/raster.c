#include "raster.h"
#include <stdlib.h>
#include <string.h>

struct ras {
    FILE *in;
    int swap;            /* stream byte order differs from host */
    int version;         /* 1, 2 or 3 */
    ras_hdr_t hdr;
    uint32_t rows_left;
    uint8_t *prev;       /* last decoded line (RLE repeat) */
    uint32_t repeat;     /* remaining repeats of prev */
    int have_page;
};

static uint32_t rd32(const ras_t *r, const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return r->swap ? __builtin_bswap32(v) : v;
}

static int white_is_ff(uint32_t cs)
{
    /* K-like spaces: 0 = no ink. libcups ("clear to end of line") fills these with 0x00. */
    switch (cs) {
    case 3: /* K */ case 4: case 5: /* CMY, YMC */ case 6: case 7: case 8: /* CMYK, YMCK, KCMY */
    case 9: case 10: case 11: 
        return 0;
    default:
        return 1;
    }
}

ras_t *ras_open(FILE *in)
{
    uint8_t s[4];
    if (fread(s, 1, 4, in) != 4) return NULL;
    uint32_t w; memcpy(&w, s, 4);
    ras_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->in = in;
    switch (w) {
    case 0x52615374u: r->version = 1; break;                   /* RaSt */
    case 0x74536152u: r->version = 1; r->swap = 1; break;
    case 0x52615332u: r->version = 2; break;                   /* RaS2 */
    case 0x32536152u: r->version = 2; r->swap = 1; break;
    case 0x52615333u: r->version = 3; break;                   /* RaS3 */
    case 0x33536152u: r->version = 3; r->swap = 1; break;
    default: free(r); return NULL;
    }
    return r;
}

/* Page framing: libcups writes the sync word for the FIRST page only (see raster.h).
 * This function therefore reads a bare header; ras_open() consumed the only sync word.
 * Do not "fix" this by reading a sync word here. */
/* Page framing: libcups writes the 4-byte sync word for the FIRST page only (ras_open reads
 * it); pages 2..n follow immediately as header+pixels with no sync. Verified by byte count:
 * 3 pages of 1403 px at v3 = 4 + 3*(1796 + 1403) = 9601. Do not add a per-page sync read
 * here -- it would silently corrupt multi-page jobs while 1-page tests keep passing. */
int ras_next_page(ras_t *r, ras_hdr_t *h)
{
    size_t want = r->version == 1 ? RAS_HDR_V1_SIZE : RAS_HDR_V2_SIZE;
    memset(&r->hdr, 0, sizeof r->hdr);
    size_t got = fread(r->hdr.raw, 1, want, r->in);
    if (got == 0) return 0;
    if (got != want) return -1;
    const uint8_t *b = r->hdr.raw;
    ras_hdr_t *x = &r->hdr;
    memcpy(x->media_type, b + 128, 64);
    x->duplex = rd32(r, b + 272);
    x->hw_res[0] = rd32(r, b + 276);   x->hw_res[1] = rd32(r, b + 280);
    x->num_copies = rd32(r, b + 340);  x->orientation = rd32(r, b + 344);
    x->page_size[0] = rd32(r, b + 352); x->page_size[1] = rd32(r, b + 356);
    x->tumble = rd32(r, b + 368);
    x->width = rd32(r, b + 372);       x->height = rd32(r, b + 376);
    x->bits_per_color = rd32(r, b + 384);
    x->bits_per_pixel = rd32(r, b + 388);
    x->bytes_per_line = rd32(r, b + 392);
    x->color_order = rd32(r, b + 396);
    x->color_space = rd32(r, b + 400);
    x->compression = rd32(r, b + 404);
    if (r->version != 1) {
        x->num_colors = rd32(r, b + 420);
        memcpy(x->page_size_name, b + 1732, 64);
    }
    x->media_type[63] = x->page_size_name[63] = 0;
    if (x->bytes_per_line == 0 || x->bytes_per_line > (1u << 24) || x->height > (1u << 20)) return -1;
    free(r->prev);
    r->prev = malloc(x->bytes_per_line);
    if (!r->prev) return -1;
    r->rows_left = x->height;
    r->repeat = 0;
    r->have_page = 1;
    *h = *x;
    return 1;
}

static int getb(ras_t *r) { return fgetc(r->in); }

int ras_read_line(ras_t *r, uint8_t *line)
{
    if (!r->have_page || r->rows_left == 0) return 0;
    uint32_t bpl = r->hdr.bytes_per_line;

    if (r->version != 2) {                       /* uncompressed */
        if (fread(line, 1, bpl, r->in) != bpl) return -1;
        r->rows_left--;
        return 1;
    }

    if (r->repeat) {                             /* RLE: reuse the previous line */
        memcpy(line, r->prev, bpl);
        r->repeat--; r->rows_left--;
        return 1;
    }
    int rep = getb(r);                           /* line repeat count - 1 */
    if (rep < 0) return -1;
    uint32_t bpp = (r->hdr.bits_per_pixel + 7) / 8;
    if (bpp == 0) bpp = 1;
    uint32_t pos = 0;
    while (pos < bpl) {
        int b = getb(r);
        if (b < 0) return -1;
        if (b == 128) {                          /* clear to end of line */
            memset(r->prev + pos, white_is_ff(r->hdr.color_space) ? 0xFF : 0x00, bpl - pos);
            pos = bpl;
        } else if (b & 128) {                    /* literal run of (257 - b) pixels */
            size_t n = (size_t)(257 - b) * bpp;
            if (n > bpl - pos) n = bpl - pos;    /* never write past the line */
            if (fread(r->prev + pos, 1, n, r->in) != n) return -1;
            pos += (uint32_t)n;
        } else {                                 /* (b + 1) copies of one pixel */
            uint8_t px[16];
            if (bpp > sizeof px || fread(px, 1, bpp, r->in) != bpp) return -1;
            for (int i = 0; i <= b && pos < bpl; i++)
                for (uint32_t k = 0; k < bpp && pos < bpl; k++) r->prev[pos++] = px[k];
        }
    }
    memcpy(line, r->prev, bpl);
    r->repeat = (uint32_t)rep;
    if (r->repeat >= r->rows_left) r->repeat = r->rows_left - 1;
    r->rows_left--;
    return 1;
}

void ras_close(ras_t *r)
{
    if (!r) return;
    free(r->prev);
    free(r);
}
