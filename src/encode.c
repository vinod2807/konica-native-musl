#include "encode.h"
#include "jbig.h"
#include "halftone.h"
#include "acmm.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
 * Letter -- leaving the name alone -- makes it emit the Letter geometry.  A swapped pair
 * selects the same entry, and a size that is not in the table at all becomes a CUSTOM
 * page (see dim_from_points/custom_geometry below).  Verified against the running filter
 * (2026-10-16).
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

/* PJL MEDIATYPE token for the MediaType *string in the raster header* (CUPS puts the job's
 * media type there).  The vendor ignores the MediaType= job option entirely: with a raster
 * saying "Plain" and the option saying "Transparency" it still emits PLAIN (verified against
 * the running filter).  The twelve spellings below are the PPD's MediaType choices; anything
 * else -- empty, "Auto", an unknown name -- falls back to PLAIN.  Matching is exact apart
 * from case: "Transparency" and "transparency" map, "Transparencies" and "transparency film"
 * do not. */
static const char *media_type_code(const char *name)
{
    static const struct { const char *name, *code; } map[] = {
        { "Plain", "PLAIN" },           { "Recycle", "RECYCLE" },
        { "1Side", "ONESIDE" },         { "Special", "SPECIAL" },
        { "Transparency", "TRANSPARENCY" }, { "Card1", "CARDONE" },
        { "Card2", "CARDTWO" },         { "Envelope", "ENVELOPE" },
        { "Plains", "PLAINS" },         { "Card1s", "CARDONES" },
        { "Card2s", "CARDTWOS" },       { "Recycle2side", "RECYCLES" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcasecmp(map[i].name, name) == 0) return map[i].code;
    return "PLAIN";
}

/* Read the next raster row into `dst` (stride S, zero-filled past the raster width).
 * Returns 0 when a row was read, 1 when the raster has no more rows, -1 for an
 * unsupported raster format.
 *
 * The caller must NOT halftone a row that was not read: measured on the running filter,
 * a page whose raster is shorter than the paper's image height (Yd) comes out with the
 * *rest of the image* left dot-free, not halftoned from blank paper.  A6 (Yd 3499) with
 * an all-ink raster, height H: H=1 -> 570 B, H=1000 -> 10588 B, H=3499 -> 35500 B, i.e.
 * the size tracks H only; the vendor's BIH still says Yd=3499 in every case, and at H=3498
 * its stream differs from H=3499 only in the final row.  See docs/DEVICE-STREAM.md. */
static int row_fill(ras_t *r, const ras_hdr_t *h, uint8_t *line, uint8_t *dst,
                    size_t S)
{
    if (ras_read_line(r, line) != 1) return 1;
    if (to_gray(h, line, dst)) return -1;
    memset(dst + h->width, 0, S - h->width);
    return 0;
}

/* The vendor converts a page dimension given in points to device pixels at its fixed
 * 600 dpi.  The two numbers it publishes for such a dimension -- PAPERWIDTH/PAPERLENGTH
 * and the BIE's Xd/Yd -- are not simply floor(points * 600 / 72): the running filter
 * publishes one pixel more when the point count is 8 mod 9 and encodes one pixel less
 * when it is 1, 3 or 6 mod 9.  This reproduces every number in papers[] below and a
 * 69-point sweep of custom sizes (see docs/REVERSE-ENGINEERING.md). */
static void dim_from_points(unsigned pt, unsigned *pub, unsigned *enc)
{
    unsigned base = (unsigned)((uint64_t)pt * 25 / 3);   /* floor(pt * 600 / 72) */
    unsigned r = pt % 9;
    *pub = base + (r == 8);
    *enc = base - (r == 1 || r == 3 || r == 6);
}

/* Resolve the page geometry from the raster header's page size in points.  Returns 0 and
 * fills the outputs, or -1 when the vendor would refuse the page (it exits 2 having
 * written nothing) because the size is outside its supported range. */
static int page_geometry(const ras_hdr_t *h, const char **code, unsigned *pw, unsigned *pl,
                         uint32_t *xd, uint32_t *yd)
{
    const struct paper *pp = NULL;
    unsigned i;

    /* The vendor keys the geometry on the page size in points -- not on the PageSize=
     * option, not on the header's page-size name and not on the raster's pixel
     * dimensions.  A swapped pair selects the same entry (points 842x595 pick A4 just as
     * 595x842 do), because the page may be described either way round. */
    for (i = 0; i < sizeof papers / sizeof papers[0]; i++)
        if (papers[i].px == h->page_size[0] && papers[i].py == h->page_size[1]) { pp = &papers[i]; break; }
    if (!pp)
        for (i = 0; i < sizeof papers / sizeof papers[0]; i++)
            if (papers[i].px == h->page_size[1] && papers[i].py == h->page_size[0]) { pp = &papers[i]; break; }
    if (pp) {
        *code = pp->code; *pw = pp->pw; *pl = pp->pl; *xd = pp->xd; *yd = pp->yd;
        return 0;
    }

    unsigned a = h->page_size[0], b = h->page_size[1];
    unsigned lo = a < b ? a : b, hi = a < b ? b : a;
    if (lo < 72 || lo > 864 || hi > 1224) {
        fprintf(stderr, "ERROR: konica-native: page size %ux%u points is outside the "
                        "supported range (shorter side 72..864, longer side <= 1224)\n", a, b);
        return -1;
    }
    dim_from_points(lo, pw, xd);
    dim_from_points(hi, pl, yd);
    *code = "CUSTOM";
    return 0;
}

/* True when the page's size resolves to a supported geometry (reports the reason on
 * stderr when it does not).  Lets main refuse the whole job before writing a single byte,
 * exactly as the vendor does, instead of emitting a truncated PJL header. */
int enc_page_size_ok(const ras_hdr_t *h)
{
    const char *code; unsigned pw, pl; uint32_t xd, yd;
    return page_geometry(h, &code, &pw, &pl, &xd, &yd) == 0;
}

/* The vendor's DUPLEX/BINDING pair is not just the Duplex= option.  It combines that
 * option with the raster header's own Duplex/Tumble fields and with sides=, and it names
 * the edge from the *page orientation*: a landscape page (points wider than tall) swaps
 * the two names.  Measured against the running filter over a 256-combination grid
 * (docs/REVERSE-ENGINEERING.md).  Two vendor quirks to keep in mind:
 *   * Duplex= is matched case-insensitively, sides= case-sensitively;
 *   * sides=two-sided-short-edge becomes LONGEDGE and sides=two-sided-long-edge becomes
 *     SHORTEDGE -- the opposite of CUPS' naming (a vendor bug we reproduce faithfully).
 * When neither option names a duplex mode the header's Tumble supplies the binding, and
 * the header's Duplex can force DUPLEX=ON on its own.
 *
 * The vendor resolves this once per job, from the *first* page's header, and prints the
 * same DUPLEX/BINDING pair on every page: a two-page job whose pages differ in orientation
 * (595x842 then 842x595) or in their header Duplex/Tumble fields still gets one pair,
 * derived from page 1 (verified against the running filter, 2026-10-16).  enc_page() calls
 * this only while j->dup_known is clear. */
static void duplex_binding(const job_t *j, const ras_hdr_t *h, int *on, int *longedge)
{
    int tumble;
    if (j->duplex_opt) {                       /* Duplex= present: it owns the binding */
        *on = (j->duplex_opt != 1) || (h->duplex != 0);
        tumble = j->duplex_opt == 3 ? 1 : j->duplex_opt == 2 ? 0 : (h->tumble != 0);
    } else {                                   /* else sides=, else the header */
        *on = (h->duplex != 0) || (j->sides >= 2);
        tumble = j->sides == 3 ? 1 : j->sides == 2 ? 0 : (h->tumble != 0);
    }
    *longedge = tumble ^ (h->page_size[0] > h->page_size[1]);
}

int enc_page(sink_t *s, job_t *j, enc_res_t *res, const ras_hdr_t *h, ras_t *r)
{
    const char *code;
    uint32_t xd, yd, pw, pl;

    if (page_geometry(h, &code, &pw, &pl, &xd, &yd)) return -1;

    /* Build the 1-bit page image.  Rows the raster does not provide stay dot-free
     * (see row_fill). */
    size_t stride = (xd + 7) / 8;
    uint8_t *img = calloc(1, stride * (yd ? yd : 1));
    uint8_t *line = malloc(h->bytes_per_line ? h->bytes_per_line : 1);
    if (!img || !line) { free(img); free(line); return -1; }

    if (h->bits_per_pixel == 1) {
        /* Bilevel rasters bypass the colour and halftone pipeline: the running filter copies
         * the bits straight into the page image -- bit set is ink -- cropping to Xd x Yd and
         * leaving everything the raster does not cover dot-free.  Measured on A6 (Xd 2475,
         * Yd 3499): an all-ink 1bpp raster yields exactly 2475*3499 ink pixels, an all-white
         * one yields none, a checker yields half, and a rectangle drawn in the raster lands on
         * the same pixels (docs/REVERSE-ENGINEERING.md).  The raster's colour space makes no
         * difference (cs=0 and cs=3 behave identically).  The corpus is all 8bpp, which is
         * how this went unnoticed until the fuzzer exercised 1bpp.
         *
         * Verified limit: the copy is exact while no row carries ink in the two bits at or
         * just past the image width (x == xd or xd+1, i.e. only possible when the raster is
         * wider than the image).  When one does, the vendor abandons the copy and emits an
         * unexplained deterministic halftone-like pattern; that is a vendor defect (see
         * docs/DEVICE-STREAM.md section 3), the fuzzer reports it as such, and this PPD
         * (cupsBitsPerColor 8) never produces 1bpp anyway. */
        for (uint32_t y = 0; y < yd; y++) {
            unsigned n;
            if (ras_read_line(r, line) != 1) break;
            n = h->width < xd ? h->width : xd;
            if (n) {
                unsigned nb = (n + 7) / 8;
                memcpy(img + (size_t)y * stride, line, nb);
                /* Bits past Xd inside the last byte are padding in the BIE: keep them clear. */
                if (n & 7) img[(size_t)y * stride + nb - 1] &= (uint8_t)(0xffu << (8 - (n & 7)));
            }
        }
        while (ras_read_line(r, line) == 1)
            ;
        free(line);
    } else {
        /* The vendor keeps the source as one contiguous image whose row stride is
         * the raster row rounded up to 4 bytes, so the 3-tap CMM window at the
         * right edge of the raster reads into the following row rather than a zero
         * pad (verified against the running filter).  The CMM itself runs over
         * buffers of at most 2^24 bytes, i.e. bands of floor(2^24 / stride) rows;
         * at a band's last row the window reads past the band into the adjacent
         * allocation, which the evidence shows to be that band's *output* buffer,
         * i.e. the raw Phi of the band's first row.  Model both with a two-row
         * window (see docs/REVERSE-ENGINEERING.md). */
        size_t S = ((size_t)h->width + 3) & ~(size_t)3;
        unsigned band_h = (unsigned)(16777216u / S);
        if (band_h == 0) band_h = 1;
        uint8_t *ra = calloc(1, S), *rb = calloc(1, S);
        int *tone = malloc((xd ? xd : 1) * sizeof *tone);
        uint8_t (*r2)[2] = calloc(h->height ? h->height : 1, 2);
        if (!ra || !rb || !tone || !r2) {
            free(img); free(line); free(ra); free(rb); free(tone); free(r2); return -1;
        }

        int st = row_fill(r, h, line, ra, S);
        if (st < 0) {
            fprintf(stderr, "ERROR: konica-native: unsupported raster %ubpp cs=%u\n",
                    h->bits_per_pixel, h->color_space);
            free(img); free(line); free(ra); free(rb); free(tone); free(r2);
            return -1;
        }
        int present = (st == 0);                   /* is the row we are about to halftone real? */
        /* Columns the raster does not provide stay dot-free too: the running filter halftones
         * only the rectangle the raster actually covers.  Measured on A6 (Xd 2475): with a
         * 2474-column raster the vendor's image has exactly one column fewer (3499 dots) than
         * ours had, and with a fully white 2400-column raster its page is entirely dot-free.
         * With width >= Xd -- every real job -- the limit never binds. */
        unsigned xout = h->width < xd ? h->width : xd;
        r2[0][0] = ra[0]; r2[0][1] = ra[1];
        unsigned fb0 = 0, fb1 = 0;                 /* raw Phi, band's first row */
        for (uint32_t y = 0; y < yd; y++) {
            st = row_fill(r, h, line, rb, S);          /* row y + 1 */
            if (st < 0) {
                fprintf(stderr, "ERROR: konica-native: unsupported raster %ubpp cs=%u\n",
                        h->bits_per_pixel, h->color_space);
                free(img); free(line); free(ra); free(rb); free(tone); free(r2);
                return -1;
            }
            if (st == 0 && y + 1 < h->height) { r2[y + 1][0] = rb[0]; r2[y + 1][1] = rb[1]; }
            if (!present) {                            /* past the raster: img row stays zero */
                uint8_t *skip = ra; ra = rb; rb = skip;
                present = (st == 0);
                continue;
            }
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
            for (uint32_t x = 0; x < xout; x++) {
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
            halftone_row(res->ht, tone, (int)xout, (int)y, img + (size_t)y * stride);
            uint8_t *tmp = ra; ra = rb; rb = tmp;
            present = (st == 0);
        }
        /* libcups writes `height` rows per page while we encode only `yd` of them
         * (yd == height - 1); drain the rest so the next page header stays aligned. */
        while (ras_read_line(r, line) == 1)
            ;
        free(line); free(ra); free(rb); free(tone); free(r2);
    }

    uint8_t *bie = NULL; size_t blen = 0;
    if (jbig_encode_bie(xd, yd, img, stride, 128, 0x48, &bie, &blen)) {
        free(img); return -1;
    }
    free(img);

    /* ---- page header ---- */
    int dup_on, longedge;
    if (!j->dup_known) {                       /* the vendor fixes the pair on page 1 */
        duplex_binding(j, h, &j->dup_on, &j->longedge);
        j->dup_known = 1;
    }
    dup_on = j->dup_on; longedge = j->longedge;
    if (pjl(s, "@PJL SET PAGESTATUS=START\r\n")) goto err;
    if (!dup_on) {
        if (wr(s, "@PJL SET DUPLEX=OFF\r\n")) goto err;
    } else {
        if (wr(s, "@PJL SET DUPLEX=ON\r\n")) goto err;
        if (wr(s, longedge ? "@PJL SET BINDING=LONGEDGE\r\n"
                           : "@PJL SET BINDING=SHORTEDGE\r\n")) goto err;
    }
    if (pjl(s, "@PJL SET PAPER=%s\r\n", code)) goto err;
    if (pjl(s, "@PJL SET PAPERWIDTH=%u\r\n", pw)) goto err;
    if (pjl(s, "@PJL SET PAPERLENGTH=%u\r\n", pl)) goto err;
    if (wr(s, "@PJL SET MEDIASOURCE=AUTO\r\n")) goto err;
    if (pjl(s, "@PJL SET MEDIATYPE=%s\r\n", media_type_code(h->media_type))) goto err;
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
