/*
 * mkraster — synthetic CUPS raster generator for reverse-engineering a raster filter.
 *
 * Build on the host where the vendor filter works (needs libcups headers):
 *     cc -O2 -Wall -Wno-deprecated-declarations -o mkraster mkraster.c -lcups
 *
 * Why synthetic: with a *known* pixel pattern going in, every difference in the
 * vendor filter's output is attributable to that pattern. Single pixels, lines and
 * gradients reveal how pixels map to device bytes; real documents cannot.
 *
 * With -p PPD the page header comes from cupsRasterInterpretPPD(), exactly as
 * pdftoraster/gstoraster fill it, so the filter sees realistic headers.
 *
 *   mkraster -p 206.ppd -o "PageSize=A4 Duplex=DuplexNoTumble" -P solid,hgrad > in.ras
 *   mkraster -w 4960 -h 7016 -r 600 -b 8 -c w -P dot:100:100 > in.ras      (no PPD)
 *
 * Patterns (ink 0..255, 0 = bare paper), comma separated, one per page:
 *   blank  solid  hgrad  vgrad  steps[:n]  hlines:period[:thick]  vlines:period[:thick]
 *   checker:size  dot:x:y  box:x:y:w:h  frame  noise:seed  rowid  colid
 *
 * Options: -p ppd  -o "opts"  -P patterns  -n pages  -z (RLE-compressed raster)
 *          -w px -h px -r dpi -b bits(1|8) -c w|k|rgb|cmyk   (only without -p)
 */
#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

typedef struct { char name[16]; unsigned a[4]; int na; } pat_t;

static uint32_t mix(uint32_t x, uint32_t y, uint32_t s)
{
    uint32_t h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA6Bu ^ s * 0xC2B2AE35u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

static unsigned arg(const pat_t *p, int i, unsigned def) { return i < p->na ? p->a[i] : def; }

static unsigned ink_at(const pat_t *p, unsigned x, unsigned y, unsigned w, unsigned h)
{
    const char *n = p->name;
    if (!strcmp(n, "blank"))  return 0;
    if (!strcmp(n, "solid"))  return 255;
    if (!strcmp(n, "hgrad"))  return w > 1 ? x * 255u / (w - 1) : 0;
    if (!strcmp(n, "vgrad"))  return h > 1 ? y * 255u / (h - 1) : 0;
    if (!strcmp(n, "steps"))  { unsigned k = arg(p, 0, 8); if (k < 2) k = 2;
                                unsigned b = x * k / w; return b * 255u / (k - 1); }
    if (!strcmp(n, "hlines")) { unsigned per = arg(p, 0, 4), t = arg(p, 1, 1); return per && (y % per) < t ? 255 : 0; }
    if (!strcmp(n, "vlines")) { unsigned per = arg(p, 0, 4), t = arg(p, 1, 1); return per && (x % per) < t ? 255 : 0; }
    if (!strcmp(n, "checker")){ unsigned s = arg(p, 0, 1); if (!s) s = 1; return ((x / s) + (y / s)) & 1 ? 255 : 0; }
    if (!strcmp(n, "dot"))    return x == arg(p, 0, 0) && y == arg(p, 1, 0) ? 255 : 0;
    if (!strcmp(n, "box"))    { unsigned x0 = arg(p,0,0), y0 = arg(p,1,0), bw = arg(p,2,1), bh = arg(p,3,1);
                                return x >= x0 && x < x0 + bw && y >= y0 && y < y0 + bh ? 255 : 0; }
    if (!strcmp(n, "frame"))  return x == 0 || y == 0 || x == w - 1 || y == h - 1 ? 255 : 0;
    if (!strcmp(n, "noise"))  return mix(x, y, arg(p, 0, 1)) & 0xFF;
    if (!strcmp(n, "rowid"))  return x < 16 && ((y >> (15 - x)) & 1) ? 255 : 0;
    if (!strcmp(n, "colid"))  return y < 16 && ((x >> (15 - y)) & 1) ? 255 : 0;
    fprintf(stderr, "mkraster: unknown pattern '%s'\n", n);
    exit(2);
}

static int parse_patterns(char *s, pat_t *out, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        char *comma = strchr(s, ',');
        if (comma) *comma = 0;
        pat_t *p = &out[n++]; memset(p, 0, sizeof *p);
        char *c = strchr(s, ':');
        size_t len = c ? (size_t)(c - s) : strlen(s);
        if (len >= sizeof p->name) len = sizeof p->name - 1;
        memcpy(p->name, s, len);
        while (c && p->na < 4) { p->a[p->na++] = (unsigned)strtoul(c + 1, NULL, 10); c = strchr(c + 1, ':'); }
        s = comma ? comma + 1 : NULL;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *ppdfile = NULL, *optstr = "", *patstr = "blank";
    unsigned W = 4960, H = 7016, dpi = 600, bits = 8, pages = 0;
    const char *cs = "w";
    cups_mode_t mode = CUPS_RASTER_WRITE;
    int c;

    while ((c = getopt(argc, argv, "p:o:P:n:zw:h:r:b:c:")) != -1) switch (c) {
        case 'p': ppdfile = optarg; break;   case 'o': optstr = optarg; break;
        case 'P': patstr = optarg; break;    case 'n': pages = (unsigned)atoi(optarg); break;
        case 'z': mode = CUPS_RASTER_WRITE_COMPRESSED; break;
        case 'w': W = (unsigned)atoi(optarg); break;  case 'h': H = (unsigned)atoi(optarg); break;
        case 'r': dpi = (unsigned)atoi(optarg); break; case 'b': bits = (unsigned)atoi(optarg); break;
        case 'c': cs = optarg; break;
        default: fprintf(stderr, "see header comment for usage\n"); return 2;
    }

    pat_t pats[64]; char *pcopy = strdup(patstr);
    int npat = parse_patterns(pcopy, pats, 64);
    if (npat < 1) { fprintf(stderr, "mkraster: no patterns\n"); return 2; }
    if (!pages) pages = (unsigned)npat;

    cups_option_t *opts = NULL;
    int nopts = cupsParseOptions(optstr, 0, &opts);

    cups_page_header2_t hdr; memset(&hdr, 0, sizeof hdr);
    if (ppdfile) {
        ppd_file_t *ppd = ppdOpenFile(ppdfile);
        if (!ppd) { fprintf(stderr, "mkraster: cannot open PPD %s\n", ppdfile); return 1; }
        ppdMarkDefaults(ppd);
        cupsMarkOptions(ppd, nopts, opts);
        if (cupsRasterInterpretPPD(&hdr, ppd, nopts, opts, NULL)) {
            fprintf(stderr, "mkraster: cupsRasterInterpretPPD: %s\n", cupsRasterErrorString());
            return 1;
        }
        ppdClose(ppd);
    } else {
        hdr.HWResolution[0] = hdr.HWResolution[1] = dpi;
        hdr.cupsWidth = W; hdr.cupsHeight = H;
        hdr.PageSize[0] = W * 72 / dpi; hdr.PageSize[1] = H * 72 / dpi;
        hdr.cupsBitsPerColor = bits;
        hdr.cupsColorOrder = CUPS_ORDER_CHUNKED;
        hdr.NumCopies = 1;
        if (!strcmp(cs, "w"))         { hdr.cupsColorSpace = CUPS_CSPACE_W;    hdr.cupsNumColors = 1; }
        else if (!strcmp(cs, "k"))    { hdr.cupsColorSpace = CUPS_CSPACE_K;    hdr.cupsNumColors = 1; }
        else if (!strcmp(cs, "rgb"))  { hdr.cupsColorSpace = CUPS_CSPACE_RGB;  hdr.cupsNumColors = 3; }
        else if (!strcmp(cs, "cmyk")) { hdr.cupsColorSpace = CUPS_CSPACE_CMYK; hdr.cupsNumColors = 4; }
        else { fprintf(stderr, "mkraster: bad -c\n"); return 2; }
        hdr.cupsBitsPerPixel = bits * hdr.cupsNumColors;
        hdr.cupsBytesPerLine = (hdr.cupsWidth * hdr.cupsBitsPerPixel + 7) / 8;
    }

    unsigned w = hdr.cupsWidth, h = hdr.cupsHeight, bpp = hdr.cupsBitsPerPixel;
    unsigned nc = hdr.cupsNumColors ? hdr.cupsNumColors : 1;
    if (!(hdr.cupsBitsPerColor == 8 || (hdr.cupsBitsPerColor == 1 && nc == 1))) {
        fprintf(stderr, "mkraster: unsupported depth %u bits x %u colors\n", hdr.cupsBitsPerColor, nc);
        return 1;
    }
    int kidx = -1;      /* channel carrying ink, for K-like spaces */
    int inverted = 0;   /* 1 when 0 = ink (W/RGB) */
    switch (hdr.cupsColorSpace) {
        case CUPS_CSPACE_W: case CUPS_CSPACE_SW: case CUPS_CSPACE_RGB: case CUPS_CSPACE_SRGB:
        case CUPS_CSPACE_ADOBERGB: inverted = 1; break;
        case CUPS_CSPACE_K:    kidx = 0; break;
        case CUPS_CSPACE_CMYK: case CUPS_CSPACE_YMCK: kidx = 3; break;
        case CUPS_CSPACE_KCMY: kidx = 0; break;
        default: fprintf(stderr, "mkraster: unsupported colour space %d\n", (int)hdr.cupsColorSpace); return 1;
    }

    cups_raster_t *ras = cupsRasterOpen(1, mode);
    if (!ras) { fprintf(stderr, "mkraster: cupsRasterOpen failed\n"); return 1; }
    unsigned char *line = malloc(hdr.cupsBytesPerLine);

    for (unsigned pg = 0; pg < pages; pg++) {
        const pat_t *p = &pats[pg % (unsigned)npat];
        if (!cupsRasterWriteHeader2(ras, &hdr)) { fprintf(stderr, "mkraster: header write failed\n"); return 1; }
        for (unsigned y = 0; y < h; y++) {
            memset(line, 0, hdr.cupsBytesPerLine);
            for (unsigned x = 0; x < w; x++) {
                unsigned ink = ink_at(p, x, y, w, h);
                if (hdr.cupsBitsPerColor == 1) {
                    int dark = ink >= 128;
                    int bit = inverted ? !dark : dark;
                    if (bit) line[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
                } else {
                    unsigned char *px = line + x * (bpp / 8);
                    for (unsigned k = 0; k < nc; k++)
                        px[k] = inverted ? (unsigned char)(255 - ink) : ((int)k == kidx ? (unsigned char)ink : 0);
                }
            }
            if (cupsRasterWritePixels(ras, line, hdr.cupsBytesPerLine) != hdr.cupsBytesPerLine) {
                fprintf(stderr, "mkraster: pixel write failed (row %u)\n", y); return 1;
            }
        }
    }
    cupsRasterClose(ras);
    free(line); free(pcopy);
    fprintf(stderr, "mkraster: %u page(s) %ux%u px, %u dpi, %u bpp, cspace %d\n",
            pages, w, h, hdr.HWResolution[0], bpp, (int)hdr.cupsColorSpace);
    return 0;
}
