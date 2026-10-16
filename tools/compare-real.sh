#!/bin/bash
# compare-real.sh — run the VENDOR filter and konica-native on the SAME raster made from a
# real PDF, and compare the two outputs byte for byte. Run on the Ubuntu host where the
# vendor filter works. No printer is involved: all output goes to files.
#
#   tools/compare-real.sh file.pdf /path/to/206-fullbleed.ppd [outdir]
#
# Save this file in the repo's tools/ directory (it uses tools/prnscan.py and ../konica-native).
# The repo's own `make check-real` does the same comparison but keeps nothing; this script
# keeps the rasters so another machine (e.g. Alpine) can be checked against the same vendor output.
#
# PASS means byte-identical OR differing only in the 0-3 never-decoded BIE pad bytes, which the
# vendor fills with uninitialised heap memory (docs/ALPINE-PORT-STATUS.md section 7.2).
#
# Env: OPTS    filter options (default: A4 simplex, as in the corpus)
#      NATIVE  path to konica-native (default <repo>/konica-native)
#      VENDOR  vendor filter, LIBDIR its private libs
#      NAME    output stem (default: PDF basename, plus a slug of OPTS when they are non-default)
# Leaves outdir/<name>.{ras,opts,vendor.prn,native.prn} so the same raster can be re-checked
# on another machine with verify-on-target.sh.
#
# The step-2 matrix runs the same PDF twice with different Duplex options; a bare basename would
# make the second run silently overwrite the first run's captured vendor output, so non-default
# options are folded into the stem (e.g. twopage.PageSize-A4-Duplex-DuplexTumble-sides-...).
set -u
PDF=${1:?usage: $0 file.pdf /path/to/206.ppd [outdir]}
PPD_FILE=${2:?usage: $0 file.pdf /path/to/206.ppd [outdir]}
OUT=${3:-./real-compare}
HERE=$(cd "$(dirname "$0")" && pwd)
NATIVE=${NATIVE:-$HERE/../konica-native}
VENDOR=${VENDOR:-/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf}
LIBDIR=${LIBDIR:-/usr/local/lib/konica/lib}
PDFTOPDF=${PDFTOPDF:-/usr/lib/cups/filter/pdftopdf}
PDFTORASTER=${PDFTORASTER:-/usr/lib/cups/filter/pdftoraster}
DEFAULT_OPTS='PageSize=A4 media=A4 Duplex=None sides=one-sided Duplexer=true MediaType=Plain back-side-orientation=Rotated'
OPTS=${OPTS:-$DEFAULT_OPTS}

for f in "$PDF" "$PPD_FILE" "$NATIVE" "$VENDOR" "$PDFTOPDF" "$PDFTORASTER"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 2; }
done
mkdir -p "$OUT"
name=$(basename "$PDF" .pdf)
if [ -n "${NAME:-}" ]; then
    name=$NAME
elif [ "$OPTS" != "$DEFAULT_OPTS" ]; then
    slug=$(printf '%s\n' "$OPTS" | tr ' ' '\n' \
           | grep -E '^(PageSize|Duplex|sides|MediaType|Resolution|OCM_TonerSave)=' \
           | tr -cs 'A-Za-z0-9' '-' | sed 's/^-//; s/-$//')
    [ -n "$slug" ] && name="$name.$slug"
fi
export PPD="$PPD_FILE" CONTENT_TYPE=application/pdf FINAL_CONTENT_TYPE=application/vnd.cups-raster \
       CUPS_SERVERROOT=/etc/cups CUPS_DATADIR=/usr/share/cups

"$PDFTOPDF" 1 cmp t 1 "$OPTS" "$PDF" > "$OUT/$name.pdf2" 2> "$OUT/$name.err" \
    || { echo "FAIL $name: pdftopdf failed (see $OUT/$name.err)"; exit 2; }
"$PDFTORASTER" 1 cmp t 1 "$OPTS" < "$OUT/$name.pdf2" > "$OUT/$name.ras" 2>> "$OUT/$name.err" \
    || { echo "FAIL $name: pdftoraster failed (see $OUT/$name.err)"; exit 2; }
[ -s "$OUT/$name.ras" ] || { echo "FAIL $name: empty raster"; exit 2; }
printf '%s\n' "$OPTS" > "$OUT/$name.opts"

LD_LIBRARY_PATH=$LIBDIR "$VENDOR" 1 cmp t 1 "$OPTS" < "$OUT/$name.ras" > "$OUT/$name.vendor.prn" 2>> "$OUT/$name.err"
vrc=$?
"$NATIVE" 1 cmp t 1 "$OPTS" < "$OUT/$name.ras" > "$OUT/$name.native.prn" 2>> "$OUT/$name.err"
nrc=$?

echo "$name: vendor exit=$vrc $(stat -c%s "$OUT/$name.vendor.prn") B | native exit=$nrc $(stat -c%s "$OUT/$name.native.prn") B"
if [ "$vrc" -ne 0 ] || [ "$nrc" -ne 0 ] || [ ! -s "$OUT/$name.vendor.prn" ]; then
    echo "FAIL $name (a filter exited non-zero or produced nothing; see $OUT/$name.err)"; exit 1
fi
if cmp -s "$OUT/$name.vendor.prn" "$OUT/$name.native.prn"; then
    echo "PASS $name (byte-identical)"
elif v=$(python3 -I "$HERE/prnscan.py" padcmp "$OUT/$name.native.prn" "$OUT/$name.vendor.prn"); then
    echo "PASS $name ($(echo "$v" | head -1))"
else
    echo "FAIL $name: $(echo "$v" | head -1)"
    python3 -I "$HERE/prnscan.py" diff "$OUT/$name.native.prn" "$OUT/$name.vendor.prn" -n 5
    exit 1
fi
