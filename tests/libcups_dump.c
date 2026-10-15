/* Reference reader: prints the same lines as `konica-native --dump`, using libcups. */
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    int fd = 0;
    if (argc > 1) { FILE *f = fopen(argv[1], "rb"); if (!f) return 1; fd = fileno(f); }
    cups_raster_t *r = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!r) return 1;
    cups_page_header2_t h;
    int page = 0;
    while (cupsRasterReadHeader2(r, &h)) {
        uint64_t hash = 1469598103934665603ULL;
        unsigned char *line = malloc(h.cupsBytesPerLine);
        unsigned rows = 0;
        for (unsigned y = 0; y < h.cupsHeight; y++) {
            if (cupsRasterReadPixels(r, line, h.cupsBytesPerLine) != h.cupsBytesPerLine) break;
            for (unsigned i = 0; i < h.cupsBytesPerLine; i++) { hash ^= line[i]; hash *= 1099511628211ULL; }
            rows++;
        }
        free(line);
        printf("page %d: %ux%u %ubpp cs=%u dpi=%ux%u duplex=%u tumble=%u rows=%u fnv=%016llx\n",
               ++page, h.cupsWidth, h.cupsHeight, h.cupsBitsPerPixel, (unsigned)h.cupsColorSpace,
               h.HWResolution[0], h.HWResolution[1], h.Duplex, h.Tumble, rows, (unsigned long long)hash);
    }
    cupsRasterClose(r);
    return 0;
}
