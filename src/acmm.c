#include "acmm.h"
#include "lutgen.h"
#include <stdio.h>
#include <stdlib.h>

#define ACMM_N   33
#define ACMM_LEN (ACMM_N * ACMM_N * ACMM_N)     /* 35937 */

struct acmm { uint8_t lut[ACMM_LEN]; };

/* The vendor's per-sample code: fraction in bits 7..5, lattice index in 4..0. */
static int bcode(int s) { return ((s & 7) << 5) + (s >> 3); }

acmm_t *acmm_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    acmm_t *a = malloc(sizeof *a);
    if (!a || fread(a->lut, 1, ACMM_LEN, f) != ACMM_LEN) {
        free(a); fclose(f); return NULL;
    }
    fclose(f);
    return a;
}

void acmm_free(acmm_t *a) { free(a); }

acmm_t *acmm_generate(const char *srgb_icm, const char *gray_icm)
{
    acmm_t *a = malloc(sizeof *a);
    if (!a) return NULL;
    if (lutgen_build(srgb_icm, gray_icm, a->lut) != 0) { free(a); return NULL; }
    return a;
}

int acmm_phi(const acmm_t *a, int s0, int s1, int s2)
{
    int f[3], idx[3] = { 0, 1, 2 }, c[3] = { 0, 0, 0 };
    int base, acc, i, j;

    f[0] = bcode(s0); f[1] = bcode(s1); f[2] = bcode(s2);

    /* stable ascending sort of the three axes by their code (matches the
     * vendor's cmp/jae chain, which is stable on ties). */
    for (i = 1; i < 3; i++) {
        int k = idx[i], v = f[k];
        for (j = i - 1; j >= 0 && f[idx[j]] > v; j--) idx[j + 1] = idx[j];
        idx[j + 1] = k;
    }

    base = (s0 >> 3) * 1089 + (s1 >> 3) * 33 + (s2 >> 3);

    acc  = (255 - f[idx[2]]) * a->lut[base + c[0]*1089 + c[1]*33 + c[2]];
    c[idx[2]] = 1;
    acc += (f[idx[2]] - f[idx[1]]) * a->lut[base + c[0]*1089 + c[1]*33 + c[2]];
    c[idx[1]] = 1;
    acc += (f[idx[1]] - f[idx[0]]) * a->lut[base + c[0]*1089 + c[1]*33 + c[2]];
    c[idx[0]] = 1;
    acc += f[idx[0]] * a->lut[base + c[0]*1089 + c[1]*33 + c[2]];

    return (int)(((uint64_t)(uint32_t)acc * 0x80808081ULL) >> 39);
}
