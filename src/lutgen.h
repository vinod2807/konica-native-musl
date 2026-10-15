#ifndef LUTGEN_H
#define LUTGEN_H
/*
 * Regenerate the vendor colour-management (ACMM) table.
 *
 * The vendor filter does not ship its 33x33x33 neighbourhood table as a file:
 * its CMM builds a colour conversion from two installed ICC profiles and
 * samples that conversion on a 33-level grid, in a single-precision (SSE
 * float) pipeline.  The conversion has been recovered in full from the running
 * filter (PCM_fLUTCreate / PCM_fXFilter8Set and the stage objects they build):
 *
 *     input device space (matrix/shaper RGB, the vendor's implicit "sRGB"):
 *         t_k = rTRC[x_k]                        (Profiles/sRGB.icm, 1024-entry)
 *         Y   = M_y0*t_0 + M_y1*t_1 + M_y2*t_2   (Y row of rXYZ/gXYZ/bXYZ)
 *     PCS:
 *         fy  = f(Y / 127.5),  f(w) = w^(1/3), or 841*w/108 + 16/116 below the
 *                                   0.008856452 threshold
 *         L   = (1.16*fy - 0.16) * 255
 *     output device space (inverse grey profile):
 *         g   = kTRCinv_sGrayInv[L]              (Profiles/sGrayInv.icm, 256-entry)
 *
 * The grid is sampled at x = 8*i for i = 0..32 and each result is rounded to a
 * byte the way the vendor's PCM_fVArrayCopy does.  Everything is read from the
 * two profiles at run time, so no vendor table is embedded here; the vendor's
 * own constants (the D50 PCS white scaled to 127.5, and the Lab/L constants)
 * are reproduced literally.  See docs/LUT-GENERATOR.md.
 */
#include <stdint.h>

#define LUTGEN_N    33
#define LUTGEN_LEN  (LUTGEN_N * LUTGEN_N * LUTGEN_N)   /* 35937 */

/* Build the table into out[LUTGEN_LEN].
 *
 *   srgb_icm  input matrix/shaper RGB profile (Profiles/sRGB.icm)
 *   gray_icm  output grey profile whose kTRC is a *decreasing* curve
 *             (Profiles/sGrayInv.icm)
 *
 * Returns 0 on success, -1 if either profile is absent, unreadable or does not
 * carry the tags this conversion needs.  Soft linked with libm for pow(). */
int lutgen_build(const char *srgb_icm, const char *gray_icm,
                 uint8_t out[LUTGEN_LEN]);

#endif
