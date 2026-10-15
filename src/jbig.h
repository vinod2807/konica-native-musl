#ifndef JBIG_H
#define JBIG_H
/*
 * Minimal JBIG1 (ITU-T T.82 / ISO 11544) bi-level encoder, specialised to the one
 * configuration the Konica 206 device stream uses:
 *
 *   1 plane, 1 resolution layer, no ATMOVE (mx = my = 0),
 *   order = ILEAVE|SMID (3),  options = LRLTWO|TPBON (0x48),  l0 lines per stripe.
 *
 * This is the same adaptive QM arithmetic coder and the same two-line (LRLTWO)
 * template that jbigkit emits, re-derived from the disassembly of the vendor
 * filter's own JBIG writer (ImxJBIGEncoderInit / ImxJBIGImageBandAdd).  It is
 * written from the T.82 tables rather than copied, so the kit stays dependency
 * free.  The one behaviour that matters for byte-exactness: the coder is NOT
 * allowed to strip trailing zero bytes from a stripe before the FF 02 SDNORM
 * marker, and the finished BIE is zero-padded up to a multiple of four bytes.
 *
 * Verified against the 60-page capture corpus: 60/60 byte-identical BIEs.
 */
#include <stddef.h>
#include <stdint.h>

/* Encode a 1-bit image (row-major, MSB first, stride = (xd+7)/8) into a BIE.
 * On success *out points to a malloc()ed buffer of *outlen bytes (caller frees). */
int jbig_encode_bie(unsigned long xd, unsigned long yd, const uint8_t *img,
                    unsigned long stride, unsigned long l0, int options,
                    uint8_t **out, size_t *outlen);

#endif
