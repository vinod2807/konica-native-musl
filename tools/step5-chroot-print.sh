#!/bin/sh
# step5-chroot-print.sh - print REAL pages from an Alpine/musl box (or chroot) to the Konica 206.
#
#   tools/step5-chroot-print.sh file.pdf [more.pdf ...]
#
# Unlike step 4 (which captures the device stream to a file), this drives the ACTUAL USB printer.
# It starts an ISOLATED cupsd on 127.0.0.1:$CUPS_TEST_PORT (default 8631) so the machine's own CUPS
# on :631 is never disturbed, creates a temporary queue on the printer's USB URI, prints the given
# PDF(s), waits for each job, reports the bytes sent, then stops cupsd and deletes the queue. It
# backs up /etc/cups/cupsd.conf and restores it on exit.
#
# Run as root, on a machine where the printer is attached: an Alpine install, OR an Alpine chroot
# that shares the host's /dev (the chroot sees /dev/bus/usb, so the usb backend's libusb can reach
# the printer -- this is how it was first run, on a Ubuntu host).
#
# Assumes the box was already prepared by tools/step3-alpine.sh (runtime data at the default path)
# and tools/step4-cups-chain.sh (filter installed at $FILTER, gstoraster disabled for
# application/vnd.cups-raster so poppler's pdftoraster does the work).
#
# Env overrides:
#   CUPS_TEST_PORT  default 8631
#   PRINTER_URI     default usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1
#   QUEUE           default KONICA206_STEP5   (deleted on exit)
#   PPD             default /var/lib/konica-native/KonicaMinolta-206-native.ppd
#   LP_OPTS         default "-o media=A4 -o sides=one-sided"
#
# For copies: this PPD (like the full-bleed one) says *cupsManualCopies: False, so `lp -n 2` prints
# ONE copy. Point PPD at a copy with *cupsManualCopies: True and pass `-n 2` in LP_OPTS to get two.
set -u

[ $# -ge 1 ] || { echo "usage: $0 file.pdf [more.pdf ...]"; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 2; }

PORT=${CUPS_TEST_PORT:-8631}
URI=${PRINTER_URI:-usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1}
QUEUE=${QUEUE:-KONICA206_STEP5}
PPD=${PPD:-/var/lib/konica-native/KonicaMinolta-206-native.ppd}
LP_OPTS=${LP_OPTS:--o media=A4 -o sides=one-sided}
FILTER=/usr/local/lib/konica/konica-native
CONF=/etc/cups/cupsd.conf
BAK=/etc/cups/cupsd.conf.step5.orig

[ -x "$FILTER" ] || { echo "filter missing: $FILTER (run tools/step4-cups-chain.sh first)"; exit 2; }
[ -f "$PPD" ]    || { echo "PPD missing: $PPD"; exit 2; }

mkdir -p /run/cups /var/spool/cups /var/log/cups /var/cache/cups
[ -e "$BAK" ] || cp -a "$CONF" "$BAK"

CUPSD_PID=""
cleanup() {
    [ -n "$CUPSD_PID" ] && kill "$CUPSD_PID" 2>/dev/null
    [ -e "$BAK" ] && cp -a "$BAK" "$CONF"
    return 0
}
trap cleanup EXIT INT TERM

echo "=== isolated cupsd on 127.0.0.1:$PORT ==="
cat > "$CONF" <<CONF
ErrorLog /var/log/cups/error_log
LogLevel debug
MaxLogSize 0
Listen localhost:$PORT
Browsing Off
WebInterface No
<Location />
  Order allow,deny
  Allow localhost
</Location>
CONF
: > /var/log/cups/error_log
cupsd -f &
CUPSD_PID=$!
sleep 3
export CUPS_SERVER=localhost:$PORT
lpstat -r || { echo "cupsd did not start on $PORT"; exit 2; }

echo "=== queue $QUEUE -> $URI ==="
lpadmin -x "$QUEUE" >/dev/null 2>&1
lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD" -o printer-is-shared=false -D "konica-native step5" >/dev/null 2>&1 \
    || { echo "lpadmin failed"; exit 2; }
lpstat -p "$QUEUE" -v
grep -aE '^\*cupsFilter|^\*cupsManualCopies' "/etc/cups/ppd/$QUEUE.ppd"

rc=0
for PDF in "$@"; do
    [ -f "$PDF" ] || { echo "skip (missing): $PDF"; continue; }
    echo
    echo "=== print $PDF ==="
    out=$(lp -d "$QUEUE" $LP_OPTS "$PDF" 2>&1)
    job=$(printf '%s' "$out" | sed -n 's/.*request id is \([^ ]*\).*/\1/p')
    echo "job: $job  ($out)"
    i=0
    while [ $i -lt 300 ]; do
        [ "$(lpstat -o "$QUEUE" 2>/dev/null | wc -l)" -eq 0 ] && break
        sleep 1; i=$((i+1))
    done
    sleep 2
    echo "state: $(lpstat -p "$QUEUE" 2>/dev/null | head -1)"
    grep -a "\[Job ${job##*-}\]" /var/log/cups/error_log \
        | grep -aE 'Sent [0-9]+ bytes|exited with|Job completed|failed|Filter failed' | tail -8
    grep -aq "\[Job ${job##*-}\] Job completed" /var/log/cups/error_log || rc=1
done

lpadmin -x "$QUEUE" >/dev/null 2>&1
echo
echo "=== done (rc=$rc) ==="
exit $rc
