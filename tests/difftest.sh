#!/bin/sh
# Differential test: run the candidate filter on every captured raster and compare its
# output with the vendor filter's captured output, byte for byte.
#
#   tests/difftest.sh /path/to/konica-corpus [candidate-binary]
#
# Prints PASS/FAIL per case and, for failures, the first differing range (via prnscan).
# Cases marked deterministic=no come with corpus/<id>.mask (byte ranges that differed between
# two vendor runs, written by capture-corpus.sh). Those ranges are ignored in the comparison and
# the case reports PASS(masked). A masked PASS proves everything *else* matches; the masked
# bytes (timestamps/counters) still need their own explanation before the filter is complete.
# Variable-length vendor output cannot be masked byte-wise and stays FAIL.
set -u
DIR=${1:?usage: $0 corpus-dir [candidate]}
CAND=${2:-$(dirname "$0")/../konica-native}
SCAN=$(dirname "$0")/../tools/prnscan.py
MAN=$DIR/manifest.tsv
[ -r "$MAN" ] || { echo "no manifest at $MAN" >&2; exit 2; }
[ -x "$CAND" ] || { echo "candidate not built: $CAND (run make)" >&2; exit 2; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
tail -n +2 "$MAN" | while IFS='	' read -r id opts pats pages rasb prnb rc det rsha psha; do
    ras=$DIR/corpus/$id.ras; ref=$DIR/corpus/$id.prn
    [ -r "$ras" ] && [ -r "$ref" ] || continue
    "$CAND" 1 difftest "$id" 1 "$opts" < "$ras" > "$T/out.prn" 2> "$T/err"
    mask=$DIR/corpus/$id.mask
    if cmp -s "$T/out.prn" "$ref"; then echo "PASS $id"
    elif [ -s "$mask" ] && m=$(python3 -I "$SCAN" cmpmask "$T/out.prn" "$ref" "$mask"); then
        echo "PASS(masked) $id  [$m]"
    else
        echo "FAIL $id  (ours $(stat -c%s "$T/out.prn") B, vendor $(stat -c%s "$ref") B, deterministic=$det)"
        [ -s "$T/err" ] && sed -n '1p' "$T/err" | sed 's/^/     /'
        [ -s "$T/out.prn" ] && python3 -I "$SCAN" diff "$T/out.prn" "$ref" -n 3 | sed 's/^/     /'
    fi
done | tee "$T/log"
echo "----"
echo "PASS: $(grep -c '^PASS' "$T/log") (of which masked: $(grep -c '^PASS(masked)' "$T/log"))   FAIL: $(grep -c '^FAIL' "$T/log")"
