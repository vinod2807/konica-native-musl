#!/bin/sh
# Cross-check our libc-only raster reader against libcups on synthetic rasters.
# Needs: ./konica-native, tests/libcups_dump (make check-reader builds both), tools/mkraster.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MKR=${MKRASTER:-$HERE/tools/mkraster}
[ -x "$MKR" ] || cc -O2 -Wno-deprecated-declarations -o "$MKR" "$HERE/tools/mkraster.c" -lcups || exit 2
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
BIN=${NATIVE:-$HERE/konica-native}

# Rewrite an uncompressed (v3) stream as v1: "RaSt" sync, 420-byte headers, same pixels.
to_v1() {
python3 -I - "$1" "$2" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:4] == b"3SaR", "to_v1 needs a little-endian v3 stream"
out, pos = bytearray(b"tSaR"), 4
while pos < len(d):
    hdr = d[pos:pos + 1796]
    h, bpl = struct.unpack_from("<I", hdr, 376)[0], struct.unpack_from("<I", hdr, 392)[0]
    out += hdr[:420] + d[pos + 1796:pos + 1796 + h * bpl]
    pos += 1796 + h * bpl
open(sys.argv[2], "wb").write(out)
PY
}
t() { # name, mkraster args...
    name=$1; shift
    "$MKR" "$@" > "$T/in.ras" 2>/dev/null || { echo "SKIP $name (mkraster)"; return; }
    case $name in *-v1) to_v1 "$T/in.ras" "$T/v1.ras" && mv "$T/v1.ras" "$T/in.ras" ;; esac
    "$BIN" --dump "$T/in.ras" > "$T/ours" 2>&1
    "$HERE/tests/libcups_dump" "$T/in.ras" > "$T/ref" 2>&1
    if cmp -s "$T/ours" "$T/ref" && [ -s "$T/ref" ]; then echo "PASS $name"; else
        echo "FAIL $name"; diff "$T/ours" "$T/ref" | head -4; fail=1; fi
}
for z in "" "-z" "v1"; do
    l=${z:-v3}; [ "$z" = "-z" ] && l=v2rle; [ "$z" = "v1" ] && { l=v1; z=; }
    t "gray8-solid-$l"    $z -w 301 -h 97 -b 8 -c w -P solid
    t "gray8-noise-$l"    $z -w 301 -h 97 -b 8 -c w -P noise:7
    t "gray8-hgrad-$l"    $z -w 640 -h 480 -b 8 -c w -P hgrad
    t "gray8-3pg-$l"      $z -w 128 -h 64 -b 8 -c w -P solid,vgrad,checker:3
    t "gray1-checker-$l"  $z -w 203 -h 77 -b 1 -c k -P checker:1
    t "gray1-lines-$l"    $z -w 1000 -h 300 -b 1 -c w -P hlines:3,vlines:5
    t "rgb8-noise-$l"     $z -w 100 -h 50 -b 8 -c rgb -P noise:3
    t "rgb8-blank-$l"     $z -w 100 -h 50 -b 8 -c rgb -P blank
    t "cmyk8-grad-$l"     $z -w 100 -h 50 -b 8 -c cmyk -P hgrad
done
exit $fail
