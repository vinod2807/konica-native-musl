#!/bin/sh
# step3-alpine.sh - build and verify konica-native on Alpine/musl. No printer, no CUPS changes.
#
# Run as root (installs packages, writes the runtime data) from an empty working directory, on an
# Alpine box or inside an Alpine chroot:
#
#   tools/step3-alpine.sh /path/to/konica-runtime-data.tgz [/path/to/real-compare.tgz]
#
# konica-runtime-data.tgz is the vendor's data, packed on the Ubuntu host with:
#   tar -czf konica-runtime-data.tgz -C /usr/local/lib/konica/KonicaMinolta/245igdi \
#       Halftones/COPA.DEFAULT.600x600.hft Profiles/sRGB.icm Profiles/sGrayInv.icm
# real-compare.tgz is the step-2 bundle (a directory named real-compare, holding *.ras, *.opts and
# *.vendor.prn) produced by tools/compare-real.sh:
#   tar -czf real-compare.tgz real-compare
#
# It installs build-base/make/git/python3/cups-dev, unpacks the data into the default path, clones
# the repo, builds with `make CC=cc`, runs `make check`, runs tools/verify-on-target.sh on the saved
# rasters, prints the data checksums (compare with Ubuntu) and a SUMMARY block to paste back.
#
# Paths are made absolute up front: the script cd's into the clone, so a relative argument or a
# relative summary.txt would be resolved against the wrong directory.
set -u
DATA=${1:?usage: $0 konica-runtime-data.tgz [real-compare.tgz]}
RC=${2:-}
ROOT=/usr/local/lib/konica/KonicaMinolta/245igdi
WD=$PWD
abspath() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$WD" "$1" ;; esac; }
DATA=$(abspath "$DATA")
[ -n "$RC" ] && RC=$(abspath "$RC")
res() { printf '%-28s %s\n' "$1" "$2" >> "$WD/summary.txt"; }
: > "$WD/summary.txt"

apk add --no-cache build-base make git python3 cups-dev >/dev/null 2>&1 || { echo "apk install failed"; exit 2; }

mkdir -p "$ROOT" && tar -C "$ROOT" -xzf "$DATA" || { echo "cannot unpack $DATA"; exit 2; }
chown -R root:root /usr/local/lib/konica && chmod -R go-w /usr/local/lib/konica
echo "== runtime data sha256 (compare with Ubuntu) =="; sha256sum "$ROOT"/Halftones/* "$ROOT"/Profiles/*

[ -d konica-native-musl ] || git clone -q https://github.com/vinod2807/konica-native-musl || exit 2
cd konica-native-musl || exit 2
git log -1 --format='commit %h' | tee "$WD/commit.txt" >/dev/null; res commit "$(cat "$WD/commit.txt")"
make clean >/dev/null 2>&1
if make CC=cc >"$WD/build.log" 2>&1; then res build OK; else res build FAIL; tail -20 "$WD/build.log"; fi
res ldd "$(ldd ./konica-native 2>&1 | tr '\n' ' ' | cut -c1-120)"
res "ldd shows glibc?" "$(ldd ./konica-native 2>&1 | grep -c 'libc.so.6')"

make check >"$WD/check.log" 2>&1; mkrc=$?
res "make check exit" "$mkrc"
res "check-lutgen" "$(grep -E '^PASS check-lutgen' "$WD/check.log" | tail -1)"
res "check-reader" "$(sed -n '/check-reader\.sh/,/check-lutgen\.sh/p' "$WD/check.log" | grep -c '^PASS') PASS / $(sed -n '/check-reader\.sh/,/check-lutgen\.sh/p' "$WD/check.log" | grep -c '^FAIL') FAIL"
res "make check totals" "$(grep -c '^PASS' "$WD/check.log") PASS / $(grep -c '^FAIL' "$WD/check.log") FAIL"
grep -E 'FAIL|SKIP' "$WD/check.log" | head -10

if [ -n "$RC" ]; then
    tar -xzf "$RC" -C "$WD" && sh tools/verify-on-target.sh "$WD/real-compare" ./konica-native | tee "$WD/verify.log" | tail -15
    res "verify-on-target" "$(tail -1 "$WD/verify.log")"
else
    res "verify-on-target" "skipped (no real-compare.tgz given)"
fi
cd "$WD"
echo; echo "===== SUMMARY (paste this back) ====="; cat "$WD/summary.txt"
