/*
 * Regenerate the vendor ACMM 33x33x33 table from installed ICC profiles.
 *
 * The pipeline and its constants are documented in lutgen.h and
 * docs/LUT-GENERATOR.md.  What matters here is that every arithmetic step is
 * reproduced in the vendor's own precision and order: the CMM runs its
 * conversion stages in IEEE single precision (SSE float), so this file keeps
 * its intermediates in `float` and avoids fusing multiplies into adds
 * (-ffp-contract=off in the Makefile), and it calls the same libm pow() the
 * vendor calls for the cube root.
 */
#include "lutgen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vendor constants, transcribed from the filter's .rodata. */
#define TRC_NORM    255.0f    /* 0x468580: TRC input divisor                  */
#define LAB_YN      127.5f    /* 0x4689bc: PCS white Y, D50 scaled by 127.5   */
#define LAB_L_A     1.16f     /* 0x468a00: L = (1.16*fy - 0.16) * 255         */
#define LAB_L_B     0.16f     /* 0x468a04                                     */
#define LAB_L_NORM  255.0f    /* 0x4689c8                                     */
#define LAB_TOE_M   841.0f    /* 0x4689f8                                     */
#define LAB_TOE_D   108.0f    /* 0x4689fc                                     */
#define LAB_K       0x1.1a7b96p-3f   /* 0x4689e0: 16/116                      */
#define LAB_TH      0x1.22354ep-7f   /* 0x4689ec: 0.008856452070176601        */

/* --- minimal ICC reader -------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long  n;
    uint8_t *b;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0 || n > (long)(8u << 20)) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    b = malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* Offset of a tag, or NULL if absent/malformed. */
static const uint8_t *icc_tag(const uint8_t *b, size_t len, const char *sig,
                              uint32_t *tsz)
{
    uint32_t n, i;

    if (len < 132 || memcmp(b + 36, "acsp", 4) != 0) return NULL;
    n = be32(b + 128);
    if (n == 0 || n > 256 || (size_t)132 + (size_t)n * 12 > len) return NULL;
    for (i = 0; i < n; i++) {
        const uint8_t *e = b + 132 + i * 12;
        if (memcmp(e, sig, 4) == 0) {
            uint32_t off = be32(e + 4), sz = be32(e + 8);
            if ((size_t)off + sz > len || sz < 12) return NULL;
            *tsz = sz;
            return b + off;
        }
    }
    return NULL;
}

/* A 'curv' tag (uInt16 lookup table). */
static int curv_u16(const uint8_t *b, size_t len, const char *sig,
                    uint16_t **out, uint32_t *nout)
{
    const uint8_t *t;
    uint32_t sz, n, i;
    uint16_t *v;

    t = icc_tag(b, len, sig, &sz);
    if (!t || memcmp(t, "curv", 4) != 0) return -1;
    n = be32(t + 8);
    if (n < 2 || (size_t)12 + (size_t)n * 2 > sz) return -1;
    v = malloc((size_t)n * sizeof *v);
    if (!v) return -1;
    for (i = 0; i < n; i++) v[i] = be16(t + 12 + i * 2);
    *out = v;
    *nout = n;
    return 0;
}

/* The Y component (s15Fixed16) of an 'XYZ ' tag. */
static int xyz_y(const uint8_t *b, size_t len, const char *sig, int32_t *y)
{
    uint32_t sz;
    const uint8_t *t = icc_tag(b, len, sig, &sz);

    if (!t || memcmp(t, "XYZ ", 4) != 0 || sz < 20) return -1;
    *y = (int32_t)be32(t + 12);
    return 0;
}

/* --- the vendor's arithmetic -------------------------------------------- */

/* Vendor single-precision TRC lookup: normalise by 255, scale to (n-1)
 * intervals, truncate to an index, then a forward-difference slope. */
static float trc_lookup(const float *tab, uint32_t n, int x)
{
    float  w = (float)x / TRC_NORM;
    double p, fr;
    int    i;
    float  d;

    if (w <= 0.0f) return tab[0];
    if ((double)w >= 0.999999) return tab[n - 1];
    p  = (double)w * (double)(n - 1);
    i  = (int)p;
    fr = p - (double)i;
    if (fr <= 0.0) return tab[i];
    d  = tab[i + 1] - tab[i];
    return (float)((double)tab[i] + fr * (double)d);
}

/* f() of the CIELAB transform. */
static float lab_f(float w)
{
    if (w <= 0.0f)      return LAB_K;
    if (w > LAB_TH)     return (float)pow((double)w, 1.0 / 3.0);
    return (w * LAB_TOE_M) / LAB_TOE_D + LAB_K;
}

/* Invert a monotonically decreasing TRC (the grey kTRC). */
static float gray_inv(const float *tab, uint32_t n, float v)
{
    uint32_t i;
    float a, b, g;

    if (v >= tab[0])     return 0.0f;
    if (v <= tab[n - 1]) return (float)(n - 1);
    for (i = 0; i + 1 < n; i++)
        if (tab[i] >= v && v >= tab[i + 1]) break;
    a = tab[i];
    b = tab[i + 1];
    g = 1.0f / (a - b);
    return ((a - v) * (float)(i + 1) + (v - b) * (float)i) * g;
}

/* PCM_fVArrayCopy: clamp then round-to-nearest, half away from zero. */
static uint8_t to_byte(float v)
{
    if (v <= 0.0f)   return 0;
    if (v >= 254.5f) return 255;          /* vendor: v >= 255 - 0.5 -> 255 */
    return (uint8_t)(int)(v + 0.5f);
}

int lutgen_build(const char *srgb_icm, const char *gray_icm,
                 uint8_t out[LUTGEN_LEN])
{
    uint8_t *sb = NULL, *gb = NULL;
    size_t  slen = 0, glen = 0;
    uint16_t *itu = NULL, *otu = NULL;
    uint32_t itn = 0, otn = 0;
    float *itab = NULL, *otab = NULL;
    float  my[3];
    int32_t yr, yg, yb;
    uint32_t i, j, k;
    int    rc = -1;

    sb = slurp(srgb_icm, &slen);
    if (!sb) { fprintf(stderr, "lutgen: cannot read %s\n", srgb_icm); goto done; }
    gb = slurp(gray_icm, &glen);
    if (!gb) { fprintf(stderr, "lutgen: cannot read %s\n", gray_icm); goto done; }

    /* input TRC (sRGB.icm rTRC), output TRC (sGrayInv.icm kTRC) */
    if (curv_u16(sb, slen, "rTRC", &itu, &itn) != 0) {
        fprintf(stderr, "lutgen: %s: no usable rTRC\n", srgb_icm);
        goto done;
    }
    if (curv_u16(gb, glen, "kTRC", &otu, &otn) != 0) {
        fprintf(stderr, "lutgen: %s: no usable kTRC\n", gray_icm);
        goto done;
    }

    /* scale 0..65535 -> 0..255 (the vendor multiplies by a precomputed
     * reciprocal; doing it that way reproduces its 1024- and 256-entry tables
     * exactly, where v*255/65535 is a ULP out on a few entries) */
    itab = malloc((size_t)itn * sizeof *itab);
    otab = malloc((size_t)otn * sizeof *otab);
    if (!itab || !otab) { fprintf(stderr, "lutgen: out of memory\n"); goto done; }
    for (i = 0; i < itn; i++)
        itab[i] = (float)itu[i] * (255.0f / 65535.0f);
    for (i = 0; i < otn; i++)
        otab[i] = (float)otu[i] * (255.0f / 65535.0f);
    if (otab[0] <= otab[otn - 1]) {
        fprintf(stderr, "lutgen: %s: kTRC is not decreasing\n", gray_icm);
        goto done;
    }

    /* Y row of the input profile's colourant matrix, normalised so the
     * primaries sum to unity luminance (the vendor's white normalisation). */
    if (xyz_y(sb, slen, "rXYZ", &yr) != 0 ||
        xyz_y(sb, slen, "gXYZ", &yg) != 0 ||
        xyz_y(sb, slen, "bXYZ", &yb) != 0) {
        fprintf(stderr, "lutgen: %s: missing colourant matrix\n", srgb_icm);
        goto done;
    }
    {
        float sum = (float)(yr + yg + yb);
        if (sum == 0.0f) {
            fprintf(stderr, "lutgen: %s: degenerate colourant matrix\n", srgb_icm);
            goto done;
        }
        my[0] = 0.5f * ((float)yr / sum);
        my[1] = 0.5f * ((float)yg / sum);
        my[2] = 0.5f * ((float)yb / sum);
    }

    for (i = 0; i < LUTGEN_N; i++) {
        for (j = 0; j < LUTGEN_N; j++) {
            for (k = 0; k < LUTGEN_N; k++) {
                int x0 = 8 * (int)i, x1 = 8 * (int)j, x2 = 8 * (int)k;
                float t0 = trc_lookup(itab, itn, x0);
                float t1 = trc_lookup(itab, itn, x1);
                float t2 = trc_lookup(itab, itn, x2);
                float Y  = my[0] * t0 + my[1] * t1 + my[2] * t2;
                float fy = lab_f(Y / LAB_YN);
                float L  = (LAB_L_A * fy - LAB_L_B) * LAB_L_NORM;
                out[((size_t)i * LUTGEN_N + j) * LUTGEN_N + k] =
                    to_byte(gray_inv(otab, otn, L));
            }
        }
    }
    rc = 0;

done:
    free(sb);
    free(gb);
    free(itu);
    free(otu);
    free(itab);
    free(otab);
    return rc;
}
