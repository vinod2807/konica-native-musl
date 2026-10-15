#include "halftone.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct halftone {
    int w, h;
    uint8_t *scr;      /* w*h threshold bytes */
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

halftone_t *halftone_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hdr[0x4c];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return NULL; }
    if (memcmp(hdr, "tHsG", 4) != 0) { fclose(f); return NULL; }
    /* First plane record at 0x0c: {u32 offset, u32 size}; the plane's own
     * sub-header at `offset` is {u16 width, u16 height, u16, u16}. */
    uint32_t off = rd32(hdr + 0x0c);
    if (off < 0x10 || off > 0x1000) { fclose(f); return NULL; }
    uint8_t sub[8];
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return NULL; }
    if (fread(sub, 1, 8, f) != 8) { fclose(f); return NULL; }
    int w = sub[0] | (sub[1] << 8);
    int h = sub[2] | (sub[3] << 8);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { fclose(f); return NULL; }
    halftone_t *ht = calloc(1, sizeof *ht);
    if (!ht) { fclose(f); return NULL; }
    ht->w = w; ht->h = h;
    ht->scr = malloc((size_t)w * (size_t)h);
    if (!ht->scr || fread(ht->scr, 1, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
        free(ht->scr); free(ht); fclose(f); return NULL;
    }
    fclose(f);
    return ht;
}

void halftone_free(halftone_t *ht) { if (ht) { free(ht->scr); free(ht); } }
int  halftone_width(const halftone_t *ht) { return ht->w; }
int  halftone_height(const halftone_t *ht) { return ht->h; }

void halftone_row(const halftone_t *ht, const int *tone, int width, int y,
                  uint8_t *out)
{
    const uint8_t *srow = ht->scr + (size_t)(y % ht->h) * (size_t)ht->w;
    int nbytes = (width + 7) / 8, x, bit = 0;
    uint8_t acc = 0;
    for (x = 0; x < nbytes; x++) out[x] = 0;
    for (x = 0; x < width; x++) {
        if (tone[x] >= srow[x % ht->w]) acc |= (uint8_t)(0x80 >> bit);
        if (++bit == 8) { out[x >> 3] = acc; acc = 0; bit = 0; }
    }
    if (bit) out[width >> 3] = acc;
}
