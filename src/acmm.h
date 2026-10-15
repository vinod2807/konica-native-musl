#ifndef ACMM_H
#define ACMM_H
/*
 * The vendor colour-management (ACMM) neighbourhood stage.
 *
 * The vendor filter applies a 3x1 "colour-world" filter element
 * (PCM_lfFilter8Eval3x1) to the 8-bit source one byte/sample, reading a
 * three-sample sliding window.  Recovered exactly from the disassembly and
 * verified byte-for-byte against the running vendor filter:
 *
 *     T_raw(x) = Phi( src[x], src[x+1], src[x+2] )
 *
 * Phi is tetrahedral interpolation of a 33x33x33 byte table, with
 *     i_k = s_k >> 3                    lattice index (0..32)
 *     f_k = (s_k & 7)*32 + (s_k >> 3)   the vendor's weight/order code
 *   cell corner offsets {0,1089,33,1122,1,1090,34,1123}   (strides 33^2,33,1)
 *   and  out = ( sum(weight * corner) * 0x80808081 ) >> 39   (i.e. /255)
 *
 * The 33^3 table is generated at runtime by the vendor's CMM from the installed
 * grey profile (Profiles/sGrayInv.icm); it is not a file the vendor ships.  To
 * stay dependency-free and embed no vendor data, this module loads a dump of the
 * table (35937 bytes) captured from the running vendor filter — see
 * tools/dump-lut.gdb and docs/REVERSE-ENGINEERING.md.  When no dump is supplied
 * the caller falls back to the flat-tone approximation.
 */
#include <stdint.h>

typedef struct acmm acmm_t;

/* Load a 33^3 (35937-byte) LUT dump; NULL if unavailable. */
acmm_t *acmm_load(const char *path);
void    acmm_free(acmm_t *a);

/* Phi(s0,s1,s2) -> 0..255. */
int     acmm_phi(const acmm_t *a, int s0, int s1, int s2);

#endif
