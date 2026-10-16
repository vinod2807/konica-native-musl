#!/bin/sh
# Real-document test: exercise the kit on a *real* CUPS raster, not a synthetic one.
#
# The 60-case corpus is built with cupsRasterInterpretPPD(), so its headers are always the
# shape mkraster produces.  Real CUPS output is not: pdftoraster writes a v3
# (uncompressed) raster whose PageSizeName comes from the job, not the PPD -- an A4 page
# arrives as 4961x7016 calling itself "A4.Borderless".  That difference hid a
# geometry-selection defect and it also produces BIE lengths that are not a multiple of 4,
# which the corpus never does.  This test runs the real chain and diffs both filters.
#
#   tests/real-docs.sh [file.pdf ...]
#
# With no arguments it generates one with tests/mkpdf.py, so nothing private or
# third-party has to live in the repository.  Pass your own PDFs (they are not copied
# anywhere) to widen the test locally.
#
# Environment: PPD VENDOR LIBDIR PDFTOPDF PDFTORASTER OPTS NATIVE (all optional).
# Exit: 0 = every document matches, 1 = a document differs, 2 = prerequisites missing.
#
# Nothing is ever sent to a printer: both filters write to files in a temp directory.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
NATIVE=${NATIVE:-$HERE/konica-native}
VENDOR=${VENDOR:-/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf}
LIBDIR=${LIBDIR:-/usr/local/lib/konica/lib}
PDFTOPDF=${PDFTOPDF:-/usr/lib/cups/filter/pdftopdf}
PDFTORASTER=${PDFTORASTER:-/usr/lib/cups/filter/pdftoraster}
OPTS=${OPTS:-PageSize=A4 media=A4 Duplex=None sides=one-sided Duplexer=true MediaType=Plain back-side-orientation=Rotated}

# The PPD has to be the vendor's: it is what makes pdftoraster emit the same header a real
# queue would.  Try the usual places when PPD is not set.
if [ -z "${PPD:-}" ]; then
    for p in /etc/cups/ppd/KONICA_MINOLTA_206.ppd \
             /var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd; do
        [ -r "$p" ] && { PPD=$p; break; }
    done
fi

skip() { echo "SKIP real-docs: $1"; exit 2; }
[ -x "$NATIVE" ]    || skip "$NATIVE not built (run make)"
[ -x "$VENDOR" ]    || skip "vendor filter $VENDOR not installed"
[ -x "$PDFTOPDF" ]  || skip "$PDFTOPDF not installed (cups-filters)"
[ -x "$PDFTORASTER" ] || skip "$PDFTORASTER not installed (cups-filters)"
[ -n "${PPD:-}" ] && [ -r "$PPD" ] || skip "no readable PPD (set PPD=/path/to/vendor.ppd)"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

if [ $# -eq 0 ]; then
    python3 -I "$HERE/tests/mkpdf.py" "$T/generated.pdf" || exit 2
    set -- "$T/generated.pdf"
fi

export PPD CONTENT_TYPE=application/pdf FINAL_CONTENT_TYPE=application/vnd.cups-raster \
       CUPS_SERVERROOT=${CUPS_SERVERROOT:-/etc/cups} CUPS_DATADIR=${CUPS_DATADIR:-/usr/share/cups}

fail=0
for pdf in "$@"; do
    [ -r "$pdf" ] || { echo "FAIL real-docs: no such file: $pdf"; fail=1; continue; }
    name=$(basename "$pdf" .pdf)
    "$PDFTOPDF" 1 cmp t 1 "$OPTS" "$pdf" > "$T/$name.pdf2" 2> "$T/$name.err" \
        || { echo "SKIP real-docs: $name: pdftopdf failed"; continue; }
    "$PDFTORASTER" 1 cmp t 1 "$OPTS" < "$T/$name.pdf2" > "$T/$name.ras" 2>> "$T/$name.err" \
        || { echo "SKIP real-docs: $name: pdftoraster failed"; continue; }
    [ -s "$T/$name.ras" ] || { echo "SKIP real-docs: $name: empty raster"; continue; }

    LD_LIBRARY_PATH=$LIBDIR "$VENDOR" 1 cmp t 1 "$OPTS" < "$T/$name.ras" > "$T/$name.vendor.prn" 2>> "$T/$name.err"
    vrc=$?
    "$NATIVE" 1 cmp t 1 "$OPTS" < "$T/$name.ras" > "$T/$name.native.prn" 2>> "$T/$name.err"
    nrc=$?
    if [ $vrc -ne 0 ] || [ $nrc -ne 0 ]; then
        echo "FAIL real-docs: $name: vendor exit $vrc, native exit $nrc"; fail=1; continue
    fi

    pages=$("$NATIVE" --dump "$T/$name.ras" 2>/dev/null | wc -l)
    verdict=$(python3 -I "$HERE/tools/prnscan.py" padcmp "$T/$name.native.prn" "$T/$name.vendor.prn" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        printf 'PASS %-18s %s  [%s page(s)]\n' "$name" "$(echo "$verdict" | head -1)" "$pages"
    else
        printf 'FAIL %-18s %s\n' "$name" "$(echo "$verdict" | head -1)"
        fail=1
        python3 -I "$HERE/tools/prnscan.py" diff "$T/$name.native.prn" "$T/$name.vendor.prn" -n 3
    fi
    echo "$verdict" | tail -n +2 | sed 's/^/     /'
done

[ $fail -eq 0 ] && echo "real-docs: all documents match" || echo "real-docs: FAILED"
exit $fail
