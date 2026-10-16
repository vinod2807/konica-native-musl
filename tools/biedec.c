/*
 * biedec -- decode a JBIG1 BIE (the page image inside a 245igdirf device stream) to a PBM.
 *
 * Test tool only: it links the system libjbig, which the kit itself never needs (the kit
 * only encodes).  It exists so tests can compare what two filters *decoded* to, instead of
 * comparing compressed bytes -- the compressed form can differ in ways that do not change
 * a single pixel (the never-decoded BIE alignment pad is the extreme case).
 *
 * Build:  cc -O2 -Wall -o tools/biedec tools/biedec.c -ljbig
 * Use:    tools/biedec < page.bie > page.pbm     (P4, 1 = ink, matching the encoder)
 *
 * The input file must be the raw BIE (SDNORM-terminated); use
 *   python3 tools/prnscan.py bie <stream.prn> <out-prefix>
 * to cut the pages out of a device stream first.
 */
#include <jbig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        fprintf(stderr, "usage: %s < page.bie > page.pbm\n", argv[0]);
        return 2;
    }
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return 1;
    for (;;) {
        if (len == cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) return 1; }
        size_t n = fread(buf + len, 1, cap - len, stdin);
        if (!n) break;
        len += n;
    }

    struct jbg_dec_state d;
    jbg_dec_init(&d);
    size_t cnt = 0;
    int rc = jbg_dec_in(&d, buf, len, &cnt);
    if (rc != JBG_EOK) {
        fprintf(stderr, "biedec: decode failed (rc=%d) after %zu of %zu B\n", rc, cnt, len);
        return 1;
    }
    long w = jbg_dec_getwidth(&d), h = jbg_dec_getheight(&d);
    const unsigned char *img = jbg_dec_getimage(&d, 0);
    size_t bytes = jbg_dec_getsize(&d);
    if (w <= 0 || h <= 0 || !img || bytes < (size_t)((w + 7) / 8) * (size_t)h) {
        fprintf(stderr, "biedec: no image\n");
        return 1;
    }
    size_t stride = (size_t)(w + 7) / 8, bits = 0;
    for (size_t i = 0; i < stride * (size_t)h; i++)
        for (int b = 0; b < 8; b++) bits += (img[i] >> (7 - b)) & 1;
    fprintf(stderr, "biedec: %ldx%ld, %zu ink bit(s), %zu B decoded\n", w, h, bits, bytes);

    printf("P4\n%ld %ld\n", w, h);
    fwrite(img, 1, stride * (size_t)h, stdout);
    jbg_dec_free(&d);
    free(buf);
    return 0;
}
