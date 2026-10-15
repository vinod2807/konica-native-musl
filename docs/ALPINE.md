# Alpine Linux — build, test, install, rollback

How to build and test `konica-native` natively on Alpine/musl. Verified on **Alpine
3.24.2 (x86_64, musl)** with gcc 15.2.0. See `docs/ALPINE-PORT-STATUS.md` for the exact
baseline and test results.

> Status: the **build and offline tests** below are verified. **CUPS/printer installation
> (Phase 5) is NOT run yet** — the install/rollback section is a plan, not a tested recipe.

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
CUPS packages are only needed for the reader cross-check, not to run the filter.

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
errors out and produces a 119-byte stub.

The whole-page colour transform needs the vendor's 33³ ACMM lookup table as well. The kit
does not bundle it; load a runtime dump with `KONICA_LUT=/path/to/lut33.bin`
(`tools/dump-lut.gdb` reproduces it; see `docs/REVERSE-ENGINEERING.md` §9.5). If it is
unset, the filter still runs, with a flat-tone approximation.

```sh
# reader + writer cross-check vs libcups (needs cups-dev)
make check-reader

# full differential corpus (needs python3 and the corpus)
KONICA_LUT=/path/to/lut33.bin tests/difftest.sh /path/to/konica-corpus-full
```

Expected today: `check-reader` PASS 27/27; `difftest` **PASS 60 / FAIL 0** (with
`KONICA_LUT`); without it, the historical PASS 7 subset.

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

## Install (PLANNED — not yet tested)

Target (to be validated in Phase 5, using a temporary queue, never editing existing config):

```sh
install -Dm755 konica-native /usr/lib/cups/filter/konica-206
# runtime screen: either the vendor install path above, or a user-supplied file via KONICA_HFT
```

The filter's CUPS interface (argv: job-id user title copies options [file]) and the PPD
mapping must be checked against **Alpine's** CUPS version before use. Do not assume the
205i/225i/245i PPD describes the 206, and do not modify `/etc/cups/` or existing queues
without explicit approval.

## Rollback

Nothing in the build or the offline tests installs anything system-wide. If the planned
Phase-5 install is performed:

```sh
rm -f /usr/lib/cups/filter/konica-206
# remove only the temporary test queue/PPD that was created for validation
```

Restore any pre-existing queue/PPD from the backup taken **before** the change. Because no
source-level install step exists yet, rollback is currently a no-op.
