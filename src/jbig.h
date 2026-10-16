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
 * marker.
 *
 * Verified against the 60-page capture corpus: 60/60 byte-identical BIEs, and
 * byte-identical on real pdftoraster output except for the 0-3 pad bytes below.
 *
 * PAD BYTES (known, documented divergence): the stream is rounded up to a multiple of
 * four bytes.  Those bytes follow the last SDNORM marker and the device never decodes
 * them, but the vendor writes *uninitialised* malloc()ed memory there, so their value
 * tracks the allocator's history rather than the image.  Evidence (2026-10-16):
 *   - real 2-page A4 document, page lengths 77476 and 44036 (both < 128 KiB, so glibc
 *     serves both from the heap): page 2's pad bytes were 42 be, exactly page 1's bytes
 *     at the same offsets - a fresh malloc of 44036 bytes reused the block freed after
 *     page 1 and its tail was never written;
 *   - corpus cases 029/031/033 (page lengths >= 128 KiB, so glibc serves them with
 *     mmap and hands back zeroed pages): the pad bytes were 00 00, even though stale
 *     bytes were available in the previous page's block.
 * Reproducing that would mean emulating the C library's allocator, so this encoder emits
 * deterministic zeros instead.  Byte-exact diffs of real documents should mask the final
 * (len % 4 ? 4 - len % 4 : 0) bytes of any page whose stream is not already 4-aligned.
 */
#include <stddef.h>
#include <stdint.h>

/* Encode a 1-bit image (row-major, MSB first, stride = (xd+7)/8) into a BIE.
 * On success *out points to a malloc()ed buffer of *outlen bytes (caller frees);
 * *outlen is the stream length rounded up to a multiple of four with zero pad bytes. */
int jbig_encode_bie(unsigned long xd, unsigned long yd, const uint8_t *img,
                    unsigned long stride, unsigned long l0, int options,
                    uint8_t **out, size_t *outlen);

#endif
