#!/bin/sh
# step4-cups-chain.sh - exercise the CUPS raster chain on Alpine WITHOUT a printer.
#
#   tools/step4-cups-chain.sh file.pdf /path/to/ppd [native-binary]
#
# It is the tested form of the runbook's step 4. It:
#   1. installs cups/cups-client/cups-filters/poppler-utils/ghostscript and disables the
#      gstoraster path for application/vnd.cups-raster (Alpine's Ghostscript makes empty
#      rasters with this PPD, so poppler's pdftoraster must do the work);
#   2. installs the filter at the default path and checks the fail-safe (missing .hft ->
#      exit 1 with 0 bytes on stdout);
#   3. builds the native PPD from the one you pass (cupsFilter -> konica-native, drop the
#      *cupsICCProfile lines; keeps *cupsBackSide: Rotated);
#   4. runs pdftopdf -> pdftoraster -> konica-native to FILES and scans the result;
#   5. starts an ISOLATED cupsd on 127.0.0.1:$CUPS_TEST_PORT (default 8631) with a capture
#      backend and checks `lp -n 1` vs `lp -n 2`, then, if the PPD says
#      `*cupsManualCopies: False`, re-runs with a `True` copy to show the copies fix.
#
# Requires the runtime data (.hft, .icm) at the default path (see tools/step3-alpine.sh).
# Nothing is sent to a printer. Existing queues and /etc/cups/cupsd.conf are left untouched
# (the config is backed up and restored; the test queue is deleted).
set -u
PDF=${1:?usage: $0 file.pdf ppd [native-binary]}
PPD_IN=${2:?usage: $0 file.pdf ppd [native-binary]}
NATIVE=${3:-./konica-native}
PORT=${CUPS_TEST_PORT:-8631}
FILTER=/usr/local/lib/konica/konica-native
NPPD=/var/lib/konica-native/KonicaMinolta-206-native.ppd
DATA=/usr/local/lib/konica/KonicaMinolta/245igdi
WD=$(mktemp -d /tmp/step4.XXXXXX)
chmod 0777 "$WD"   # CUPS backends run unprivileged; the capture backend must write here
abspath() { case "$1" in /*) printf '%s' "$1" ;; *) printf '%s/%s' "$PWD" "$1" ;; esac; }
PDF=$(abspath "$PDF"); PPD_IN=$(abspath "$PPD_IN"); NATIVE=$(abspath "$NATIVE")
res() { printf '%-30s %s\n' "$1" "$2" >> "$WD/summary.txt"; }
: > "$WD/summary.txt"

CONF_BAK=/etc/cups/cupsd.conf.step4.orig
restore_conf() { [ -e "$CONF_BAK" ] && cp -a "$CONF_BAK" /etc/cups/cupsd.conf; return 0; }
trap restore_conf EXIT INT TERM

echo "=== 4.1 packages and the Ghostscript workaround ==="
apk add --no-cache cups cups-client cups-filters poppler-utils ghostscript >/dev/null 2>&1 \
    || { echo "apk install failed"; exit 2; }
ls /usr/lib/cups/filter/pdftopdf /usr/lib/cups/filter/pdftoraster >/dev/null || { echo "cups-filters missing"; exit 2; }
if [ ! -e /usr/share/cups/mime/cupsfilters-ghostscript.convs.orig ]; then
    cp -a /usr/share/cups/mime/cupsfilters-ghostscript.convs /usr/share/cups/mime/cupsfilters-ghostscript.convs.orig
fi
sed -i '/gstoraster/ { /application\/vnd\.cups-raster/ { /^#DISABLED/! s|^|#DISABLED | } }' \
    /usr/share/cups/mime/cupsfilters-ghostscript.convs

echo "=== 4.2 install the filter + fail-safe ==="
install -m 0755 -o root -g root "$NATIVE" "$FILTER" || exit 2
HFT=$DATA/Halftones/COPA.DEFAULT.600x600.hft
[ -f "$HFT" ] || { echo "runtime data missing at $DATA (run tools/step3-alpine.sh first)"; exit 2; }
mv "$HFT" "$HFT.away"
"$FILTER" 1 u t 1 '' < /dev/null > "$WD/noscreen.prn" 2>"$WD/noscreen.err"; rc=$?
nbytes=$(stat -c%s "$WD/noscreen.prn")
mv "$HFT.away" "$HFT"
res "missing .hft -> exit/bytes" "$rc / $nbytes"

echo "=== 4.3 the native PPD ==="
mkdir -p "$(dirname "$NPPD")"
sed -e 's|^\*cupsFilter:.*|*cupsFilter: "application/vnd.cups-raster 0 '"$FILTER"'"|' \
    -e '/^\*cupsICCProfile/d' "$PPD_IN" > "$NPPD"
res "cupsFilter" "$(grep -a '^\*cupsFilter' "$NPPD" | head -1)"
res "cupsManualCopies" "$(grep -a '^\*cupsManualCopies' "$NPPD" | head -1)"

echo "=== 4.4 the chain, to files ==="
O='PageSize=A4 media=A4 Duplex=None sides=one-sided Duplexer=true MediaType=Plain'
export PPD="$NPPD" CONTENT_TYPE=application/pdf FINAL_CONTENT_TYPE=application/vnd.cups-raster \
       CUPS_SERVERROOT=/etc/cups CUPS_DATADIR=/usr/share/cups
/usr/lib/cups/filter/pdftopdf  1 u t 1 "$O" "$PDF"      > "$WD/a.pdf2" 2>/dev/null || res pdftopdf FAIL
/usr/lib/cups/filter/pdftoraster 1 u t 1 "$O" < "$WD/a.pdf2" > "$WD/a.ras" 2>/dev/null || res pdftoraster FAIL
"$FILTER"                       1 u t 1 "$O" < "$WD/a.ras"  > "$WD/a.prn" 2>/dev/null || res konica-native FAIL
res "chain prn" "$(stat -c%s "$WD/a.prn" 2>/dev/null) bytes, $(grep -ac '@PJL EOJ' "$WD/a.prn" 2>/dev/null) EOJ, $(grep -ac 'PAGESTATUS=START' "$WD/a.prn" 2>/dev/null) page(s)"
python3 "$(dirname "$0")/prnscan.py" scan "$WD/a.prn" 2>/dev/null | sed -n '1,8p'

echo "=== 4.5 isolated cupsd + capture backend: lp -n 1 vs -n 2 ==="
cat > /usr/lib/cups/backend/capture <<'BACKEND'
#!/bin/sh
cat > "${DEVICE_URI#capture:}"
exit 0
BACKEND
chmod 0755 /usr/lib/cups/backend/capture
mkdir -p /run/cups /var/spool/cups /var/log/cups /var/cache/cups
[ -e "$CONF_BAK" ] || cp -a /etc/cups/cupsd.conf "$CONF_BAK"
cat > /etc/cups/cupsd.conf <<CONF
ErrorLog /var/log/cups/error_log
Listen localhost:$PORT
Browsing Off
WebInterface No
<Location />
  Order allow,deny
  Allow localhost
</Location>
CONF
cupsd 2>/dev/null; sleep 2
export CUPS_SERVER=localhost:$PORT

run_job() {   # run_job <queue> <copies> <ppd> <label>
    _q=$1 _n=$2 _p=$3 _l=$4
    lpadmin -x "$_q" >/dev/null 2>&1
    lpadmin -p "$_q" -v "capture:$WD/capture.prn" -P "$_p" -E >/dev/null 2>&1 || { res "$_l" "lpadmin failed"; return; }
    rm -f "$WD/capture.prn"
    lp -d "$_q" -n "$_n" -o media=A4 -o sides=one-sided "$PDF" >/dev/null 2>&1
    i=0; while [ $i -lt 60 ] && lpstat -o "$_q" 2>/dev/null | grep -q .; do sleep 0.5; i=$((i+1)); done
    sleep 1
    if [ -f "$WD/capture.prn" ]; then
        res "$_l" "bytes=$(stat -c%s "$WD/capture.prn") pages=$(grep -ac 'PAGESTATUS=START' "$WD/capture.prn") $(grep -ao '@PJL SET COPIES=[0-9]*' "$WD/capture.prn" | head -1)"
    else
        res "$_l" "no output"
    fi
    lpadmin -x "$_q" >/dev/null 2>&1
}

if lpstat -r >/dev/null 2>&1; then
    run_job KONICA_NATIVE_TEST 1 "$NPPD" "lp -n 1"
    run_job KONICA_NATIVE_TEST 2 "$NPPD" "lp -n 2"
    if grep -qa '^\*cupsManualCopies: False' "$NPPD"; then
        sed 's|^\*cupsManualCopies:.*|*cupsManualCopies: True|' "$NPPD" > "$WD/manual.ppd"
        run_job KONICA_NATIVE_TEST 2 "$WD/manual.ppd" "lp -n 2 (ManualCopies True)"
    fi
else
    res cupsd "could not start on port $PORT"
fi

pid=$(cat /run/cups/cupsd.pid 2>/dev/null || cat /var/run/cups/cupsd.pid 2>/dev/null)
[ -n "${pid:-}" ] && kill "$pid" 2>/dev/null
restore_conf

echo "== artifacts in $WD =="; ls -l "$WD"
echo; echo "===== SUMMARY (paste this back) ====="; cat "$WD/summary.txt"
