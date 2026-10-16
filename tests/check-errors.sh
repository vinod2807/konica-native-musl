#!/bin/sh
# Failure-mode tests for the runtime-data hardening.
#
# The screen and the colour table are loaded in enc_open(), before the first byte of the job
# is written, so a broken install must produce a clear non-zero exit and *nothing at all* on
# stdout.  A truncated PJL job is much worse than no job: the printer can sit in "data
# receiving" until it times out.  The historical flat-tone approximation must therefore be
# reachable only through the explicit KONICA_FLAT_TONE=1 escape hatch.
#
# This test builds its own one-page raster, so it needs neither libcups nor the vendor
# filter -- only the kit itself.  The one case that needs a real halftone screen skips when
# the vendor tree is not installed.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${NATIVE:-$HERE/konica-native}
[ -x "$BIN" ] || { echo "SKIP check-errors: $BIN not built (run make)"; exit 2; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
OPTS='PageSize=A6 media=A6 Duplex=None sides=one-sided Duplexer=true MediaType=Plain back-side-orientation=Rotated'
HFT=${KONICA_HFT:-/usr/local/lib/konica/KonicaMinolta/245igdi/Halftones/COPA.DEFAULT.600x600.hft}
fail=0

# A minimal valid one-page 8-bit grey v3 raster.  Offsets are header-relative (the file is
# the 4-byte sync word followed by this 1796-byte header), matching src/raster.c.
python3 -I - "$T/in.ras" <<'PY' || exit 2
import struct, sys
W, H = 2478, 3501                      # A6 at 600 dpi, +2/-1 like the vendor's raster
hdr = bytearray(1796)
struct.pack_into("<I", hdr, 276, 600)  # HWResolution[0]
struct.pack_into("<I", hdr, 280, 600)  # HWResolution[1]
struct.pack_into("<I", hdr, 340, 1)    # NumCopies
struct.pack_into("<I", hdr, 352, 297)  # PageSize[0], points
struct.pack_into("<I", hdr, 356, 420)  # PageSize[1], points
struct.pack_into("<I", hdr, 372, W)    # Width
struct.pack_into("<I", hdr, 376, H)    # Height
struct.pack_into("<I", hdr, 384, 8)    # BitsPerColor
struct.pack_into("<I", hdr, 388, 8)    # BitsPerPixel
struct.pack_into("<I", hdr, 392, W)    # BytesPerLine
struct.pack_into("<I", hdr, 400, 0)    # ColorSpace = W (grey)
struct.pack_into("<I", hdr, 420, 1)    # NumColors
hdr[1732:1736] = b"A6\x00\x00"
open(sys.argv[1], "wb").write(b"3SaR" + bytes(hdr) + b"\xff" * (W * H))
PY

# run <label> <expect-rc-zero|expect-fail> <env-assignments...>
run() {
    label=$1; expect=$2; shift 2
    env "$@" "$BIN" 1 check t 1 "$OPTS" < "$T/in.ras" > "$T/out" 2> "$T/err"
    rc=$?
    n=$(wc -c < "$T/out")
    if [ "$expect" = fail ]; then
        if [ $rc -eq 0 ]; then echo "FAIL $label: expected non-zero exit, got 0"; fail=1; return; fi
        if [ "$n" -ne 0 ]; then echo "FAIL $label: wrote $n B to stdout before failing"; fail=1; return; fi
        echo "PASS $label (exit $rc, 0 B on stdout: $(tr -d '\n' < "$T/err" | cut -c1-72))"
    else
        if [ $rc -ne 0 ]; then echo "FAIL $label: expected success, got exit $rc: $(head -1 "$T/err")"; fail=1; return; fi
        if [ "$n" -eq 0 ]; then echo "FAIL $label: empty stdout"; fail=1; return; fi
        echo "PASS $label (exit 0, $n B on stdout)"
    fi
}

: > "$T/garbage.hft"; printf 'not a halftone screen at all' > "$T/garbage.hft"

if [ -r "$HFT" ]; then
    run "healthy run"                     ok   KONICA_HFT="$HFT"
else
    echo "SKIP healthy run / flat-tone hatch: no halftone screen at $HFT"
fi

run "missing halftone screen"             fail KONICA_HFT=/nonexistent.hft
run "corrupt halftone screen"             fail KONICA_HFT="$T/garbage.hft"
if [ -r "$HFT" ]; then
    run "unbuildable colour table"        fail KONICA_HFT="$HFT" KONICA_SRGB=/nonexistent.icm
    run "KONICA_FLAT_TONE=1 escape hatch" ok   KONICA_HFT="$HFT" KONICA_SRGB=/nonexistent.icm KONICA_FLAT_TONE=1
fi

# Not a raster at all: rejected before anything else happens.
printf 'this is not a CUPS raster stream' > "$T/bogus"
env "$BIN" 1 check t 1 "$OPTS" < "$T/bogus" > "$T/out" 2> "$T/err"
rc=$?
if [ $rc -ne 0 ] && [ ! -s "$T/out" ]; then echo "PASS not-a-raster (exit $rc, 0 B on stdout)"
else echo "FAIL not-a-raster: exit $rc, $(wc -c < "$T/out") B on stdout"; fail=1
fi

[ $fail -eq 0 ] && echo "check-errors: all cases pass" || echo "check-errors: FAILED"
exit $fail
