#include "encode.h"
#include "jbig.h"
#include "halftone.h"
#include "acmm.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int sink_write(sink_t *s, const void *p, size_t n)
{
    if (n == 0) return 0;
    if (fwrite(p, 1, n, s->out) != n) return -1;
    s->bytes += n;
    return 0;
}

static int wr(sink_t *s, const char *t) { return sink_write(s, t, strlen(t)); }

static int pjl(sink_t *s, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return sink_write(s, buf, (size_t)n);
}

/*
 * The device stream, derived from the capture corpus (see docs/DEVICE-STREAM.md).
 *
 *   job   : UEL "@PJL\r\n"  COMPRESS COVER HOLD SECTION COPIES
 *   page x n: PAGESTATUS=START DUPLEX[ BINDING] PAPER PAPERWIDTH PAPERLENGTH
 *             MEDIASOURCE MEDIATYPE RESOLUTION
 *             then, per <=32768-byte chunk: IMAGELEN=<n>\r\n <n bytes>
 *             PAGESTATUS=END
 *   job   : "@PJL EOJ\r\n" UEL
 *
 * The per-page image is one JBIG BIE (see jbig.c).  Its size (xd x yd) and the
 * PAPERWIDTH/PAPERLENGTH lines are a per-paper value that the vendor derives from the
 * raster header's PageSize, expressed in points, at the device's fixed 600 dpi.  The
 * table below reproduces the captures exactly and is selected by those points.
 *
 * The key is the points, not the PageSize= option and not the header's page-size *name*:
 * a real pdftoraster raster calls an A4 page "A4.Borderless" (a name absent from the PPD)
 * yet the vendor still emits the A4 geometry, while rewriting the header's points to
 * Letter -- leaving the name alone -- makes it emit the Letter geometry.  Verified
 * against the running filter (2026-10-16).
 */

struct paper { const char *psn; const char *code; unsigned px, py, pw, pl, xd, yd; };

static const struct paper papers[] = {
  { "16K", "MLTM16K", 553, 765, 4608, 6375, 4608, 6375 },
  { "8K", "MLTM8K", 757, 1100, 6308, 9166, 6307, 9166 },
  { "A3", "A3", 842, 1191, 7016, 9925, 7016, 9924 },
  { "A4", "A4", 595, 842, 4958, 7016, 4957, 7016 },
  { "A5", "A5", 420, 595, 3500, 4958, 3499, 4957 },
  { "A6", "A6", 297, 420, 2475, 3500, 2475, 3499 },
  { "B4", "JISB4", 729, 1032, 6075, 8600, 6075, 8599 },
  { "B5", "JISB5", 516, 729, 4300, 6075, 4299, 6075 },
  { "B6", "JISB6", 363, 516, 3025, 4300, 3024, 4299 },
  { "Comm10", "COM10", 297, 684, 2475, 5700, 2475, 5700 },
  { "EnvC6", "C63OF4", 323, 459, 2692, 3825, 2691, 3825 },
  { "EnvDL", "ENVDL", 312, 624, 2600, 5200, 2599, 5199 },
  { "EnvPersonal", "C6ENV", 261, 468, 2175, 3900, 2175, 3900 },
  { "Executive", "EXECUTIVE", 522, 756, 4350, 6300, 4350, 6300 },
  { "FabFoldGermanLegal", "FLS3", 612, 935, 5100, 7792, 5100, 7791 },
  { "FLS_220X330", "FLS4", 624, 935, 5200, 7792, 5199, 7791 },
  { "FLS_8D125X13D25", "FLS5", 584, 955, 4867, 7958, 4866, 7957 },
  { "FLS_8D25X13", "FLS2", 595, 935, 4958, 7792, 4957, 7791 },
  { "FLS_8D5X13D5", "FLS6", 612, 972, 5100, 8100, 5100, 8100 },
  { "FLS_8X13", "FLS1", 575, 935, 4792, 7792, 4791, 7791 },
  { "Legal", "LEGAL", 612, 1008, 5100, 8400, 5100, 8400 },
  { "Letter", "LETTER", 612, 792, 5100, 6600, 5100, 6600 },
  { "Statement", "INVOICE", 396, 612, 3300, 5100, 3300, 5100 },
  { "Tabloid", "LEDGER", 792, 1224, 6600, 10200, 6600, 10200 },
};

static const char *hft_path(void)
{
    const char *p = getenv("KONICA_HFT");
    if (p && *p) return p;
    return "/usr/local/lib/konica/KonicaMinolta/245igdi/Halftones/"
           "COPA.DEFAULT.600x600.hft";
}

/* The vendor CMM does not read its 33^3 table from disk: it regenerates it
 * from the installed ICC profiles.  We do the same (see lutgen.h), keeping a
 * captured dump (KONICA_LUT) as an optional override/cross-check. */
#define PROF_DIR "/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/"

static const char *env_or(const char *env, const char *def)
{
    const char *p = getenv(env);
    return (p && *p) ? p : def;
}

static const char *srgb_path(void) { return env_or("KONICA_SRGB", PROF_DIR "sRGB.icm"); }
static const char *gray_path(void) { return env_or("KONICA_GRAY", PROF_DIR "sGrayInv.icm"); }

static acmm_t *acmm_open(void)
{
    const char *dump = env_or("KONICA_LUT", NULL);
    if (dump) return acmm_load(dump);
    return acmm_generate(srgb_path(), gray_path());
}

struct enc_res { halftone_t *ht; acmm_t *acmm; };

/* The historical flat-tone approximation is accepted only when explicitly asked for;
 * by default a colour table that cannot be built fails the job. */
static int flat_tone_allowed(void)
{
    const char *p = getenv("KONICA_FLAT_TONE");
    return p && *p && strcmp(p, "0");
}

enc_res_t *enc_open(void)
{
    enc_res_t *res = calloc(1, sizeof *res);
    if (!res) return NULL;
    res->ht = halftone_load(hft_path());
    if (!res->ht) {
        fprintf(stderr, "ERROR: konica-native: cannot load halftone screen %s\n", hft_path());
        free(res);
        return NULL;
    }
    res->acmm = acmm_open();
    if (!res->acmm && !flat_tone_allowed()) {
        fprintf(stderr, "ERROR: konica-native: cannot build the colour table from the ICC "
                        "profiles; set KONICA_SRGB/KONICA_GRAY (or KONICA_LUT), or set "
                        "KONICA_FLAT_TONE=1 to accept the flat-tone approximation\n");
        halftone_free(res->ht);
        free(res);
        return NULL;
    }
    return res;
}

void enc_close(enc_res_t *res)
{
    if (!res) return;
    acmm_free(res->acmm);
    halftone_free(res->ht);
    free(res);
}

int enc_job_begin(sink_t *s, const job_t *j)
{
    (void)j;
    if (wr(s, "\x1b%-12345X@PJL\r\n")) return -1;
    if (wr(s, "@PJL SET COMPRESS=JBIG\r\n")) return -1;
    if (wr(s, "@PJL SET COVER=OFF\r\n")) return -1;
    if (wr(s, "@PJL SET HOLD=OFF\r\n")) return -1;
    if (wr(s, "@PJL SET SECTION=OFF\r\n")) return -1;
    if (wr(s, "@PJL SET COPIES=1\r\n")) return -1;
    return 0;
}

int enc_job_end(sink_t *s, const job_t *j)
{
    (void)j;
    if (wr(s, "@PJL EOJ\r\n")) return -1;
    if (wr(s, "\x1b%-12345X\r\n")) return -1;
    return 0;
}

/* Map one source row (h->bytes_per_line bytes) to 8-bit gray in `gray`. */
static int to_gray(const ras_hdr_t *h, const uint8_t *line, uint8_t *gray)
{
    uint32_t x;
    if (h->bits_per_pixel == 8) {
        memcpy(gray, line, h->width);
        return 0;
    }
    if (h->bits_per_pixel == 1) {
        for (x = 0; x < h->width; x++)
            gray[x] = (line[x >> 3] >> (7 - (x & 7))) & 1 ? 0x00 : 0xff;
        return 0;
    }
    return -1;
}

/* Read the next raster row into `dst` (stride S, zero-filled past the raster
 * width).  A missing row is padded white, matching the short-page rule.
 * Returns 0, or -1 for an unsupported raster format. */
static int row_fill(ras_t *r, const ras_hdr_t *h, uint8_t *line, uint8_t *dst,
                    size_t S)
{
    if (ras_read_line(r, line) == 1) {
        if (to_gray(h, line, dst)) return -1;
    } else {
        memset(dst, 0, h->width);                  /* past the image: zeros */
    }
    memset(dst + h->width, 0, S - h->width);
    return 0;
}

int enc_page(sink_t *s, const job_t *j, enc_res_t *res, const ras_hdr_t *h, ras_t *r)
{
    const struct paper *pp = NULL;
    unsigned i;
    uint32_t xd, yd;

    /* Select by page-size points (what the vendor uses); fall back to the header's
     * page-size name, then to the raster dimensions for an unknown size. */
    for (i = 0; i < sizeof papers / sizeof papers[0]; i++)
        if (papers[i].px == h->page_size[0] && papers[i].py == h->page_size[1]) { pp = &papers[i]; break; }
    if (!pp)
        for (i = 0; i < sizeof papers / sizeof papers[0]; i++)
            if (!strcmp(papers[i].psn, h->page_size_name)) { pp = &papers[i]; break; }

    unsigned pw = pp ? pp->pw : h->width;
    unsigned pl = pp ? pp->pl : (h->height ? h->height - 1 : 0);
    xd = pp ? pp->xd : (h->width ? h->width - 1 : 0);
    yd = pp ? pp->yd : (h->height ? h->height - 1 : 0);

    /* Build the 1-bit page image. Rows beyond the raster are left white. */
    size_t stride = (xd + 7) / 8;
    uint8_t *img = calloc(1, stride * (yd ? yd : 1));
    uint8_t *line = malloc(h->bytes_per_line ? h->bytes_per_line : 1);
    /* The vendor keeps the source as one contiguous image whose row stride is
     * the raster row rounded up to 4 bytes, so the 3-tap CMM window at the
     * right edge reads the first bytes of the following row rather than a zero
     * pad (verified against the running filter).  The CMM itself runs over
     * buffers of at most 2^24 bytes, i.e. bands of floor(2^24 / stride) rows;
     * at a band's last row the window reads past the band into the adjacent
     * allocation, which the evidence shows to be that band's *output* buffer,
     * i.e. the raw Phi of the band's first row.  Model both with a two-row
     * window (see docs/REVERSE-ENGINEERING.md). */
    size_t S = ((size_t)h->width + 3) & ~(size_t)3;
    if (S < xd) S = xd;
    unsigned band_h = (unsigned)(16777216u / S);
    if (band_h == 0) band_h = 1;
    uint8_t *ra = calloc(1, S), *rb = calloc(1, S);
    int *tone = malloc((xd ? xd : 1) * sizeof *tone);
    uint8_t (*r2)[2] = calloc(h->height ? h->height : 1, 2);
    if (!img || !line || !ra || !rb || !tone || !r2) {
        free(img); free(line); free(ra); free(rb); free(tone); free(r2); return -1;
    }

    if (row_fill(r, h, line, ra, S)) {
        fprintf(stderr, "ERROR: konica-native: unsupported raster %ubpp cs=%u\n",
                h->bits_per_pixel, h->color_space);
        free(img); free(line); free(ra); free(rb); free(tone); free(r2);
        return -1;
    }
    r2[0][0] = ra[0]; r2[0][1] = ra[1];
    unsigned fb0 = 0, fb1 = 0;                 /* raw Phi, band's first row */
    for (uint32_t y = 0; y < yd; y++) {
        row_fill(r, h, line, rb, S);               /* row y + 1 (white past end) */
        if (y + 1 < h->height) { r2[y + 1][0] = rb[0]; r2[y + 1][1] = rb[1]; }
        unsigned bstart = (unsigned)(y / band_h) * band_h;
        unsigned blen = yd - bstart; if (blen > band_h) blen = band_h;
        int band_first = (y == bstart);
        int band_last  = (y == bstart + blen - 1);
        unsigned wb[2] = { 0, 0 };                 /* bytes S and S+1 of the row */
        if (band_last) {
            if (blen == band_h) { wb[0] = fb0; wb[1] = fb1; }
            else if (bstart >= band_h) {           /* leftover of the prior band */
                uint32_t lr = bstart - band_h + blen;
                if (lr < h->height) { wb[0] = r2[lr][0]; wb[1] = r2[lr][1]; }
            }
        }
        for (uint32_t x = 0; x < xd; x++) {
            int v0 = ra[x], v1, v2;
            if (x + 1 < S) v1 = ra[x + 1];
            else v1 = band_last ? (int)wb[x + 1 - S] : rb[x + 1 - S];
            if (x + 2 < S) v2 = ra[x + 2];
            else v2 = band_last ? (int)wb[x + 2 - S] : rb[x + 2 - S];
            if (res->acmm) {
                int raw = acmm_phi(res->acmm, v0, v1, v2);
                if (band_first && x < 2) { if (x == 0) fb0 = (unsigned)raw; else fb1 = (unsigned)raw; }
                /* OCM_TonerSave TRUE (and absent) subtracts 10 from the CMM
                 * output before halftoning; FALSE passes it through unchanged
                 * (verified against the running filter, see docs/REVERSE-ENGINEERING.md). */
                int t = raw - (j->toner_save ? 10 : 0);
                tone[x] = t > 0 ? t : 0;
            } else {
                tone[x] = (j->toner_save ? 245 : 255) - ra[x];  /* flat-tone approximation */
                if (band_first && x < 2) { if (x == 0) fb0 = (unsigned)tone[x]; else fb1 = (unsigned)tone[x]; }
            }
        }
        halftone_row(res->ht, tone, (int)xd, (int)y, img + (size_t)y * stride);
        uint8_t *tmp = ra; ra = rb; rb = tmp;
    }
    /* libcups writes `height` rows per page while we encode only `yd` of them
     * (yd == height - 1); drain the rest so the next page header stays aligned. */
    while (ras_read_line(r, line) == 1)
        ;
    free(line); free(ra); free(rb); free(tone); free(r2);

    uint8_t *bie = NULL; size_t blen = 0;
    if (jbig_encode_bie(xd, yd, img, stride, 128, 0x48, &bie, &blen)) {
        free(img); return -1;
    }
    free(img);

    /* ---- page header ---- */
    if (pjl(s, "@PJL SET PAGESTATUS=START\r\n")) goto err;
    if (j->duplex == 0) {
        if (wr(s, "@PJL SET DUPLEX=OFF\r\n")) goto err;
    } else {
        if (wr(s, "@PJL SET DUPLEX=ON\r\n")) goto err;
        if (wr(s, j->duplex == 1 ? "@PJL SET BINDING=SHORTEDGE\r\n"
                                 : "@PJL SET BINDING=LONGEDGE\r\n")) goto err;
    }
    if (pjl(s, "@PJL SET PAPER=%s\r\n", pp ? pp->code : "A4")) goto err;
    if (pjl(s, "@PJL SET PAPERWIDTH=%u\r\n", pw)) goto err;
    if (pjl(s, "@PJL SET PAPERLENGTH=%u\r\n", pl)) goto err;
    if (wr(s, "@PJL SET MEDIASOURCE=AUTO\r\n")) goto err;
    if (wr(s, "@PJL SET MEDIATYPE=PLAIN\r\n")) goto err;
    /* The vendor reports the *device* resolution, not the raster's: a raster whose
     * HWResolution was rewritten to 300 or 1200 dpi still yields an identical stream
     * with RESOLUTION=600 (verified against the running filter, 2026-10-16). */
    if (h->hw_res[0] && h->hw_res[0] != 600)
        fprintf(stderr, "WARNING: konica-native: raster is %udpi; the device is 600dpi and the "
                        "vendor forces RESOLUTION=600\n", h->hw_res[0]);
    if (pjl(s, "@PJL SET RESOLUTION=600\r\n")) goto err;

    /* ---- image, chunked at 32768 bytes, each with its own IMAGELEN ---- */
    for (size_t off = 0; off < blen; off += 32768) {
        size_t n = blen - off; if (n > 32768) n = 32768;
        if (pjl(s, "@PJL SET IMAGELEN=%zu\r\n", n)) goto err;
        if (sink_write(s, bie + off, n)) goto err;
    }

    if (wr(s, "@PJL SET PAGESTATUS=END\r\n")) goto err;
    free(bie);
    return 0;
err:
    free(bie);
    return -1;
}
