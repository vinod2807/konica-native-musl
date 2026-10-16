# Alpine Linux — build, test, install, rollback

How to build and test `konica-native` natively on Alpine/musl. Verified on **Alpine
3.24.2 (x86_64, musl)** with gcc 15.2.0. See `docs/ALPINE-PORT-STATUS.md` for the exact
baseline and test results.

> Status: the **build and offline tests** are verified, the **CUPS raster chain is tested on
> Alpine without a printer** (step 4 below, `tools/step4-cups-chain.sh`), and the **first real
> prints** (text, photo, both duplex bindings, copies) came out correct on the host
> (`docs/ALPINE-PORT-STATUS.md` §3.5).

## Dependencies

Minimal, for building the filter:

```sh
apk add --no-cache build-base make
# build-base pulls in: gcc, musl-dev, binutils, fortify-headers, patch, ...
```

For the offline **tests** only:

```sh
apk add --no-cache python3      # tests/difftest.sh + tools/prnscan.py
apk add --no-cache cups-dev     # tests/check-reader.sh (builds mkraster + libcups_dump)
```

The core filter uses **no** runtime dependencies beyond musl libc — no libcups, no libppd.
(The build links `-lm` for the CMM colour transform's cube root; on musl the maths
functions live in libc and `libm` is a compatibility stub. CUPS packages are only needed
for the reader cross-check, not to run the filter.)

## Build

```sh
make clean
make CC=cc
file ./konica-native
# ELF 64-bit LSB pie executable, x86-64, dynamically linked,
# interpreter /lib/ld-musl-x86_64.so.1
ldd ./konica-native
#   /lib/ld-musl-x86_64.so.1 (…)
#   libc.musl-x86_64.so.1 => /lib/ld-musl-x86_64.so.1 (…)
```

Strict build (should be warning-free):

```sh
make clean
make CC=cc CFLAGS='-O2 -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow'
```

## Test

Runtime data: the filter loads a halftone screen at runtime. Default path is the vendor
install (`/usr/local/lib/konica/KonicaMinolta/245igdi/Halftones/COPA.DEFAULT.600x600.hft`);
override with the `KONICA_HFT` environment variable. If the screen is missing the filter
exits non-zero and writes **nothing** to stdout.

The whole-page colour transform needs the vendor's 33³ ACMM lookup table. The vendor
*computes* it at run time from the installed ICC profiles, and the kit regenerates the same
table from them (`src/lutgen.c`), so **no dump is required**. The profiles default to the
vendor install (`.../245igdi/Profiles/{sRGB,sGrayInv}.icm`); override with `KONICA_SRGB`
and `KONICA_GRAY`. A captured dump can still be forced with `KONICA_LUT=/path/to/lut33.bin`
(`tools/dump-lut.gdb` reproduces it; see `docs/LUT-GENERATOR.md`).

The screen and the table are both loaded before the first byte reaches stdout, so if the
profiles cannot be read the filter exits non-zero with an empty stdout rather than emitting a
half-correct job; the old flat-tone approximation is available only via `KONICA_FLAT_TONE=1`.

```sh
# reader + writer cross-check vs libcups (needs cups-dev)
make check-reader

# regenerated CMM table vs the vendor's
make check-lutgen

# full differential corpus (needs python3 and the corpus)
tests/difftest.sh /path/to/konica-corpus-full
```

Expected today: `check-reader` PASS 27/27; `check-geometry` PASS 6/6; `check-lutgen` PASS;
`difftest` **PASS 60 / FAIL 0** — with the table regenerated from the profiles, no `KONICA_LUT`
needed. Without readable profiles the filter now stops with a hard error before writing a single
byte (see `KONICA_FLAT_TONE=1` in `README.md` for the old approximation).

## Disposable Alpine test rootfs (no Docker/Podman)

If Docker/Podman are unavailable, a minirootfs chroot works:

```sh
T=/var/tmp/alpine
mkdir -p "$T"; cd "$T"
curl -sO https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/alpine-minirootfs-3.24.2-x86_64.tar.gz
mkdir -p rootfs && tar -xzf alpine-minirootfs-3.24.2-x86_64.tar.gz -C rootfs
mount -t proc proc rootfs/proc
mount --bind /dev rootfs/dev
mount -t sysfs sys rootfs/sys
cp -L /etc/resolv.conf rootfs/etc/resolv.conf
chroot rootfs apk add --no-cache build-base make python3

# give the build inside the chroot access to the sources, corpus and vendor screen
mkdir -p rootfs/work
cp -r /path/to/konica-native-musl/{src,tests,tools,docs,Makefile} rootfs/work/
mkdir -p rootfs/corpus rootfs/usr/local/lib/konica
mount --bind /path/to/konica-corpus-full rootfs/corpus
mount --bind /usr/local/lib/konica        rootfs/usr/local/lib/konica

chroot rootfs sh -c 'cd /work && make clean && make && ./tests/difftest.sh /corpus'
```

To tear down:

```sh
umount rootfs/corpus rootfs/usr/local/lib/konica rootfs/proc rootfs/dev rootfs/sys
rm -rf "$T"
```

### Note: do not copy stale build artifacts into a fresh environment

`tools/mkraster` and `tests/libcups_dump` are **build outputs**. If a host (glibc) copy of
them is carried into the musl chroot, `check-reader.sh` will run the stale binary and every
case reports `SKIP (mkraster)`. Remove them (`rm -f tools/mkraster tests/libcups_dump`)
before running the tests so they are rebuilt natively.

## Install (tested on Alpine, without a printer)

The chain `pdftopdf → pdftoraster → konica-native` has been exercised end-to-end on Alpine
3.24.2 inside an **isolated `cupsd`** with a capture backend; nothing was sent to a printer.
`tools/step4-cups-chain.sh file.pdf PPD [native-binary]` runs the whole check and prints a
SUMMARY.

```sh
apk add --no-cache cups cups-client cups-filters poppler-utils ghostscript
# Alpine's Ghostscript 10.05.1 makes empty rasters with this PPD, so disable its raster path:
cp -a /usr/share/cups/mime/cupsfilters-ghostscript.convs /usr/share/cups/mime/cupsfilters-ghostscript.convs.orig
sed -i '/gstoraster/ { /application\/vnd\.cups-raster/ { /^#DISABLED/! s|^|#DISABLED | } }' \
    /usr/share/cups/mime/cupsfilters-ghostscript.convs

install -m 0755 -o root -g root konica-native /usr/local/lib/konica/konica-native

mkdir -p /var/lib/konica-native
sed -e 's|^\*cupsFilter:.*|*cupsFilter: "application/vnd.cups-raster 0 /usr/local/lib/konica/konica-native"|' \
    -e '/^\*cupsICCProfile/d' \
    KonicaMinolta-206-fullbleed.ppd > /var/lib/konica-native/KonicaMinolta-206-native.ppd
```

Use the **full-bleed 206** PPD (not the shipped 205/225/245i ones) and keep
`*cupsBackSide: Rotated`. CUPS runs filters with a scrubbed environment, so `KONICA_*` overrides
do **not** apply in production: the runtime data must sit at the default path. With the screen
missing the filter exits 1 and writes **0 bytes** (verified).

Then create a *temporary* test queue and print — never edit an existing queue or `/etc/cups/`.

### Copies need `*cupsManualCopies: True`

The full-bleed 206 PPD says `*cupsManualCopies: False`, and `konica-native` — like the vendor
filter — ignores the `copies` argument and always emits `@PJL SET COPIES=1`. With `False`, CUPS
produces **one copy** for `lp -n 2` (verified with a real `cupsd` on Alpine: `lp -n 1` and
`lp -n 2` both give a single-page, 103 538-byte stream). Adding

```
*cupsManualCopies: True
```

to the generated PPD makes CUPS duplicate the pages itself: `lp -n 2` then yields two pages in
one job (206 936 bytes), and `konica-native` stays **byte-identical to the vendor** on that
two-page raster. This is a PPD change, not a filter change.

## Rollback

Nothing in the build or the offline tests installs anything system-wide. If the test install
above was performed:

```sh
lpadmin -x KONICA206_NATIVE_TEST          # the temporary test queue
rm -f /usr/local/lib/konica/konica-native
rm -rf /var/lib/konica-native
cp -a /usr/share/cups/mime/cupsfilters-ghostscript.convs.orig /usr/share/cups/mime/cupsfilters-ghostscript.convs
```

Restore any pre-existing queue/PPD from the backup taken **before** the change.
