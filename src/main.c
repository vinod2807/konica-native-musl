/*
 * konica-native — replacement for the vendor raster filter 245igdirf (musl-clean).
 *
 *   As a CUPS filter:  konica-native job user title copies options [file]
 *   Reader check:      konica-native --dump [file]     (headers + FNV-1a hash of all pixels)
 */
#include "encode.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *opt_value(const char *opts, const char *key, char *buf, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = opts; p && *p;) {
        while (*p == ' ') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != ' ' && i + 1 < n) buf[i++] = *p++;
            buf[i] = 0;
            return buf;
        }
        while (*p && *p != ' ') p++;
    }
    return NULL;
}

static void parse_job(job_t *j, const char *opts)
{
    char v[64];
    j->options = opts;
    j->duplex = 0;
    if (opt_value(opts, "Duplex", v, sizeof v)) {
        if (!strcmp(v, "DuplexNoTumble")) j->duplex = 1;
        else if (!strcmp(v, "DuplexTumble")) j->duplex = 2;
    }
    /* OCM_TonerSave resolves to TRUE when the option is absent. Corpus-proven: case 005
     * (option absent) is byte-identical to case 034 (OCM_TonerSave=TRUE), while 035
     * (=FALSE) differs. Toner save changes only the tone baseline, not the halftone
     * neighbourhood stage, so absent -> TRUE is safe for byte-exactness. */
    j->toner_save = opt_value(opts, "OCM_TonerSave", v, sizeof v) ? !strcmp(v, "TRUE") : 1;
}

static int dump(FILE *in)
{
    ras_t *r = ras_open(in);
    if (!r) { fprintf(stderr, "ERROR: not a CUPS raster stream\n"); return 1; }
    ras_hdr_t h;
    int page = 0, rc;
    while ((rc = ras_next_page(r, &h)) == 1) {
        uint64_t hash = 1469598103934665603ULL;
        uint8_t *line = malloc(h.bytes_per_line);
        if (!line) { fprintf(stderr, "ERROR: out of memory\n"); return 1; }
        uint32_t rows = 0;
        int lr;
        while ((lr = ras_read_line(r, line)) == 1) {
            for (uint32_t i = 0; i < h.bytes_per_line; i++) { hash ^= line[i]; hash *= 1099511628211ULL; }
            rows++;
        }
        free(line);
        if (lr < 0) { fprintf(stderr, "ERROR: truncated raster on page %d row %u\n", page + 1, rows); return 1; }
        printf("page %d: %ux%u %ubpp cs=%u dpi=%ux%u duplex=%u tumble=%u rows=%u fnv=%016llx\n",
               ++page, h.width, h.height, h.bits_per_pixel, h.color_space, h.hw_res[0], h.hw_res[1],
               h.duplex, h.tumble, rows, (unsigned long long)hash);
    }
    ras_close(r);
    return rc < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--dump")) {
        FILE *in = argc > 2 ? fopen(argv[2], "rb") : stdin;
        if (!in) { perror("ERROR: open"); return 1; }
        return dump(in);
    }
    if (argc < 6) {
        fprintf(stderr, "ERROR: usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }
    FILE *in = argc > 6 ? fopen(argv[6], "rb") : stdin;
    if (!in) { perror("ERROR: open raster"); return 1; }
    ras_t *r = ras_open(in);
    if (!r) { fprintf(stderr, "ERROR: input is not a CUPS raster stream\n"); return 1; }

    job_t job; parse_job(&job, argv[5]);
    sink_t out = { stdout, 0 };
    if (enc_job_begin(&out, &job)) return 1;

    ras_hdr_t h;
    int rc;
    while ((rc = ras_next_page(r, &h)) == 1)
        if (enc_page(&out, &job, &h, r)) return 1;
    if (rc < 0) { fprintf(stderr, "ERROR: bad page header\n"); return 1; }

    if (enc_job_end(&out, &job)) return 1;
    fflush(stdout);
    ras_close(r);
    return 0;
}
