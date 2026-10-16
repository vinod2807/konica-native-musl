#ifndef ENCODE_H
#define ENCODE_H
#include "raster.h"
#include <stdio.h>

typedef struct { FILE *out; unsigned long long bytes; } sink_t;

typedef struct {
    const char *options;     /* argv[5] exactly as CUPS passed it */
    /* Duplex=...: 0 absent, 1 present but not a duplex mode (None, Foo, ...), 2 DuplexNoTumble,
     * 3 DuplexTumble.  Matched case-insensitively, as the vendor does. */
    int duplex_opt;
    /* sides=...: 0 absent or not one of the two spellings below, 2 two-sided-long-edge,
     * 3 two-sided-short-edge.  Matched case-sensitively, as the vendor does. */
    int sides;
    int toner_save;          /* OCM_TonerSave: 1 TRUE, 0 FALSE; absent resolves to 1 (TRUE, corpus-proven) */
    /* The vendor works DUPLEX/BINDING out once, from the *first* page's header, and then
     * prints the same pair on every page of the job (a two-page job whose pages differ in
     * orientation or in their header Duplex/Tumble fields still gets one pair).  Fixed on
     * the first enc_page() call. */
    int dup_known, dup_on, longedge;
} job_t;

int sink_write(sink_t *s, const void *p, size_t n);

/* Runtime resources: the halftone screen and the colour table. They are loaded by
 * enc_open() *before* the first byte of the job is written, so a missing runtime file
 * fails the job cleanly instead of emitting a truncated PJL job (which can leave the
 * printer stuck in "data receiving"). enc_open() returns NULL on failure, having written
 * the reason to stderr. */
typedef struct enc_res enc_res_t;
enc_res_t *enc_open(void);
void enc_close(enc_res_t *res);

/* The three stages of a job. These are the whole of the reverse-engineering target:
 * everything between the raster the filter reads and the bytes the USB backend gets.
 * Return 0 on success, -1 on error (message already written to stderr). */
int enc_job_begin(sink_t *s, const job_t *j);
int enc_page(sink_t *s, job_t *j, enc_res_t *res, const ras_hdr_t *h, ras_t *r);
/* False when the page's size is outside the range the vendor supports, in which case the
 * vendor refuses the whole job: exit 2 with nothing on stdout.  Checked before the first
 * byte so we do not leave a truncated PJL header on the printer. */
int enc_page_size_ok(const ras_hdr_t *h);
int enc_job_end(sink_t *s, const job_t *j);

#endif
