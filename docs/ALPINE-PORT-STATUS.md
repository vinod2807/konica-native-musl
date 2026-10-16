# Alpine port — status and baseline

Report for the OpenCode task brief (finish + port the Konica Minolta bizhub 206 native
driver to Alpine/musl). This file records the **environment, exact commands, and results**
for Phases 1–2, and separates PASS / FAIL / SKIP / BLOCKED / NOT RUN.

_Last updated: 2026-10-16._

## 1. Repository / baseline facts

* Project root: `/home/spot/Downloads/konica-native-musl`.
* **Git repository**, published PUBLIC at
  <https://github.com/vinod2807/konica-native-musl>, branch `main`. This file describes the
  state at commit `983ec30` (2026-10-16).
* Source tree: `src/{main,raster,encode,jbig,halftone,acmm,lutgen}.{c,h}`, `tests/`, `tools/`,
  `docs/`, plus `Makefile`, `README.md`, `HANDOFF.md`.

  *(Originally the project was not a git repository and was tracked by hand; it was initialised
  and published afterwards. The old hand-tracked file list is superseded by the `Layout`
  section of `README.md`.)*

* Build artifact `./konica-native` is produced by `make`; `tools/mkraster`, `tests/libcups_dump`,
  `tools/lutgen-dump` and `tools/biedec` are produced by the test targets. **These are build
  artifacts, not tracked sources** — see the stale-artifact note in §5.

## 2. Build environments

| | Development host | Native Alpine target |
|---|---|---|
| OS | Puppy Linux 26.04 (x86_64) | Alpine Linux 3.24.2 (x86_64) |
| libc | glibc 2.43 | musl (x86_64) |
| compiler | gcc 15.2.0 (Ubuntu 15.2.0-16ubuntu1) | gcc (Alpine 15.2.0) 15.2.0 |
| make | GNU Make (host) | GNU Make 4.4.1 |
| loader | `/lib64/ld-linux-x86-64.so.2` | `/lib/ld-musl-x86_64.so.1` |

Alpine test environment method: no Docker/Podman/`apk` on the host, but `chroot`, `bwrap`,
`curl` and root are available, so a **disposable Alpine minirootfs** was used:

```sh
T=/initrd/mnt/tmpfs/tmp/opencode/alpine
curl -sO https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/alpine-minirootfs-3.24.2-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.24.2-x86_64.tar.gz -C $T/rootfs
mount -t proc proc $T/rootfs/proc ; mount --bind /dev $T/rootfs/dev ; mount -t sysfs sys $T/rootfs/sys
cp -L /etc/resolv.conf $T/rootfs/etc/resolv.conf
chroot $T/rootfs apk add --no-cache build-base make
# for tests only:
chroot $T/rootfs apk add --no-cache python3 cups-dev
```

Minimal package set for the build: `build-base make`. For the tests: `python3` (difftest /
prnscan) and `cups-dev` (reader cross-check). No CUPS runtime is needed to build or to run
`konica-native`.

## 3. Commands actually run and results

### 3.1 Host (glibc)

```sh
make clean && make
# cc -O2 -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L -o konica-native ...
# exit 0

make CC=cc CFLAGS='-O2 -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow'
# exit 0  (no warnings after the halftone.c size_t fixes in §4)

sh tests/check-reader.sh      # PASS 27 / FAIL 0 / SKIP 0
make check-lutgen             # PASS: regenerated 33³ CMM table == the vendor's
./tests/difftest.sh /root/konica-corpus-full
# PASS: 60 (of which masked: 0)   FAIL: 0
```

(The `KONICA_LUT=/tmp/lut33.bin` prefix seen in earlier revisions of this log is no
longer needed: the table is regenerated from the installed ICC profiles. See
`docs/LUT-GENERATOR.md`.)

### 3.2 Native Alpine / musl

```sh
chroot $T/rootfs sh -c 'cd /work && make clean && make CC=cc'
# exit 0
file konica-native
# ELF 64-bit LSB pie executable, x86-64, dynamically linked,
# interpreter /lib/ld-musl-x86_64.so.1
ldd konica-native
#   /lib/ld-musl-x86_64.so.1
#   libc.musl-x86_64.so.1 => /lib/ld-musl-x86_64.so.1     (no glibc dependency)

chroot $T/rootfs sh -c "cd /work && make CC=cc CFLAGS='-O2 -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow'"
# exit 0  (clean)

chroot $T/rootfs sh -c 'cd /work && sh tests/check-reader.sh'   # PASS 27 / FAIL 0 / SKIP 0
chroot $T/rootfs sh -c 'cd /work && make check-lutgen'          # PASS (byte-identical table)
chroot $T/rootfs sh -c 'cd /work && ./tests/difftest.sh /corpus'
# PASS: 60 (of which masked: 0)   FAIL: 0
```

**Re-verified on Alpine 3.20.3 / gcc 13.2.1 / musl 1.2.x (2026-10-15):** the musl build is
warning-clean and the differential test reports `PASS: 60   FAIL: 0`, identical to glibc.

**Re-verified again on Alpine 3.24.2 / musl 1.2.5 (2026-10-15, `lut-generator` branch):** after
adding the CMM table generator (`src/lutgen.c`), the musl build is still warning-clean,
`tests/difftest.sh /corpus` reports `PASS: 60   FAIL: 0` with the table **regenerated from the
installed ICC profiles** (no `KONICA_LUT`), `make check-lutgen` passes (byte-identical table,
sha256 `84a424f5…c6d6f`) and `make check-reader` is 27/27.

**Re-verified once more on Alpine 3.24.2 / musl 1.2.5 (2026-10-16, commit `da1070d`):** a fresh
`alpine-minirootfs` chroot with the vendor runtime data unpacked at the default path (sha256 match
Ubuntu: `.hft 24486c45…`, `sRGB.icm 2b3aa164…`, `sGrayInv.icm 01fd6208…`). `make CC=cc` gives a
musl-only binary (`ldd` shows no `libc.so.6`); `make check` exits 0 with **62 PASS / 0 FAIL**
(reader 27/27, `check-lutgen` PASS with the same sha256 `84a424f5…c6d6f`, geometry 6/6, mediatype
20/20, errors 6/6); `difftest.sh` over the 60-case corpus is **PASS 60 / FAIL 0**; and
`tools/verify-on-target.sh` over 14 saved rasters is **PASS: 14 / FAIL: 0**. `tools/step3-alpine.sh`
automates this whole check.

**Conclusion:** the source is already musl-clean and produces **identical results** on glibc
and musl (60/60 corpus, 27/27 reader). No source portability defect was found.

### 3.3 Reader cross-check under Alpine needs `cups-dev`

`tests/check-reader.sh` compares our libc-only reader against libcups via
`tools/mkraster` + `tests/libcups_dump`, both built with `-lcups`. In the Alpine chroot this
required `apk add cups-dev`. Once built natively it passes 27/27.

### 3.4 CUPS chain on Alpine, without a printer (2026-10-16)

The raster chain `pdftopdf → pdftoraster → konica-native` was run on Alpine 3.24.2 with the
generated native PPD, both by hand and through an **isolated `cupsd`** on `127.0.0.1:8631` with a
capture backend. Nothing was sent to a printer. `tools/step4-cups-chain.sh` automates it.

* filters present (`pdftopdf`, `pdftoraster`); the gstoraster path for
  `application/vnd.cups-raster` disabled (Alpine's Ghostscript 10.05.1 makes empty rasters);
* the filter installed at `/usr/local/lib/konica/konica-native`; a missing `.hft` → exit 1 with
  **0 bytes** on stdout;
* native PPD built from the full-bleed 206 PPD (`cupsFilter` → the filter, ICC lines dropped);
* chain output: 103 538 bytes, one `@PJL EOJ`, `COMPRESS=JBIG`, `IMAGELEN` chunks — a valid
  stream (the bytes differ from the Ubuntu host only because poppler rasterises differently);
* **copies:** `lp -n 2` yields **one** copy, because the full-bleed PPD says
  `*cupsManualCopies: False` and the filter — like the vendor — always emits
  `@PJL SET COPIES=1`. With `*cupsManualCopies: True`, CUPS duplicates the pages (`lp -n 2` →
  206 936 bytes, two pages, one job), and `konica-native` stays byte-identical to the vendor on
  that two-page raster. See `docs/ALPINE.md` "Copies need `*cupsManualCopies: True`".

### 3.5 First real prints (Ubuntu host, 2026-10-16)

With the printer attached to the Ubuntu host, a **temporary** queue `KONICA206_NATIVE_TEST`
(`usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1`, the native PPD) printed the
runbook's step-5 rows. Every job ended `Job completed` with each filter reporting
`exited with no errors`:

| Row | Job | Stream sent | Paper |
|---|---|---|---|
| text, A4 simplex | 230 | — | correct |
| photo, A4 simplex | 231 | 73 221 B | correct |
| twopage, long-edge duplex | 232 | 167 401 B | correct |
| twopage, short-edge duplex | 233 | 169 007 B | correct |
| text, `lp -n 2` (`*cupsManualCopies: True`) | 234 | 191 608 B (~2×) | two copies |

The host chain is `pdftopdf → gstoraster → konica-native → usb` (Ubuntu's Ghostscript handles
this PPD; Alpine needed poppler's `pdftoraster`). The temporary queue and the installed filter
are reversible — see `docs/ALPINE.md` "Rollback".

### 3.6 Printing from the Alpine/musl chroot (2026-10-16)

There is no separate Alpine box in this setup, but the Alpine chroot shares the host's `/dev`, so a
**musl `cupsd` inside the chroot** can drive the USB printer directly. This closes the runbook's
last gap ("a print from the Alpine machine itself"). A `cupsd` isolated on `127.0.0.1:8631` (host
CUPS on `:631` untouched) with a queue on the printer's USB URI printed the step-5 rows from the
**musl** filter at `/usr/local/lib/konica/konica-native`; every job ended `Job completed` with each
filter `exited with no errors`:

| Row | Job | Stream sent |
|---|---|---|
| text, A4 simplex | 11 | 103 538 B |
| photo, A4 simplex | 12 | 70 701 B |
| twopage, long-edge duplex | 13 | 172 325 B |
| twopage, short-edge duplex | 14 | 174 151 B |
| text, `lp -n 2` (`*cupsManualCopies: True`) | 15 | 206 936 B (~2×) |

The chain is `pdftopdf → pdftoraster → konica-native → usb` (Alpine uses poppler's `pdftoraster`).
The byte counts differ from §3.5 because the rasterizer differs (Ghostscript vs poppler) — the
filter is deterministic *given the raster*. `tools/step5-chroot-print.sh` reproduces this.

## 4. Portability fixes made

One source change, to make the strict build clean (no behaviour change):

* `src/halftone.c` — three `-Wsign-conversion` sites in `halftone_load` / `halftone_row`
  (`(size_t)w * h` and `(size_t)(y % ht->h) * ht->w`). Changed the `int` operands to
  `size_t` explicitly. Strict build is now warning-free.

Earlier tidy-ups landed in the same session (non-portability, no behaviour change):

* `src/main.c` — removed a duplicated `if (!line)` NULL check; consolidated the stale
  `OCM_TonerSave` comment and set the absent-option default to `1` (corpus-proven: case 005
  ≡ 034, ≠ 035).
* `src/encode.h` — corrected the `toner_save` field comment.
* `src/raster.h` — collapsed two duplicated page-framing comment blocks into one.

## 5. Stale-artifact hazard (found and worked around)

`tools/mkraster` and `tests/libcups_dump` are **build artifacts**. When the source tree was
copied into the Alpine chroot, the host's **glibc** `tools/mkraster` came with it;
`check-reader.sh` saw it as executable and ran it under musl, where it failed to load →
every case reported `SKIP (mkraster)`. Removing `tools/mkraster tests/libcups_dump` and
rebuilding natively fixed it. `make clean` now removes **every** build artifact
(`konica-native`, `konica-native-asan`, `tests/libcups_dump`, `tools/lutgen-dump`,
`tools/biedec`, `tools/mkraster`), so the hazard is closed.

## 6. Asset inventory (what is available on this machine)

| Asset | Path | Status |
|---|---|---|
| Vendor filter | `/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf` | present (`0548f3f3…`) |
| Vendor plugin | `…/Filters/mtorf.ocm` | present (`9c22aa97…`, has DWARF) |
| Halftone screens | `…/245igdi/Halftones/COPA.DEFAULT.{600x600,1200x600}.hft` | present |
| Full-bleed PPD | `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | present |
| Corpus (60 cases) | `/root/konica-corpus-full/corpus/*.{ras,prn}` + `manifest.tsv` | present |
| Corpus integrity | 120/120 sha256 match `manifest.tsv` | **verified** |
| RE scratch tooling | `/tmp/g*.cmds`, `/tmp/probe*.py`, `/tmp/gen_*.py`, `/tmp/traw*.bin` | gone (ephemeral `/tmp`; the durable recipes live in `tools/` and `docs/`) |
| Disassembly tooling | `gdb`, `strace`, `objdump`, `nm`, `python3` | present |

All corpus `.ras`/`.prn` hashes were re-verified against `manifest.tsv` before any testing;
the corpus was not modified.

## 7. Test status summary

| Check | Host (glibc) | Alpine (musl) |
|---|---|---|
| Default build | PASS | PASS |
| Strict build (`-Wconversion -Wshadow`) | PASS (clean) | PASS (clean) |
| `check-reader.sh` | PASS 27/27 | PASS 27/27 |
| `check-lutgen` (33³ CMM table) | PASS — byte-identical to the vendor's, sha256 `84a424f5…c6d6f` | PASS — same sha256 (2026-10-16) |
| `check-geometry.sh` | PASS 6/6 | PASS 6/6 |
| `difftest.sh` (60 cases) | **PASS 60 / FAIL 0** | **PASS 60 / FAIL 0** (re-run 2026-10-16) |
| Real documents vs the vendor filter | 2026-10-16 re-check: `WordCraft-test`, `7.pdf`, `problematic.pdf` byte-identical; CV pad-bytes-only; a real-photo page (A4 and Letter) byte-identical (§7.2) | **PASS 14 / FAIL 0** via `tools/verify-on-target.sh` on the same rasters (2026-10-16) |
| CUPS chain (`pdftopdf`→`pdftoraster`→filter, incl. an isolated `cupsd`) | PASS — step 4 (2026-10-16) | PASS — step 4 (2026-10-16) |
| Printer / hardware prints | PASS — 5/5 rows, 2026-10-16 (§3.5) | not applicable (host printer) |

### 7.1 Corrections to earlier revisions of this file

Sections 7 and 8 previously reported `difftest.sh` as **PASS 7 / FAIL 53** and listed the ACMM
colour-world filter as BLOCKED. Both statements were stale leftovers from an earlier phase and
are withdrawn:

* the colour stage is **fully recovered**: `src/lutgen.c` regenerates the vendor's 33³ CMM
  table byte-for-byte from the installed `sRGB.icm`/`sGrayInv.icm` profiles (`make check-lutgen`,
  sha256 `84a424f5cccdbe7070b3f926fe9834b515db69391440ccac1dc5b046622c6d6f`), so there is no
  flat-tone-only fallback on the normal path;
* `difftest.sh` against the 60-case corpus is **60 PASS / 0 FAIL** on both glibc and musl;
* the 7-vs-53 split predates the LUT generator landing (2026-10-15) and was never updated.

### 7.2 Real-document verification (2026-10-16)

Three real PDFs were rendered with the real CUPS chain (`pdftopdf` → `pdftoraster`, CUPS raster
v3, 8-bit, `cs=0`) and fed to both the vendor's `245igdi/Filters/245igdirf` and this kit, with
stdout captured to files — **nothing was ever sent to a printer**. This found two defects that
the synthetic corpus cannot see, both now fixed:

* **geometry keying.** The vendor derives `PAPERWIDTH`/`PAPERLENGTH` and the BIH `Xd`/`Yd` crop
  from the raster header's `PageSize` **in points** — not from the `PageSize=` option, not from
  `PageSizeName`, and not from the raster's pixel dimensions. A real `pdftoraster` A4 page is
  4961×7016 pixels and calls itself `A4.Borderless`, a name that is absent from the PPD, so the
  old name-keyed table fell through to the pixel-dimension fallback and emitted `4960/7015`
  instead of the vendor's `4958/7016`. The table in `src/encode.c` is now keyed on points
  **only** (either orientation), with `CUSTOM` for anything else; `tests/check-geometry.sh`
  pins the lookup and the custom bounds.
* **forced resolution.** The vendor reports the *device* resolution, not the raster's: rewriting
  a raster's `HWResolution` to 300 or 1200 dpi still produces a byte-identical stream with
  `@PJL SET RESOLUTION=600`. The kit now emits 600 unconditionally and warns on stderr if the
  raster disagrees.

Results after the fixes:

| Document | Pages | Vendor vs kit |
|---|---|---|
| `WordCraft-test.pdf` | 1 | byte-identical (2439 B) |
| `7.pdf` | 4 | byte-identical (456864 B) |
| `ACF0E705EDF100D6FF0E523618376FA1-Tejaswini_CV.pdf` | 2 | identical except the final 2 bytes of page 2 |
| `problematic.pdf` | 3 | byte-identical (721699 B) — re-checked 2026-10-16 |
| a real-photo page (A4 / Letter) | 1 / 1 | byte-identical (430428 / 452649 B) — re-checked 2026-10-16 |

The single remaining difference is a **vendor defect that cannot be reproduced portably**: the
4-byte padding at the end of a BIE follows the last SDNORM marker and is never decoded, and the
vendor writes *uninitialised* `malloc()`ed memory there. Evidence: page 1's stream is 77476 bytes
and page 2's 44036, both under glibc's 128 KiB `mmap` threshold, so page 2's allocation reuses the
block freed after page 1 and its untouched tail held page 1's bytes — `42 be`, exactly page 1's
bytes at offsets 44034–44035. Corpus cases 029/031/033 have page streams ≥ 128 KiB, which glibc
serves with `mmap` (freshly zeroed pages), and there the vendor's pad bytes are `00 00` even
though stale bytes were available. Reproducing that would mean emulating the C library's
allocator, so the kit emits deterministic zeros; a byte-exact diff of a real document should mask
the trailing `len % 4 ? 4 - len % 4 : 0` bytes of any page whose stream is not already 4-aligned.

**Re-run 2026-10-16 with `tools/compare-real.sh` / `tools/verify-on-target.sh`:** the two
byte-identical documents above still reproduce exactly (`WordCraft-test` 2439 B, `7.pdf`
456864 B), a third real 3-page document (`problematic.pdf`, 721699 B) is byte-identical, and the
CV is pad-bytes-only as before — the helper scripts keep every raster, so the same vendor output
can be re-checked on another machine. Two further documents are pad-bytes-only (a 1-page bus
ticket; the CV), and a page built from a real photograph is byte-identical on both A4 and Letter.
(A fourth PDF on the host is password-encrypted, so `pdftopdf` refuses it before either filter
runs.) Copies were checked at the same time: the vendor ignores `argv[4]` (copies `1`/`2`/`5` →
byte-identical), so `COPIES=1` is faithful, not a gap.

A second instance of the same defect was found later by the fuzzer, this time *inside* the
decoded image: when a raster row is not a multiple of 4 bytes, the byte just past the row (the
buffer's stride padding) is never written by the vendor, and the CMM's right-edge window reads
it — so the page's **last column** depends on what the recycled buffer happened to hold. The
vendor's own output then moves with `MALLOC_PERTURB_` (seed 2002: 225616 / 223567 / 223151 /
225720 bytes for 0/1/85/170), so no deterministic filter can match it. `src/encode.c` keeps
writing zeros and `tests/fuzz-difftest.sh` classifies such a case as vendor-nondeterministic
rather than divergent; see `docs/REVERSE-ENGINEERING.md` §9.2.

## 8. Remaining blockers

1. **FIXED (was blocked):** the ACMM colour-world stage. The 33³ CMM table is regenerated
   byte-for-byte from the shipped ICC profiles (`make check-lutgen`), so the flat-tone path is no
   longer used on the normal path. `src/encode.c` now loads the halftone screen and the colour
   table in `enc_open()`, *before* the first PJL byte is written, so an unreadable profile is a
   hard error with no partial output on stdout rather than a silent downgrade; the historical
   flat-tone approximation survives only behind the explicit `KONICA_FLAT_TONE=1` escape hatch.
2. **DONE for the chain; DONE for hardware on the host and from Alpine.** The CUPS chain on Alpine
   is verified without a printer (§3.4), the first real prints on the Ubuntu host all came out
   correct (§3.5), and the same rows print from the Alpine/musl chroot itself (§3.6,
   `tools/step5-chroot-print.sh`): text, photo, both duplex bindings and `lp -n 2` copies. One
   PPD-level finding: the generated PPD must say `*cupsManualCopies: True` or `lp -n 2` prints a
   single copy.
3. **DONE (was NOT RUN):** the Alpine/musl re-verification. A fresh `alpine-minirootfs` chroot
   (Alpine 3.24.2, musl 1.2.5) at commit `da1070d` reproduced every host result — musl-only
   binary, `make check` 62 PASS / 0 FAIL, `difftest` 60/60, `check-lutgen` byte-identical, and
   `verify-on-target` 14/14 on real rasters. See §3.2 and `tools/step3-alpine.sh`.
