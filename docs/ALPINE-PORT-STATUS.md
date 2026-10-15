# Alpine port — status and baseline

Report for the OpenCode task brief (finish + port the Konica Minolta bizhub 206 native
driver to Alpine/musl). This file records the **environment, exact commands, and results**
for Phases 1–2, and separates PASS / FAIL / SKIP / BLOCKED / NOT RUN.

_Last updated: 2026-10-15._

## 1. Repository / baseline facts

* Project root: `/home/spot/Downloads/konica-native-musl`.
* **Not a Git repository** (`git status` → `fatal: not a git repository`). No commit hash or
  branch exists; no branch/backup could be created. Changes are tracked here by hand.
* Source tree (tracked source, `find . -maxdepth 3 -type f`):

  ```
  Makefile  README-RE.md
  src/{main,raster,encode,jbig,halftone,acmm}.c  src/{raster,encode,jbig,halftone,acmm}.h
  tests/{difftest.sh,check-reader.sh,libcups_dump.c}
  tools/{capture-corpus.sh,dump-lut.gdb,mkraster.c,prnscan.py}
  docs/{DEVICE-STREAM.md,REVERSE-ENGINEERING.md,ALPINE.md,ALPINE-PORT-STATUS.md}
  ```

* Build artifact `./konica-native` is produced by `make`; `tools/mkraster` and
  `tests/libcups_dump` are produced by `make check-reader`. **These are build artifacts, not
  tracked sources** — see the stale-artifact note in §5.

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
KONICA_LUT=/tmp/lut33.bin ./tests/difftest.sh /root/konica-corpus-full
# PASS: 60 (of which masked: 0)   FAIL: 0
```

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
chroot $T/rootfs sh -c 'cd /work && KONICA_LUT=/tmp/lut33.bin ./tests/difftest.sh /corpus'
# PASS: 60 (of which masked: 0)   FAIL: 0
```

**Re-verified on Alpine 3.20.3 / gcc 13.2.1 / musl 1.2.x (2026-10-15):** the musl build is
warning-clean and the differential test reports `PASS: 60   FAIL: 0`, identical to glibc.

**Conclusion:** the source is already musl-clean and produces **identical results** on glibc
and musl (60/60 corpus, 27/27 reader). No source portability defect was found.

### 3.3 Reader cross-check under Alpine needs `cups-dev`

`tests/check-reader.sh` compares our libc-only reader against libcups via
`tools/mkraster` + `tests/libcups_dump`, both built with `-lcups`. In the Alpine chroot this
required `apk add cups-dev`. Once built natively it passes 27/27.

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
rebuilding natively fixed it. `make clean` currently removes `konica-native` and
`tests/libcups_dump` but **not `tools/mkraster`** — flagged for a future Makefile tidy.

## 6. Asset inventory (what is available on this machine)

| Asset | Path | Status |
|---|---|---|
| Vendor filter | `/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf` | present (`0548f3f3…`) |
| Vendor plugin | `…/Filters/mtorf.ocm` | present (`9c22aa97…`, has DWARF) |
| Halftone screens | `…/245igdi/Halftones/COPA.DEFAULT.{600x600,1200x600}.hft` | present |
| Full-bleed PPD | `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | present |
| Corpus (60 cases) | `/root/konica-corpus-full/corpus/*.{ras,prn}` + `manifest.tsv` | present |
| Corpus integrity | 120/120 sha256 match `manifest.tsv` | **verified** |
| RE scratch tooling | `/tmp/g*.cmds`, `/tmp/probe*.py`, `/tmp/gen_*.py`, `/tmp/traw*.bin` | present |
| Disassembly tooling | `gdb`, `strace`, `objdump`, `nm`, `python3` | present |

All corpus `.ras`/`.prn` hashes were re-verified against `manifest.tsv` before any testing;
the corpus was not modified.

## 7. Test status summary

| Check | Host (glibc) | Alpine (musl) |
|---|---|---|
| Default build | PASS | PASS |
| Strict build | PASS (clean) | PASS (clean) |
| `check-reader.sh` | PASS 27/27 | PASS 27/27 |
| `difftest.sh` (60 cases) | **PASS 7 / FAIL 53** | **PASS 7 / FAIL 53** |
| Printer / CUPS integration | NOT RUN | NOT RUN |

The 7 passing cases are `001-simplex-blank`, `002-simplex-solid`, `009/010/011/012-probe-dot_*`,
`019-tex-hlines_16`. The 53 failures are all attributed to the un-reproduced nonlinear
colour-conversion stage (ACMM `PCM_lfFilter8Eval3x1`); see `docs/REVERSE-ENGINEERING.md`
§6 and §9. **The driver is not complete.**

## 8. Remaining blockers

1. **BLOCKED (algorithmic):** the ACMM `PCM_lfFilter8Eval3x1` colour-world filter is
   identified and its call chain confirmed, but its full branch logic and table construction
   are not yet recovered, so `src/halftone.c` reproduces only flat tones. Next step in
   `docs/REVERSE-ENGINEERING.md` §9.
2. **NOT RUN:** CUPS integration on Alpine (Phase 5) and hardware testing (needs explicit
   user approval; nothing has been sent to a printer).
