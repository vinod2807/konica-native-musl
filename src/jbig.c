#include "jbig.h"
#include <stdlib.h>
#include <string.h>

/* ---- T.82 Table 24: probability estimation state machine ---- */
static const unsigned short lsztab[113] = {
  0x5a1d,0x2586,0x1114,0x080b,0x03d8,0x01da,0x00e5,0x006f,0x0036,0x001a,
  0x000d,0x0006,0x0003,0x0001,0x5a7f,0x3f25,0x2cf2,0x207c,0x17b9,0x1182,
  0x0cef,0x09a1,0x072f,0x055c,0x0406,0x0303,0x0240,0x01b1,0x0144,0x00f5,
  0x00b7,0x008a,0x0068,0x004e,0x003b,0x002c,0x5ae1,0x484c,0x3a0d,0x2ef1,
  0x261f,0x1f33,0x19a8,0x1518,0x1177,0x0e74,0x0bfb,0x09f8,0x0861,0x0706,
  0x05cd,0x04de,0x040f,0x0363,0x02d4,0x025c,0x01f8,0x01a4,0x0160,0x0125,
  0x00f6,0x00cb,0x00ab,0x008f,0x5b12,0x4d04,0x412c,0x37d8,0x2fe8,0x293c,
  0x2379,0x1edf,0x1aa9,0x174e,0x1424,0x119c,0x0f6b,0x0d51,0x0bb6,0x0a40,
  0x5832,0x4d1c,0x438e,0x3bdd,0x34ee,0x2eae,0x299a,0x2516,0x5570,0x4ca9,
  0x44d9,0x3e22,0x3824,0x32b4,0x2e17,0x56a8,0x4f46,0x47e5,0x41cf,0x3c3d,
  0x375e,0x5231,0x4c0f,0x4639,0x415e,0x5627,0x50e7,0x4b85,0x5597,0x504f,
  0x5a10,0x5522,0x59eb };
static const unsigned char nmpstab[113] = {
  1,2,3,4,5,6,7,8,9,10,11,12,13,13,15,16,17,18,19,20,21,22,23,24,25,26,27,
  28,29,30,31,32,33,34,35,9,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,
  53,54,55,56,57,58,59,60,61,62,63,32,65,66,67,68,69,70,71,72,73,74,75,76,
  77,78,79,48,81,82,83,84,85,86,87,71,89,90,91,92,93,94,86,96,97,98,99,100,
  93,102,103,104,99,106,107,103,109,107,111,109,111 };
static const unsigned char nlpstab[113] = {
  129,14,16,18,20,23,25,28,30,33,35,9,10,12,143,36,38,39,40,42,43,45,46,48,
  49,51,52,54,56,57,59,60,62,63,32,33,165,64,65,67,68,69,70,72,73,74,75,77,
  78,79,48,50,50,51,52,53,54,55,56,57,58,59,61,61,193,80,81,82,83,84,86,87,
  87,72,72,74,74,75,77,77,208,88,89,90,91,92,93,86,216,95,96,97,99,99,93,223,
  101,102,103,104,99,105,106,107,103,233,108,109,110,111,238,112,240 };

#define MARKER_ESC 0xff
#define TPB2CX     0x195   /* context of the typical-prediction bit, LRLTWO */

typedef struct { uint8_t *d; size_t len, cap; } buf_t;

static int bput(buf_t *b, int byte)
{
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        uint8_t *nd = realloc(b->d, ncap);
        if (!nd) return -1;
        b->d = nd; b->cap = ncap;
    }
    b->d[b->len++] = (uint8_t)byte;
    return 0;
}

typedef struct {
    uint8_t st[4096];      /* context probability state, persists across stripes */
    unsigned long c, a;
    long ct, sc;
    long buffer;           /* -1 = empty */
    buf_t *out;
} qm_t;

static void qm_init(qm_t *s, int reuse)
{
    if (!reuse) memset(s->st, 0, sizeof s->st);
    s->c = 0; s->a = 0x10000L; s->sc = 0; s->ct = 11; s->buffer = -1;
}

static void qm_encode(qm_t *s, int cx, int pix)
{
    unsigned lsz, ss;
    uint8_t *st = s->st + cx;
    long temp;
    ss = *st & 0x7f;
    lsz = lsztab[ss];
    if (((pix << 7) ^ *st) & 0x80) {
        if ((s->a -= lsz) >= lsz) { s->c += s->a; s->a = lsz; }
        *st &= 0x80; *st ^= nlpstab[ss];
    } else {
        if ((s->a -= lsz) & 0xffff8000L) return;
        if (s->a < lsz) { s->c += s->a; s->a = lsz; }
        *st &= 0x80; *st |= nmpstab[ss];
    }
    do {
        s->a <<= 1; s->c <<= 1; --s->ct;
        if (s->ct == 0) {
            temp = (long)(s->c >> 19);
            if (temp & 0xffffff00L) {
                if (s->buffer >= 0) {
                    ++s->buffer; bput(s->out, (int)s->buffer);
                    if (s->buffer == MARKER_ESC) bput(s->out, 0x00);
                }
                for (; s->sc; --s->sc) bput(s->out, 0x00);
                s->buffer = temp & 0xff;
            } else if (temp == 0xff) {
                ++s->sc;
            } else {
                if (s->buffer >= 0) bput(s->out, (int)s->buffer);
                for (; s->sc; --s->sc) { bput(s->out, 0xff); bput(s->out, 0x00); }
                s->buffer = temp;
            }
            s->c &= 0x7ffffL; s->ct = 8;
        }
    } while (s->a < 0x8000);
}

static void qm_flush(qm_t *s)
{
    unsigned long temp;
    if ((temp = (s->a - 1 + s->c) & 0xffff0000UL) < s->c) s->c = temp + 0x8000;
    else s->c = temp;
    s->c <<= s->ct;
    if (s->c & 0xf8000000UL) {
        if (s->buffer >= 0) {
            bput(s->out, (int)(s->buffer + 1));
            if (s->buffer + 1 == MARKER_ESC) bput(s->out, 0x00);
        }
        if (s->c & 0x7fff800UL) for (; s->sc; --s->sc) bput(s->out, 0x00);
    } else {
        if (s->buffer >= 0) bput(s->out, (int)s->buffer);
        for (; s->sc; --s->sc) { bput(s->out, 0xff); bput(s->out, 0x00); }
    }
    if (s->c & 0x7fff800UL) {
        bput(s->out, (int)((s->c >> 19) & 0xff));
        if (((s->c >> 19) & 0xff) == MARKER_ESC) bput(s->out, 0x00);
        if (s->c & 0x7f800UL) {
            bput(s->out, (int)((s->c >> 11) & 0xff));
            if (((s->c >> 11) & 0xff) == MARKER_ESC) bput(s->out, 0x00);
        }
    }
}

static void put32(buf_t *b, unsigned long v)
{
    bput(b, (int)((v >> 24) & 0xff)); bput(b, (int)((v >> 16) & 0xff));
    bput(b, (int)((v >> 8) & 0xff));  bput(b, (int)(v & 0xff));
}

int jbig_encode_bie(unsigned long xd, unsigned long yd, const uint8_t *img,
                    unsigned long stride, unsigned long l0, int options,
                    uint8_t **out, size_t *outlen)
{
    buf_t b; memset(&b, 0, sizeof b);
    qm_t qm; memset(&qm, 0, sizeof qm);
    unsigned long stripe, nstripes = (yd + l0 - 1) / l0;
    unsigned long gline = 0;
    int ltp_old = 0;

    /* --- Bi-level Image Header (20 bytes). The device stores Xd = width-1,
     *     Yd = height-1 (jbigkit writes the width itself), so we build it by hand. --- */
    bput(&b, 0); bput(&b, 0); bput(&b, 1); bput(&b, 0);   /* DL=0 D=0 P=1, reserved */
    put32(&b, xd);
    put32(&b, yd);
    put32(&b, l0);
    bput(&b, 0); bput(&b, 0);                /* mx, my */
    bput(&b, 3);                             /* order = ILEAVE|SMID */
    bput(&b, options & 0x7f);                /* LRLTWO|TPBON */

    for (stripe = 0; stripe < nstripes; stripe++) {
        unsigned long i, y0 = stripe * l0;
        qm.out = &b;
        qm_init(&qm, stripe != 0);
        for (i = 0; i < l0 && y0 + i < yd; i++, gline++) {
            const uint8_t *row = img + (y0 + i) * stride;
            const uint8_t *prev = gline ? img + (gline - 1) * stride : NULL;
            int ltp = 1;
            unsigned long x;
            if (prev) {
                for (x = 0; x < xd; x++) {
                    int a = (row[x >> 3] >> (7 - (x & 7))) & 1;
                    int b2 = (prev[x >> 3] >> (7 - (x & 7))) & 1;
                    if (a != b2) { ltp = 0; break; }
                }
            } else {
                for (x = 0; x < xd; x++)
                    if ((row[x >> 3] >> (7 - (x & 7))) & 1) { ltp = 0; break; }
            }
            qm_encode(&qm, TPB2CX, ltp == ltp_old);
            ltp_old = ltp;
            if (ltp) continue;

            /* LRLTWO two-line template, exactly as the vendor's writer forms it. */
            {
                unsigned long j = 0;
                const uint8_t *p = row;
                long line_h1 = 0, line_h2 = 0;
                if (prev) line_h2 = (long)prev[0] << 8;
                while (j < xd) {
                    line_h1 |= *p;
                    if (j < stride * 8 - 8 && prev) line_h2 |= *(p - stride + 1);
                    do {
                        unsigned cx, pix;
                        line_h1 <<= 1; line_h2 <<= 1;
                        cx = (unsigned)(((line_h2 >> 10) & 0x3f0) | ((line_h1 >> 9) & 0x00f));
                        pix = (unsigned)((line_h1 >> 8) & 1);
                        qm_encode(&qm, (int)cx, (int)pix);
                    } while ((++j & 7) && j < xd);
                    p++;
                }
            }
        }
        qm_flush(&qm);
        bput(&b, MARKER_ESC); bput(&b, 0x02);   /* SDNORM stripe marker */
    }
    /* Whole BIE padded to 4 bytes.  The vendor writes uninitialised malloc()ed memory
     * here (allocator-dependent, see the header comment), so we emit deterministic
     * zeros: these bytes follow the last SDNORM and are never decoded. */
    while (b.len & 3) bput(&b, 0x00);

    *out = b.d; *outlen = b.len;
    return 0;
}
