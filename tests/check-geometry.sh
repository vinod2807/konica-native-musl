#!/bin/sh
# Geometry-keying regression test.
#
# The vendor derives PAPERWIDTH/PAPERLENGTH and the BIH Xd/Yd crop from the raster
# header's PageSize *in points*: not from the PageSize= option, not from PageSizeName,
# and not from the raster's pixel dimensions.  This matters because a real pdftoraster
# A4 page is 4961x7016 and calls itself "A4.Borderless" -- a name that is not in the PPD
# at all -- so a name-keyed table fell through to the pixel-dimension fallback and
# emitted 4960/7015 instead of the vendor's 4958/7016.  The vendor emits 4958/7016 with
# Xd/Yd = 4957/7016 for *both* the corpus raster (4958x7017, name "A4") and the real
# pdftoraster raster (4961x7016, name "A4.Borderless").
#
# This test pins the full precedence chain, points -> PageSizeName -> pixel dimensions,
# plus the fact that the vendor reports the *device* 600 dpi whatever the raster says.
#
# The geometry table itself lives in src/encode.c; each row was re-verified against the
# running vendor filter by rewriting the raster's PageSize field (see docs/DEVICE-STREAM.md).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MKR=${MKRASTER:-$HERE/tools/mkraster}
[ -x "$MKR" ] || cc -O2 -Wno-deprecated-declarations -o "$MKR" "$HERE/tools/mkraster.c" -lcups || exit 2

# Encoding needs the vendor's halftone screen and ICC profiles at run time (by design:
# nothing is vendored into the kit).  Skip cleanly when they are not installed.
HFT=${KONICA_HFT:-/usr/local/lib/konica/KonicaMinolta/245igdi/Halftones/COPA.DEFAULT.600x600.hft}
SRGB=${KONICA_SRGB:-/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/sRGB.icm}
GRAY=${KONICA_GRAY:-/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/sGrayInv.icm}
for f in "$HFT" "$SRGB" "$GRAY"; do
    [ -r "$f" ] || { echo "SKIP check-geometry: $f not readable (set KONICA_HFT/KONICA_SRGB/KONICA_GRAY)"; exit 0; }
done

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
OPTS='PageSize=A4 media=A4 Duplex=None sides=one-sided Duplexer=true MediaType=Plain back-side-orientation=Rotated'
fail=0

# Base raster: 4960x7016, 8bpp gray, blank.  Without -p mkraster writes
# PageSize = px*72/dpi = 595x841 points and an empty PageSizeName; every case below
# rewrites those two fields, which is exactly what the vendor keys on.
"$MKR" -w 4960 -h 7016 -r 600 -b 8 -c w -P blank > "$T/base.ras" || exit 2

mkvariant() { # <out> <points-x> <points-y> <page-size-name> <hw-resolution>
python3 -I - "$T/base.ras" "$1" "$2" "$3" "$4" "$5" <<'PY'
import struct, sys
src, dst, px, py, name, res = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], int(sys.argv[6])
d = bytearray(open(src, "rb").read())
struct.pack_into("<I", d, 356, px)      # PageSize[0] in points (file offset 4 + 352)
struct.pack_into("<I", d, 360, py)      # PageSize[1] in points
struct.pack_into("<I", d, 280, res)     # HWResolution[0]      (file offset 4 + 276)
struct.pack_into("<I", d, 284, res)     # HWResolution[1]
nm = name.encode()[:63]
d[1736:1800] = nm + b"\0" * (64 - len(nm))   # PageSizeName  (file offset 4 + 1732)
open(dst, "wb").write(d)
PY
}

read_fields() { # <prn> -> "PAPERWIDTH PAPERLENGTH RESOLUTION Xd Yd"
python3 -I - "$1" <<'PY'
import re, sys
d = open(sys.argv[1], "rb").read()
pw = int(re.search(rb"PAPERWIDTH=(\d+)", d).group(1))
pl = int(re.search(rb"PAPERLENGTH=(\d+)", d).group(1))
rs = int(re.search(rb"RESOLUTION=(\d+)", d).group(1))
# Concatenate the IMAGELEN chunks to get the Bi-level Image Header, whose Xd/Yd are
# stored big-endian at offsets 4 and 8.
i, bie = 0, bytearray()
while i < len(d):
    if d[i:i + 9] == b"\x1b%-12345X": i += 9; continue
    m = re.match(rb"@PJL SET IMAGELEN=(\d+)\r\n", d[i:])
    if m:
        n = int(m.group(1)); i += m.end(); bie += d[i:i + n]; i += n; continue
    m = re.match(rb"(@PJL[^\r\n]*)\r\n", d[i:]) or re.match(rb"\r\n", d[i:])
    if m: i += m.end(); continue
    break
print(pw, pl, rs, int.from_bytes(bie[4:8], "big"), int.from_bytes(bie[8:12], "big"))
PY
}

check() { # <label> <raster> <paperwidth> <paperlength> <Xd> <Yd> <resolution>
    label=$1; ras=$2
    "$HERE/konica-native" 1 geotest t 1 "$OPTS" < "$ras" > "$T/out.prn" 2> "$T/err"
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL $label: filter exited $rc ($(head -1 "$T/err"))"; fail=1; return
    fi
    got=$(read_fields "$T/out.prn")
    want="$3 $4 $7 $5 $6"
    if [ "$got" = "$want" ]; then echo "PASS $label"
    else echo "FAIL $label: got '$got', want '$want' [PAPERWIDTH PAPERLENGTH RESOLUTION Xd Yd]"; fail=1
    fi
}

# 1. A4 points, no name -> keyed by points.
mkvariant "$T/a.ras" 595 842 "" 600
check "A4 by points (no name)"            "$T/a.ras" 4958 7016 4957 7016 600

# 2. A4 points, but the name the real pdftoraster uses (absent from the PPD).
mkvariant "$T/b.ras" 595 842 "A4.Borderless" 600
check "A4 by points (name A4.Borderless)" "$T/b.ras" 4958 7016 4957 7016 600

# 3. The name says A4, the points say Letter -> points win.  This is the regression that
#    the old name-first table got wrong on real documents.
mkvariant "$T/c.ras" 612 792 "A4" 600
check "Letter by points (name says A4)"   "$T/c.ras" 5100 6600 5100 6600 600

# 4. Unknown points, known name -> name is the fallback.
mkvariant "$T/d.ras" 100 200 "Letter" 600
check "Letter by name (unknown points)"   "$T/d.ras" 5100 6600 5100 6600 600

# 5. Unknown points and unknown name -> pixel dimensions, the documented last resort
#    (width, height-1, width-1, height-1).
mkvariant "$T/e.ras" 0 0 "NoSuchSize" 600
check "pixel fallback (unknown size)"     "$T/e.ras" 4960 7015 4959 7015 600

# 6. The device is 600 dpi and the vendor forces RESOLUTION=600 even when the raster's
#    HWResolution says otherwise (verified against the running filter at 300 and 1200).
mkvariant "$T/f.ras" 595 842 "A4" 300
check "forced 600 dpi (raster says 300)"  "$T/f.ras" 4958 7016 4957 7016 600

if [ $fail -ne 0 ]; then echo "check-geometry: FAILED"; exit 1; fi
echo "check-geometry: all cases pass"
