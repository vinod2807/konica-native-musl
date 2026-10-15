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

/* The three stages of a job. These are the whole of the reverse-engineering target:
 * everything between the raster the filter reads and the bytes the USB backend gets.
 * Return 0 on success, -1 on error (message already written to stderr). */
int enc_job_begin(sink_t *s, const job_t *j);
int enc_page(sink_t *s, const job_t *j, const ras_hdr_t *h, ras_t *r);
int enc_job_end(sink_t *s, const job_t *j);

#endif
