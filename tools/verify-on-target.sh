#!/bin/sh
# verify-on-target.sh — on ANY machine (e.g. the Alpine box): run konica-native on every
# raster that compare-real.sh produced and compare with the vendor output captured on Ubuntu.
#
#   tools/verify-on-target.sh /path/to/real-compare [path/to/konica-native]
#
# Save in the repo's tools/ directory. A case passes when byte-identical or when it differs only
# in the never-decoded BIE pad bytes (prnscan.py padcmp).
set -u
DIR=${1:?usage: $0 real-compare-dir [konica-native]}
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${2:-$HERE/../konica-native}
[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }
pass=0; fail=0; n=0
for ras in "$DIR"/*.ras; do
    [ -e "$ras" ] || { echo "no .ras files in $DIR" >&2; exit 2; }
    name=$(basename "$ras" .ras); n=$((n+1))
    opts=$(cat "$DIR/$name.opts")
    v=
    "$BIN" 1 verify t 1 "$opts" < "$ras" > /tmp/verify.$$.prn 2>/tmp/verify.$$.err
    rc=$?
    if [ $rc -eq 0 ] && cmp -s /tmp/verify.$$.prn "$DIR/$name.vendor.prn"; then
        echo "PASS $name (byte-identical)"; pass=$((pass+1))
    elif [ $rc -eq 0 ] && v=$(python3 -I "$HERE/prnscan.py" padcmp /tmp/verify.$$.prn "$DIR/$name.vendor.prn"); then
        echo "PASS $name ($(echo "$v" | head -1))"; pass=$((pass+1))
    else
        echo "FAIL $name  (${v:-$(sed -n 1p /tmp/verify.$$.err)})"; fail=$((fail+1))
    fi
done
rm -f /tmp/verify.$$.prn /tmp/verify.$$.err
echo "PASS: $pass   FAIL: $fail   (of $n)"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
