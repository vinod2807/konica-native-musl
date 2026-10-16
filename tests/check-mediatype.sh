#!/bin/sh
# PJL MEDIATYPE regression test (no vendor filter needed).
#
# The vendor takes MEDIATYPE from the MediaType *string in the raster header* -- which is
# where CUPS puts the job's media type -- and ignores the MediaType= job option.  It maps
# the twelve spellings the PPD offers, matched case-insensitively, and falls back to PLAIN
# for anything else (empty, "Auto", "Transparencies", "transparency film", ...).
#
# The table below is a golden one: every row was checked against the running vendor filter
# on 2026-10-16 by rewriting the raster header's MediaType field (see
# docs/DEVICE-STREAM.md).  The test itself only needs the kit.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MK=${MKRAS:-$HERE/tools/mkras.py}

# Encoding needs the vendor's halftone screen and ICC profiles at run time (by design:
# nothing is vendored into the kit).  Skip cleanly when they are not installed.
HFT=${KONICA_HFT:-/usr/local/lib/konica/KonicaMinolta/245igdi/Halftones/COPA.DEFAULT.600x600.hft}
SRGB=${KONICA_SRGB:-/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/sRGB.icm}
GRAY=${KONICA_GRAY:-/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/sGrayInv.icm}
BIN=${NATIVE:-$HERE/konica-native}
for f in "$HFT" "$SRGB" "$GRAY"; do
    [ -r "$f" ] || { echo "SKIP check-mediatype: $f not readable (set KONICA_HFT/KONICA_SRGB/KONICA_GRAY)"; exit 0; }
done

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
# The job option says Transparency throughout: the vendor must ignore it and use the
# header's string instead, so a row that comes back TRANSPARENCY can only have come from
# the raster header.
OPTS='PageSize=A4 media=A4 Duplex=None sides=one-sided Duplexer=true MediaType=Transparency back-side-orientation=Rotated'
fail=0

mkraster() { # <out> <media-string>  (the string is written verbatim, empty included)
    python3 -I "$MK" --version 3 --size 4960x7016 --points 595x842 --name A4 -P blank -o "$1" > /dev/null
    python3 -I - "$1" "$2" <<'PY'
import sys
d = bytearray(open(sys.argv[1], "rb").read())
d[132:196] = b"\0" * 64                      # MediaType (file offset 4 + 128)
m = sys.argv[2].encode()[:63]
d[132:132 + len(m)] = m
open(sys.argv[1], "wb").write(d)
PY
}

check() { # <label> <media-string> <expected-code>
    mkraster "$T/in.ras" "$2"
    "$BIN" 1 mediatest t 1 "$OPTS" < "$T/in.ras" > "$T/out.prn" 2> "$T/err"
    if [ $? -ne 0 ]; then
        echo "FAIL $1: filter exited non-zero ($(head -1 "$T/err"))"; fail=1; return
    fi
    got=$(grep -a -o 'MEDIATYPE=[A-Z]*' "$T/out.prn" | head -1 | cut -d= -f2)
    if [ "$got" = "$3" ]; then echo "PASS $1"
    else echo "FAIL $1: header MediaType '$2' -> MEDIATYPE=$got, want $3"; fail=1
    fi
}

# The PPD's twelve MediaType choices.
check "Plain"            "Plain"          PLAIN
check "Recycle"          "Recycle"        RECYCLE
check "1Side"            "1Side"          ONESIDE
check "Special"          "Special"        SPECIAL
check "Transparency"     "Transparency"   TRANSPARENCY
check "Card1"            "Card1"          CARDONE
check "Card2"            "Card2"          CARDTWO
check "Envelope"         "Envelope"       ENVELOPE
check "Plains"           "Plains"         PLAINS
check "Card1s"           "Card1s"         CARDONES
check "Card2s"           "Card2s"         CARDTWOS
check "Recycle2side"     "Recycle2side"   RECYCLES

# Matching ignores case.
check "plain (lower case)"       "plain"          PLAIN
check "TRANSPARENCY (upper case)" "TRANSPARENCY"  TRANSPARENCY
check "recycle2side (mixed case)" "recycle2side"  RECYCLES

# Anything else falls back to PLAIN -- note "Transparencies" and "transparency film" do
# not match "Transparency": the vendor compares whole strings, not prefixes.
check "empty"                    ""                     PLAIN
check "Auto"                     "Auto"                 PLAIN
check "Foo"                      "Foo"                  PLAIN
check "Transparencies"           "Transparencies"       PLAIN
check "transparency film"        "transparency film"    PLAIN

if [ $fail -ne 0 ]; then echo "check-mediatype: FAILED"; exit 1; fi
echo "check-mediatype: all cases pass"
