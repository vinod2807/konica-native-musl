#!/bin/sh
# Randomised differential fuzzer: our filter against the vendor's, on generated rasters.
#
# The corpus is fixed, so it can only ever test the cases somebody thought of.  This drives
# the same comparison with rasters that vary along the axes where the defects actually lived:
# raster version (v1/v2/v3), the page size in points versus the page-size *name*, image
# dimensions that disagree with the points, 300/600/1200 dpi, 1- and 8-bit pixels, 1-3 pages
# of mixed geometry, and page content that leaves long runs of bare paper.
#
#   tests/fuzz-difftest.sh [cases] [seed]
#
# Every case is a pure function of its seed (tools/mkras.py --random), so a failure is
# reproducible: the failing raster and both streams are copied to $KEEP (default
# ./fuzz-failures) and the seed is printed.
#
# A case whose vendor output moves under MALLOC_PERTURB_ is reported separately: the vendor
# reads uninitialised heap there, so no filter can reproduce it byte-for-byte (see the note
# on vendor_nondet() below).  A divergent 1bpp case whose raster is wider than the image is
# likewise reported separately -- that is a known vendor defect (see known_1bpp_defect()).
#
# Needs the vendor filter, unlike the rest of the tests.  Never writes to a printer.
set -u
CASES=${1:-50}
BASE=${2:-1000}
HERE=$(cd "$(dirname "$0")/.." && pwd)
NATIVE=${NATIVE:-$HERE/konica-native}
VENDOR=${VENDOR:-/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf}
LIBDIR=${LIBDIR:-/usr/local/lib/konica/lib}
KEEP=${KEEP:-$HERE/fuzz-failures}
[ -x "$NATIVE" ] || { echo "SKIP fuzz: $NATIVE not built (run make)"; exit 2; }
[ -x "$VENDOR" ] || { echo "SKIP fuzz: vendor filter $VENDOR not installed"; exit 2; }

# The vendor resolves MediaType/PageSize through the PPD; without one it exits 5 on every
# job.  Our filter ignores the PPD entirely, so this is only needed for the oracle.
if [ -z "${PPD:-}" ]; then
    for p in /etc/cups/ppd/KONICA_MINOLTA_206.ppd \
             /var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd; do
        [ -r "$p" ] && { PPD=$p; break; }
    done
fi
[ -n "${PPD:-}" ] && [ -r "$PPD" ] || {
    echo "SKIP fuzz: no readable PPD for the vendor filter (set PPD=...)"; exit 2; }

export PPD CONTENT_TYPE=application/vnd.cups-raster FINAL_CONTENT_TYPE=application/vnd.cups-raster \
       CUPS_SERVERROOT=${CUPS_SERVERROOT:-/etc/cups} CUPS_DATADIR=${CUPS_DATADIR:-/usr/share/cups}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

save() {                                  # save() <seed> <opts> <desc>
    mkdir -p "$KEEP"
    cp "$T/in.ras" "$KEEP/seed-$1.ras"
    cp "$T/v.prn" "$KEEP/seed-$1.vendor.prn" 2>/dev/null
    cp "$T/n.prn" "$KEEP/seed-$1.native.prn" 2>/dev/null
    printf 'seed=%s\t%s\t%s\n' "$1" "$2" "$3" >> "$KEEP/cases.tsv"
    saved=$((saved + 1))
}

# The vendor reads uninitialised heap in two places: the BIE alignment pad bytes, and -- when
# a raster row is not a multiple of 4 bytes -- the byte just past the row, which the CMM's
# right-edge window reaches.  Where it does, the vendor's own output changes with the
# allocator's fill pattern (MALLOC_PERTURB_), so it is not a usable oracle for that case:
# re-run it under a perturbed heap and, if the bytes move, call the case nondeterministic
# rather than divergent.  Ours is deterministic by construction (tests/check-determinism.sh).
vendor_nondet() {
    MALLOC_PERTURB_=85 LD_LIBRARY_PATH=$LIBDIR "$VENDOR" 1 fuzz t 1 "$opts" \
        < "$T/in.ras" > "$T/v2.prn" 2>/dev/null
    ! cmp -s "$T/v.prn" "$T/v2.prn"
}

# The vendor's 1bpp path is a bit-for-bit copy -- but only while no raster row carries ink
# in the two bits at or just past the image width (x == Xd or Xd+1).  When one does (which
# needs a raster wider than the image) the vendor emits an unexplained, deterministic,
# halftone-like pattern instead: a single such bit gives 14 %-64 % ink where the copy would
# be one pixel, and a full ink row 93 %.
# That is a vendor defect tied to its own buffering, and it cannot arise for this device
# (the PPD declares cupsBitsPerColor 8, so CUPS only ever sends 8-bit rasters), so a
# divergent 1bpp case with a raster wider than the image is reported as a known defect
# rather than as our divergence.  Xd is computed with the same point rule the filter uses.
known_1bpp_defect() {
    python3 -I - "$1" <<'PY'
import sys
for page in sys.argv[1].split("; "):
    tok = page.split()
    if "1bpp" not in tok or "points=" not in page:
        continue
    w = int(tok[1].split("x")[0])
    p = int(page.split("points=")[1].split("x")[0])
    if w > p * 25 // 3 - (1 if p % 9 in (1, 3, 6) else 0):
        sys.exit(0)
sys.exit(1)
PY
}

pass=0; fail=0; skip=0; nondet=0; known=0; saved=0
i=0
while [ $i -lt "$CASES" ]; do
    i=$((i + 1))
    seed=$((BASE + i))
    python3 -I "$HERE/tools/mkras.py" --random --seed "$seed" \
            --pages $((seed % 3 + 1)) --meta "$T/meta" -o "$T/in.ras" > /dev/null 2> "$T/mk.err" || {
        echo "SKIP seed $seed: mkras.py failed: $(head -1 "$T/mk.err")"; skip=$((skip + 1)); continue; }
    IFS='	' read -r _ opts desc < "$T/meta"

    LD_LIBRARY_PATH=$LIBDIR "$VENDOR" 1 fuzz t 1 "$opts" < "$T/in.ras" > "$T/v.prn" 2> "$T/v.err"
    vrc=$?
    "$NATIVE" 1 fuzz t 1 "$opts" < "$T/in.ras" > "$T/n.prn" 2> "$T/n.err"
    nrc=$?

    if [ $vrc -ne 0 ] || [ ! -s "$T/v.prn" ]; then
        if [ $nrc -ne 0 ]; then
            skip=$((skip + 1))                       # both reject it: not our divergence
        else
            fail=$((fail + 1))
            echo "FAIL seed $seed: vendor rejected it (exit $vrc) but we produced $(stat -c%s "$T/n.prn") B"
            echo "     $desc"
            save "$seed" "$opts" "$desc"
        fi
    elif [ $nrc -ne 0 ]; then
        fail=$((fail + 1))
        echo "FAIL seed $seed: we exited $nrc, vendor produced $(stat -c%s "$T/v.prn") B"
        echo "     $desc"
        sed -n '1p' "$T/n.err" | sed 's/^/     /'
        save "$seed" "$opts" "$desc"
    elif verdict=$(python3 -I "$HERE/tools/prnscan.py" padcmp "$T/n.prn" "$T/v.prn"); then
        pass=$((pass + 1))
        case $verdict in PASS*alignment*) echo "seed $seed: $(echo "$verdict" | head -1) [$desc]";; esac
    else
        if known_1bpp_defect "$desc"; then
            known=$((known + 1))
            echo "seed $seed: known vendor 1bpp defect (raster wider than the image, ink past Xd) -- not comparable"
            echo "     $desc"
        elif vendor_nondet; then
            nondet=$((nondet + 1))
            echo "seed $seed: vendor output is nondeterministic (uninitialised heap) -- not comparable"
            echo "     $desc"
            save "$seed" "$opts" "$desc"
        else
            fail=$((fail + 1))
            echo "FAIL seed $seed: $verdict"
            echo "     $desc"
            save "$seed" "$opts" "$desc"
        fi
    fi
done

echo "----"
echo "fuzz: $pass identical, $fail divergent, $nondet vendor-nondeterministic, $known known vendor 1bpp defect, $skip skipped (of $CASES cases, seeds $((BASE + 1))..$((BASE + CASES)))"
if [ $fail -ne 0 ]; then
    [ $saved -ne 0 ] && echo "$saved failing case(s) copied to $KEEP (re-run with the printed seed)"
    exit 1
fi
exit 0
