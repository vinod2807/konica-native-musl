#!/bin/bash
# capture-corpus.sh — L0: build a (raster in -> device stream out) corpus from the
# WORKING vendor filter, so a replacement can be developed by differential testing.
#
# Run on the glibc host where the printer already works (your Ubuntu box).
# It NEVER talks to the printer: the vendor filter's stdout goes to a file, not to
# the USB backend. No paper is used.
#
# Usage:
#   ./capture-corpus.sh /path/to/206-fullbleed.ppd [outdir]
#
# Environment (all optional):
#   KONICA_FILTER   vendor filter  (default /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf)
#   KONICA_LIBDIR   its private libs, put on LD_LIBRARY_PATH (default /usr/local/lib/konica/lib)
#   BASE_OPTS       extra options appended to every case. Default mirrors what your job logs
#                   showed reaching the filter. For best fidelity, set LogLevel debug in
#                   cupsd.conf, print one real job, and paste its argv[5] here.
#   RERUNS=4        extra vendor runs per case, to find bytes that vary (timestamps/counters)
#   QUICK=1         only the core cases (about 25 runs instead of ~100)
#   RAS_UNCOMPRESSED=1  write uncompressed rasters (about 35 MB per A4 page at 600 dpi); default is RLE
#
# Output: <outdir>/corpus/NNN-name.{ras,prn,err} + manifest.tsv, and <outdir>/corpus.tar.gz

set -u
PPD_FILE=${1:?usage: $0 /path/to/206.ppd [outdir]}
OUT=${2:-./konica-corpus}
FILTER=${KONICA_FILTER:-/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf}
LIBDIR=${KONICA_LIBDIR:-/usr/local/lib/konica/lib}
# Note: Duplexer=true is a capability flag seen in real job logs; it coexists with the per-case
# Duplex=... value, so both appear in manifest.tsv. That is intentional, not a conflict.
BASE_OPTS=${BASE_OPTS:-Duplexer=true MediaType=Plain back-side-orientation=Rotated}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -r "$PPD_FILE" ] || { echo "cannot read PPD $PPD_FILE" >&2; exit 2; }
[ -x "$FILTER" ]   || { echo "vendor filter not found/executable: $FILTER" >&2; exit 2; }

mkdir -p "$OUT/corpus" "$OUT/bin"
MKR=$OUT/bin/mkraster
if [ ! -x "$MKR" ]; then
    echo "==> building mkraster"
    cc -O2 -Wall -Wno-deprecated-declarations -o "$MKR" "$HERE/mkraster.c" -lcups \
        || { echo "need libcups headers (Debian/Ubuntu: apt install libcups2-dev build-essential)" >&2; exit 2; }
fi

export PPD="$PPD_FILE" CONTENT_TYPE=application/vnd.cups-raster FINAL_CONTENT_TYPE=application/vnd.cups-raster
export CUPS_SERVERROOT=/etc/cups CUPS_DATADIR=/usr/share/cups

RERUNS=${RERUNS:-4}
ZFLAG=-z; [ "${RAS_UNCOMPRESSED:-0}" = 1 ] && ZFLAG=
MAN=$OUT/manifest.tsv
printf 'id\toptions\tpatterns\tpages\tras_bytes\tprn_bytes\texit\tdeterministic\tras_sha256\tprn_sha256\n' > "$MAN"
n=0; bad=0

# one_case NAME "OPTIONS" "PATTERNS"
one_case() {
    local name=$1 opts=$2 pats=$3
    n=$((n+1)); local id; id=$(printf '%03d-%s' "$n" "$name")
    local ras=$OUT/corpus/$id.ras prn=$OUT/corpus/$id.prn err=$OUT/corpus/$id.err
    local full="$opts $BASE_OPTS"
    if ! "$MKR" $ZFLAG -p "$PPD_FILE" -o "$full" -P "$pats" > "$ras" 2> "$err"; then
        echo "SKIP $id (mkraster failed: $(tail -1 "$err"))"; rm -f "$ras"; return
    fi
    LD_LIBRARY_PATH=$LIBDIR "$FILTER" 1 corpus "$id" 1 "$full" < "$ras" > "$prn" 2>> "$err"
    local rc=$?
    # determinism: rerun the vendor filter RERUNS more times and compare. Bytes that differ between
    # runs (timestamps, counters) go into <id>.mask (union over all reruns) for tests/difftest.sh.
    # A byte that happens to coincide across every rerun is missed, hence several reruns, not one.
    local det=yes r
    : > "$OUT/corpus/$id.mask.tmp"
    for r in $(seq 1 "$RERUNS"); do
        LD_LIBRARY_PATH=$LIBDIR "$FILTER" 1 corpus "$id" 1 "$full" < "$ras" > "$prn.rerun" 2>/dev/null
        if ! cmp -s "$prn" "$prn.rerun"; then
            det=no
            python3 -I "$HERE/prnscan.py" ranges "$prn" "$prn.rerun" >> "$OUT/corpus/$id.mask.tmp"
        fi
    done
    if [ "$det" = no ]; then mv "$OUT/corpus/$id.mask.tmp" "$OUT/corpus/$id.mask"
    else rm -f "$OUT/corpus/$id.mask.tmp"; fi
    rm -f "$prn.rerun"
    local pages; pages=$(awk -F, '{print NF}' <<< "$pats")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$full" "$pats" "$pages" \
        "$(stat -c%s "$ras")" "$(stat -c%s "$prn")" "$rc" "$det" \
        "$(sha256sum < "$ras" | cut -d' ' -f1)" "$(sha256sum < "$prn" | cut -d' ' -f1)" >> "$MAN"
    [ "$rc" -ne 0 ] || [ ! -s "$prn" ] && { bad=$((bad+1)); echo "WARN $id exit=$rc bytes=$(stat -c%s "$prn")"; }
    echo "ok   $id  prn=$(stat -c%s "$prn") B  deterministic=$det"
}

S1="Duplex=None sides=one-sided"
DL="Duplex=DuplexNoTumble sides=two-sided-long-edge"
DS="Duplex=DuplexTumble sides=two-sided-short-edge"
A4="PageSize=A4 media=A4"

# --- 1. simplex baseline and pixel-mapping probes (A4, PPD default resolution) ---
for pat in blank solid hgrad vgrad steps:8 rowid colid frame; do one_case "simplex-${pat//:/_}" "$A4 $S1" "$pat"; done
for pat in dot:0:0 dot:1:0 dot:0:1 dot:7:0 dot:8:0 dot:100:100 dot:101:100 box:50:50:8:8; do
    one_case "probe-${pat//:/_}" "$A4 $S1" "$pat"; done
for pat in hlines:2 hlines:4 hlines:16 vlines:2 vlines:4 vlines:16 checker:1 checker:2 checker:4 checker:8 noise:1 noise:2; do
    one_case "tex-${pat//:/_}" "$A4 $S1" "$pat"; done

# --- 2. duplex, page counts, back-side behaviour ---
one_case duplex-long-2pg  "$A4 $DL" "solid,hgrad"
one_case duplex-short-2pg "$A4 $DS" "solid,hgrad"
one_case duplex-long-3pg  "$A4 $DL" "solid,hgrad,vgrad"
one_case duplex-long-1pg  "$A4 $DL" "rowid"
one_case simplex-3pg      "$A4 $S1" "solid,hgrad,vgrad"

# --- 3. toner save and any other boolean/enum options present in the PPD ---
one_case tonersave-true  "$A4 $S1 OCM_TonerSave=TRUE"  "steps:8"
one_case tonersave-false "$A4 $S1 OCM_TonerSave=FALSE" "steps:8"

if [ "${QUICK:-0}" = 1 ]; then
    echo "QUICK=1: skipping resolutions and media sizes"
else
    # --- 4. resolutions the PPD offers (parsed from the PPD, nothing assumed) ---
    while read -r res; do
        one_case "res-$res" "$A4 $S1 Resolution=$res" "dot:100:100,hlines:4"
    done < <(grep -E '^\*Resolution [^/]+/' "$PPD_FILE" | sed -E 's/^\*Resolution ([^/]+)\/.*/\1/' | sort -u)
    # --- 5. every page size the PPD offers, to see header/geometry changes ---
    while read -r sz; do
        one_case "size-$sz" "PageSize=$sz media=$sz $S1" "frame"
    done < <(grep -E '^\*PageSize [^/]+/' "$PPD_FILE" | sed -E 's/^\*PageSize ([^/]+)\/.*/\1/' | sort -u)
fi

tar -C "$OUT" -czf "$OUT/corpus.tar.gz" corpus manifest.tsv
echo
echo "cases: $n   suspicious: $bad"
echo "manifest:  $MAN"
echo "tarball:   $OUT/corpus.tar.gz   ($(du -h "$OUT/corpus.tar.gz" | cut -f1))"
echo "Check first: cases with deterministic=no (note which bytes vary), exit!=0, or prn_bytes=0."
