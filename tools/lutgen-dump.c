/*
 * lutgen-dump — write the regenerated CMM 33x33x33 table (35937 bytes) to
 * stdout, so tests/check-lutgen.sh can compare it with the vendor's table.
 *
 *   tools/lutgen-dump [srgb.icm gray.icm]
 *
 * With no arguments the profiles are taken from the vendor's install paths.
 */
#include <stdio.h>
#include "lutgen.h"

#define DEF_DIR "/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/"

int main(int argc, char **argv)
{
    static uint8_t lut[LUTGEN_LEN];
    const char *srgb = (argc > 2) ? argv[1] : DEF_DIR "sRGB.icm";
    const char *gray = (argc > 2) ? argv[2] : DEF_DIR "sGrayInv.icm";

    if (lutgen_build(srgb, gray, lut) != 0) return 1;
    return fwrite(lut, 1, sizeof lut, stdout) == sizeof lut ? 0 : 1;
}
