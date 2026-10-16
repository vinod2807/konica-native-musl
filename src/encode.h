#ifndef ENCODE_H
#define ENCODE_H
#include "raster.h"
#include <stdio.h>

typedef struct { FILE *out; unsigned long long bytes; } sink_t;

typedef struct {
    const char *options;     /* argv[5] exactly as CUPS passed it */
    int duplex;              /* from Duplex=...: 0 none, 1 long edge, 2 short edge */
    int toner_save;          /* OCM_TonerSave: 1 TRUE, 0 FALSE; absent resolves to 1 (TRUE, corpus-proven) */
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
int enc_page(sink_t *s, const job_t *j, enc_res_t *res, const ras_hdr_t *h, ras_t *r);
int enc_job_end(sink_t *s, const job_t *j);

#endif
