#!/bin/sh
# Determinism test: the same raster must always produce the same device stream.
#
# Run the candidate over a corpus twice -- normally, and with MALLOC_PERTURB_ set so that
# glibc fills freshly allocated memory with a known non-zero pattern -- and require both
# runs to be byte-identical.  Any read of uninitialised memory shows up as a difference.
#
# This is not a hypothetical: the vendor filter's BIE alignment pad bytes *are* uninitialised
# heap, so two vendor runs of the same page can differ (see src/jbig.h and docs/DEVICE-STREAM.md).
# The kit deliberately zeroes those bytes, and this test is what keeps it that way.
#
#   tests/check-determinism.sh /path/to/konica-corpus [candidate-binary]
#
# Needs only the kit and a corpus; no vendor filter and no libcups.
set -u
DIR=${1:?usage: $0 corpus-dir [candidate]}
CAND=${2:-$(dirname "$0")/../konica-native}
SCAN=$(dirname "$0")/../tools/prnscan.py
MAN=$DIR/manifest.tsv
[ -r "$MAN" ] || { echo "SKIP check-determinism: no manifest at $MAN" >&2; exit 2; }
[ -x "$CAND" ] || { echo "SKIP check-determinism: $CAND not built (run make)" >&2; exit 2; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
n=0; bad=0
tail -n +2 "$MAN" > "$T/rows"
: > "$T/log"
# The loop reads from a file, not a pipe, so the counters survive in this shell.
while IFS='	' read -r id opts pats pages rasb prnb rc det rsha psha; do
    ras=$DIR/corpus/$id.ras
    [ -r "$ras" ] || continue
    n=$((n + 1))
    "$CAND" 1 dettest "$id" 1 "$opts" < "$ras" > "$T/a.prn" 2> "$T/a.err"
    ra=$?
    # 0x55 is a byte the encoder never writes as padding itself, so a perturbed allocation
    # that leaks into the stream is immediately visible.
    MALLOC_PERTURB_=85 "$CAND" 1 dettest "$id" 1 "$opts" < "$ras" > "$T/b.prn" 2> "$T/b.err"
    rb=$?
    if [ $ra -ne 0 ] || [ $rb -ne 0 ]; then
        bad=$((bad + 1))
        { echo "FAIL $id  (exit $ra / $rb)"
          sed -n '1p' "$T/a.err" "$T/b.err"; } >> "$T/log"
    elif ! cmp -s "$T/a.prn" "$T/b.prn"; then
        bad=$((bad + 1))
        { echo "FAIL $id  (nondeterministic: $(stat -c%s "$T/a.prn") B vs $(stat -c%s "$T/b.prn") B)"
          python3 -I "$SCAN" diff "$T/a.prn" "$T/b.prn" -n 3; } >> "$T/log"
    fi
done < "$T/rows"

[ -s "$T/log" ] && sed 's/^/     /' "$T/log"
echo "determinism: $n case(s) checked, $bad failure(s)"
[ $bad -eq 0 ] || exit 1
exit 0
