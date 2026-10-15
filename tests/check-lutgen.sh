#!/bin/sh
# Check the regenerated CMM 33x33x33 table against the vendor's.
#
#   tests/check-lutgen.sh [reference-35937-byte-dump]
#
# The reference is, in order of preference:
#   1. the file given as $1;
#   2. $KONICA_LUT, if set (a dump captured from the running vendor filter
#      with tools/dump-lut.gdb);
#   3. the recorded SHA-256 of the vendor table, which is only valid for the
#      vendor's own installed Profiles/sRGB.icm and Profiles/sGrayInv.icm.
#
# SKIPs (exit 0) when the profiles are not installed, so the test is usable on
# a machine that only has the source.  See docs/LUT-GENERATOR.md.
set -u
HERE=$(dirname "$0"); ROOT=$HERE/..
REF=${1:-${KONICA_LUT:-}}
EXPECT=84a424f5cccdbe7070b3f926fe9834b515db69391440ccac1dc5b046622c6d6f
DUMP=$ROOT/tools/lutgen-dump

[ -x "$DUMP" ] || cc -O2 -std=c11 -ffp-contract=off -I"$ROOT/src" -o "$DUMP" \
    "$ROOT/tools/lutgen-dump.c" "$ROOT/src/lutgen.c" -lm || exit 2

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if ! "$DUMP" > "$T/lut.bin" 2> "$T/err"; then
    echo "SKIP check-lutgen: $(sed -n 1p "$T/err")"
    exit 0
fi

n=$(wc -c < "$T/lut.bin")
[ "$n" = 35937 ] || { echo "FAIL check-lutgen: generated $n bytes, want 35937"; exit 1; }

if [ -n "$REF" ]; then
    [ -r "$REF" ] || { echo "SKIP check-lutgen: reference $REF not readable"; exit 0; }
    if cmp -s "$T/lut.bin" "$REF"; then
        echo "PASS check-lutgen: regenerated table is byte-identical to $REF"
        exit 0
    fi
    echo "FAIL check-lutgen: regenerated table differs from $REF"
    exit 1
fi

got=$(sha256sum "$T/lut.bin" | cut -d' ' -f1)
if [ "$got" = "$EXPECT" ]; then
    echo "PASS check-lutgen: sha256 $got (vendor table)"
    exit 0
fi
echo "FAIL check-lutgen: sha256 $got, want $EXPECT"
echo "      (installed profiles may differ from the vendor's;"
echo "       re-run with a captured reference: tests/check-lutgen.sh <dump>)"
exit 1
