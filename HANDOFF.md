# `konica-native-musl` — complete handoff report

**Audience: an AI assistant (Claude / GPT) or engineer picking this up cold.**
This single file is self-contained: it states what the project is, what is finished and
*verified*, exactly how to reproduce every claim, the complete recovered model, the
constraints that must not be broken, and what remains. Companion documents are listed in
[§15](#15-where-to-look); you should not need them to act, but they hold the raw evidence.

| | |
|---|---|
| Repository | `https://github.com/vinod2807/konica-native-musl` (public) |
| Default branch | `main` |
| Described code state | `48077ee` — *docs: record the recovered CMM table generator* |
| Prior commit | `e0a148e` — *lutgen: regenerate the ACMM 33³ table from the installed ICC profiles* |
| Language / build | C11, pure libc (+`libm`), `make`, gcc or clang, glibc **and** musl |
| Size | ~1 340 lines of C, ~3 300 lines of documentation |
| **Status** | **COMPLETE and byte-exact: 60/60 differential cases, fully self-contained** |

---

## 1. Executive summary

A Konica Minolta bizhub 206 printer is driven on Linux by a CUPS filter, `245igdirf`. The
filter reads a **CUPS raster** page on stdin and writes a **PJL envelope containing one
JBIG1 bitmap per page** to stdout; the CUPS backend then sends those bytes to the printer
over USB.

`konica-native` is a dependency-free C reimplementation of *exactly that* filter. It has
been reverse-engineered — nothing guessed — from the installed vendor binary
(disassembly + runtime captures), and it reproduces the vendor's stdout **byte for byte on
all 60 cases** of a captured differential corpus, on glibc and on Alpine/musl.

The last non-self-contained piece was the colour-management lookup table (a 33³ byte
grid). The vendor does not ship it as a file — its CMM *computes* it at start-up from the
installed ICC profiles. That generator has now been recovered in full and is reproduced by
`src/lutgen.c`; the regenerated table is **byte-identical** to the vendor's
(`sha256 84a424f5…6d6f`). No vendor data is embedded anywhere in the kit.

The kit has also been run on **real documents** (real PDFs → `pdftopdf` → `pdftoraster`,
CUPS raster v3) beside the vendor filter, with both stdout streams captured to files — no
printer involved. The first such run found two defects the synthetic corpus could not see: the
geometry table was keyed on `PageSizeName`, but the vendor keys on the raster header's
`PageSize` **in points** (real `pdftoraster` A4 calls itself `A4.Borderless`, a name absent
from the PPD), and the vendor forces `RESOLUTION=600` whatever the raster says. Both are fixed
and covered by `tests/check-geometry.sh`. Two of three real documents are now byte-identical;
the third differs in 2 bytes of BIE alignment padding that the device never decodes and that
the vendor fills with uninitialised heap memory ([§7.6](#76-jbig1-encoder-srcjbigc)).

The project is **finished**. [§12](#12-known-limitations--what-is-not-verified) lists the
only remaining uncertainties, all of them outside the tested corpus or outside the decoded
image.

---

## 2. Ground rules (do not break these)

These were the operating constraints of the whole project and still apply:

1. **Filter stdout to a file, never a printer.** The tool never opens a device. All testing
   is offline against captured data.
2. **Do not guess bytes.** Every value is derived from disassembly or from a capture, and
   verified against the corpus. If something cannot be reproduced faithfully, say so and
   keep the documented runtime input instead of inventing an approximation.
3. **Do not embed vendor data.** No `.hft` screen, no ICC profile, no `.ocm` plugin, no
   dumped LUT may be compiled into or committed to the kit. They are loaded at run time
   (or, for the LUT, *regenerated* from installed profiles).
4. **Do not vendor third-party encoder sources.** The JBIG1 QM coder is written from the
   ITU-T T.82 tables (`src/jbig.c`), not copied from jbigkit.
5. **Keep the kit dependency-free and pure-libc**, building on glibc and musl.
6. Work in small, reviewable, documented steps.

---

## 3. How to verify everything yourself

Everything below was re-run from a **fresh clone of the public repo** on the authoring
machine. Expected outputs are shown.

```sh
git clone https://github.com/vinod2807/konica-native-musl
cd konica-native-musl && make                     # 0 warnings, 0 errors
```

### 3.1 Differential test (the headline result)

```sh
tests/difftest.sh /path/to/konica-corpus-full
# → PASS: 60 (of which masked: 0)   FAIL: 0
```

`tests/difftest.sh` runs the candidate on every corpus `.ras` and compares, byte for byte,
with the vendor's captured `.prn`. **No environment variables are required** — the halftone
screen and the ICC profiles are read from the vendor install paths by default.

`masked: 0` matters: every case is deterministic and *fully* compared; nothing is skipped.

### 3.2 The CMM table generator vs the vendor's own table

```sh
make check-lutgen
# → PASS check-lutgen: sha256 84a424f5cccdbe7070b3f926fe9834b515db69391440ccac1dc5b046622c6d6f (vendor table)
```

This builds `tools/lutgen-dump`, regenerates the 33³ table from `sRGB.icm` + `sGrayInv.icm`,
and compares it with the recorded vendor hash. Given a captured reference dump instead it
does a byte comparison:

```sh
tests/check-lutgen.sh /tmp/lut33.bin      # → PASS … byte-identical to /tmp/lut33.bin
```

### 3.3 Reader cross-check against libcups

```sh
sudo apt install libcups2-dev      # or: apk add cups-dev
make check-reader
# → 27 PASS / 0 FAIL
```

### 3.4 Strict build

```sh
make clean && make CFLAGS='-O2 -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
                           -ffp-contract=off -D_POSIX_C_SOURCE=200809L'
# → 0 warnings
```

### 3.5 Geometry keying

```sh
make check-geometry
# → check-geometry: all cases pass
```

Pins the points-only size lookup (either orientation), the `CUSTOM` fallback and its 72..864 /
≤1224 pt bounds, the point→pixel rule, and the forced 600 dpi. Needs the vendor halftone
screen and ICC profiles, and skips cleanly without them. `make check` additionally runs
`check-mediatype` and `check-errors`, which need no vendor filter at all.

### 3.6 Verified result matrix

| Check | glibc (x86-64) | Alpine 3.24.2 / musl 1.2.5 |
|---|:---:|:---:|
| default build | clean | clean |
| strict build (`-Wconversion -Wshadow`) | clean | clean |
| `tests/difftest.sh` (LUT regenerated, no `KONICA_LUT`) | **60 / 60** | **60 / 60** |
| `make check-lutgen` | PASS | PASS |
| `make check-reader` | 27 / 27 | 27 / 27 |
| `make check-geometry` | pass | pass |
| `make check-mediatype` | 20 / 20 | 20 / 20 |
| `make check-errors` | 6 / 6 | 6 / 6 |
| `tools/verify-on-target.sh` (14 real rasters) | 14 / 14 | 14 / 14 |
| CUPS chain (`pdftopdf`→`pdftoraster`→filter, isolated `cupsd`, no printer) | pass (2026-10-16) | pass (2026-10-16) |
| `make check-determinism` (corpus, `MALLOC_PERTURB_`) | pass | not re-run |

Everything vendor-free at once: `make check`. The Alpine column is the **2026-10-16** measurement
at commit `da1070d`: a fresh `alpine-minirootfs` chroot reproduced every host result (musl-only
binary, `make check` 62 PASS / 0 FAIL, `difftest` 60/60, `check-lutgen` byte-identical,
`verify-on-target` 14/14). `tools/step3-alpine.sh` automates the whole musl build + check + verify.

---

## 4. What the software does

```
CUPS raster (stdin)
   │  src/raster.c     parse CUPS raster v1 / v2-RLE / v3, decode rows to 8-bit gray
   ▼
8-bit gray page (row by row)
   │  src/acmm.c       Φ: 3-tap tetrahedral interpolation of a 33³ byte LUT
   │                   (contiguous stride, right-edge wrap, once per band)
   │  src/lutgen.c     the 33³ table itself: regenerated from the ICC profiles at start-up
   │  src/encode.c     −10 when OCM_TonerSave is on (default)
   ▼
8-bit "tone" rows
   │  src/halftone.c   dot = (tone >= screen[page_row % 96][x % 96])
   ▼
1-bit page image
   │  src/jbig.c       JBIG1 (ITU-T T.82) encoder, 128-line stripes, LRLTWO+TPBON
   ▼
PJL envelope on stdout  (@PJL … IMAGELEN=<n> chunks … @PJL EOJ)
```

CLI, matching CUPS filter convention:

```
konica-native <job-id> <user> <title> <copies> "<options>" [raster-file]
konica-native --dump [raster-file]        # reader self-check: headers + FNV-1a pixel hash
```

---

## 5. The environment (vendor artifacts)

All of these are **inputs**, never committed. Paths are the vendor install locations.

| Artifact | Path | sha256 |
|---|---|---|
| Vendor filter (RE target) | `/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf` | `0548f3f3e20fae1e…` |
| OCM plugin | `…/245igdi/Filters/mtorf.ocm` | `9c22aa9779eaeedb…` |
| Halftone screen | `…/245igdi/Halftones/COPA.DEFAULT.600x600.hft` | `24486c453fb90e0e…` |
| Grey profile (output) | `…/245igdi/Profiles/sGrayInv.icm` | `01fd6208f2f04e61…` |
| sRGB profile (input) | `…/245igdi/Profiles/sRGB.icm` | `2b3aa1645779a9e6…` |
| PPD | `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | `ee8ec30066fdc788…` |

(The same tree is reachable at `/usr/lib/cups/filter/KonicaMinolta/…` on this machine.)

**Corpus** (`/root/konica-corpus-full` on the authoring machine): 60 cases,
`manifest.tsv` + `corpus/<id>.{ras,prn,err}`. Every case is `deterministic=yes`, i.e. two
vendor runs produced identical bytes — so no masking is needed.

### Invoking the vendor filter (for re-derivation work)

```sh
F=/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
export PPD=/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd
export CONTENT_TYPE=application/vnd.cups-raster
export FINAL_CONTENT_TYPE=application/vnd.cups-raster
export CUPS_SERVERROOT=/etc/cups CUPS_DATADIR=/usr/share/cups
export LD_LIBRARY_PATH=/usr/local/lib/konica/lib
"$F" 1 corpus <id> 1 "<options from manifest.tsv field 2>" < corpus/<id>.ras
```

---

## 6. Architecture — module map

| File | Lines | Responsibility |
|---|---:|---|
| `src/main.c` | 101 | CLI, option parsing (`Duplex`, `OCM_TonerSave`), `--dump` reader self-check |
| `src/raster.c` / `.h` | 153 / 41 | CUPS raster reader (v1, v2-RLE, v3); `ras_open`/`ras_next_page`/`ras_read_line` |
| `src/encode.c` / `.h` | 290 / 23 | PJL envelope, paper geometry table, band driver, edge wrap, tone, chunking |
| `src/acmm.c` / `.h` | 62 / 42 | Φ: 33³ tetrahedral interpolation; `acmm_load`, `acmm_generate`, `acmm_phi` |
| `src/lutgen.c` / `.h` | 254 / 44 | regenerates the 33³ table from the installed ICC profiles |
| `src/halftone.c` / `.h` | 61 / 35 | `.hft` screen loader + per-pixel dot rule |
| `src/jbig.c` / `.h` | 203 / 29 | self-contained JBIG1 encoder (BIH, QM coder, stripes, padding) |
| `tests/difftest.sh` | 37 | differential test vs captured vendor output |
| `tests/check-reader.sh` | 47 | reader cross-check vs libcups |
| `tests/check-lutgen.sh` | 51 | regenerated CMM table vs the vendor's |
| `tools/dump-lut.gdb` | 37 | captures the vendor's live 33³ table |
| `tools/lutgen-dump.c` | 22 | writes the regenerated table for `check-lutgen.sh` |
| `tools/capture-corpus.sh` | 129 | corpus capture (the L0 harness) |
| `tools/mkraster.c` | 185 | synthetic CUPS raster generator (for the reader check) |
| `tools/prnscan.py` | 246 | scan / diff / cmpmask / manifest |

Call graph for a page:

```
main → enc_job_begin
     → per page: enc_page
            ras_read_line (per row)
            acmm_phi      (per pixel)
            halftone_row  (per row)
            jbig_encode   (per band)
     → enc_job_end
```

---

## 7. The recovered model, stage by stage

### 7.1 PJL envelope (`src/encode.c`)

`\r\n`-terminated PJL lines, wrapped in a UEL. Once per job:

```
\x1b%-12345X@PJL\r\n
@PJL SET COMPRESS=JBIG\r\n
@PJL SET COVER=OFF\r\n
@PJL SET HOLD=OFF\r\n
@PJL SET SECTION=OFF\r\n
@PJL SET COPIES=1\r\n
```

`COPIES=1` is fixed: the vendor ignores CUPS's copies argument (`argv[4]` — copies `1`, `2` and
`5` give byte-identical streams), so the kit hard-codes it too. Once per page:

```
@PJL SET PAGESTATUS=START\r\n
@PJL SET DUPLEX=OFF\r\n                 (or ON + "@PJL SET BINDING=SHORTEDGE|LONGEDGE")
@PJL SET PAPER=<code>\r\n
@PJL SET PAPERWIDTH=<w>\r\n
@PJL SET PAPERLENGTH=<h>\r\n
@PJL SET MEDIASOURCE=AUTO\r\n
@PJL SET MEDIATYPE=PLAIN\r\n
@PJL SET RESOLUTION=<dpi>\r\n
@PJL SET IMAGELEN=<n>\r\n<n raw bytes>  (repeated; one per chunk, n ≤ 32768)
@PJL SET PAGESTATUS=END\r\n
```

Job end: `@PJL EOJ\r\n\x1b%-12345X\r\n`.

`DUPLEX`/`BINDING` are not a plain option mapping — the vendor combines `Duplex=`, `sides=`
and the raster header's own `Duplex`/`Tumble` fields, and names the edge from the page
*orientation* (points wider than tall swaps the two names). The pair is resolved **once per
job, from page 1**, and printed unchanged on every page. Full rule and the 256-case grid are
in `docs/REVERSE-ENGINEERING.md` §4.2.1 and `docs/DEVICE-STREAM.md` §1; the two vendor quirks
(`Duplex=` matched case-insensitively but `sides=` case-sensitively, and `sides=two-sided-long-edge`
becoming `SHORTEDGE`) are reproduced deliberately. `RESOLUTION=600` and `MEDIATYPE=` come from
the header, not the options. `OCM_TonerSave` never appears in the envelope — it only changes
pixels.

### 7.2 Page geometry (`papers[]` in `src/encode.c`)

The table is keyed **only** on the raster header's **`PageSize` in points**: a real
`pdftoraster` A4 page is 4961×7016 pixels and calls itself `A4.Borderless` (a name that is not
in the PPD at all), and it still gets the A4 row. `PageSizeName` and the raster's pixel
dimensions are *not* consulted (that fallback was removed once the points rule was pinned).
A point pair matches an entry in either orientation (`842×595` = `595×842`), and a pair that
matches nothing becomes `PAPER=CUSTOM` (shorter side as the width) as long as the shorter side
is 72..864 pt and the longer at most 1224 pt; outside that the vendor exits 2 with no output
(the kit exits 1, also with no output, before writing anything).

Each point count `pt` yields two pixel counts — published and encoded — by
`base = floor(pt*600/72)`; `pub = base + (pt % 9 == 8)`, `enc = base − (pt % 9 ∈ {1,3,6})`.
`tests/check-geometry.sh` pins the lookup, the custom bounds and the 600 dpi. The full
verified table (`PageSize` points → PJL code → `pw, pl` → image `xd, yd`):

| PageSize | PJL code | pw | pl | xd | yd |
|---|---|---:|---:|---:|---:|
| 16K | MLTM16K | 4608 | 6375 | 4608 | 6375 |
| 8K | MLTM8K | 6308 | 9166 | 6307 | 9166 |
| A3 | A3 | 7016 | 9925 | 7016 | 9924 |
| A4 | A4 | 4958 | 7016 | 4957 | 7016 |
| A5 | A5 | 3500 | 4958 | 3499 | 4957 |
| A6 | A6 | 2475 | 3500 | 2475 | 3499 |
| B4 | JISB4 | 6075 | 8600 | 6075 | 8599 |
| B5 | JISB5 | 4300 | 6075 | 4299 | 6075 |
| B6 | JISB6 | 3025 | 4300 | 3024 | 4299 |
| Comm10 | COM10 | 2475 | 5700 | 2475 | 5700 |
| EnvC6 | C63OF4 | 2692 | 3825 | 2691 | 3825 |
| EnvDL | ENVDL | 2600 | 5200 | 2599 | 5199 |
| EnvPersonal | C6ENV | 2175 | 3900 | 2175 | 3900 |
| Executive | EXECUTIVE | 4350 | 6300 | 4350 | 6300 |
| FabFoldGermanLegal | FLS3 | 5100 | 7792 | 5100 | 7791 |
| FLS_220X330 | FLS4 | 5200 | 7792 | 5199 | 7791 |
| FLS_8D125X13D25 | FLS5 | 4867 | 7958 | 4866 | 7957 |
| FLS_8D25X13 | FLS2 | 4958 | 7792 | 4957 | 7791 |
| FLS_8D5X13D5 | FLS6 | 5100 | 8100 | 5100 | 8100 |
| FLS_8X13 | FLS1 | 4792 | 7792 | 4791 | 7791 |
| Legal | LEGAL | 5100 | 8400 | 5100 | 8400 |
| Letter | LETTER | 5100 | 6600 | 5100 | 6600 |
| Statement | INVOICE | 3300 | 5100 | 3300 | 5100 |
| Tabloid | LEDGER | 6600 | 10200 | 6600 | 10200 |

Point keys (`px × py`), in table order: 16K 553×765 · 8K 757×1100 · A3 842×1191 · A4 595×842 ·
A5 420×595 · A6 297×420 · B4 729×1032 · B5 516×729 · B6 363×516 · Comm10 297×684 ·
EnvC6 323×459 · EnvDL 312×624 · EnvPersonal 261×468 · Executive 522×756 · FLS3 612×935 ·
FLS4 624×935 · FLS5 584×955 · FLS2 595×935 · FLS6 612×972 · FLS1 575×935 · Legal 612×1008 ·
Letter 612×792 · Statement 396×612 · Tabloid 792×1224. Every row was re-verified against the
running vendor filter by rewriting the raster's `PageSize` field and diffing the output.

`RESOLUTION=600` is hard-coded: the vendor ignores the raster's `HWResolution` (300 dpi and
1200 dpi rasters give byte-identical streams). The kit warns on stderr if the raster disagrees.

### 7.3 The ACMM colour-world stage, Φ (`src/acmm.c`)

For non-uniform input the vendor first runs the page through a 3×1 ACMM filter element
(`PCM_lfFilter8Eval3x1`) over the contiguous 8-bit source:

```
tone[x] = max(0, Φ(src[x], src[x+1], src[x+2]) − (OCM_TonerSave ? 10 : 0))
```

* stride `S = round_up(width, 4)`; the taps at the right edge **wrap into the next row**
  (this is what the `037-size-16K` discrepancy turned out to be);
* `Φ` is **tetrahedral interpolation of a 33³ byte table**:

```
bcode(s)  = ((s & 7) << 5) + (s >> 3)          # fraction in bits 7..5, lattice in 4..0
base      = (s0>>3)*1089 + (s1>>3)*33 + (s2>>3)
offsets   = {0,1089,33,1122,1,1090,34,1123}    # strides 33², 33, 1
out       = ( Σ weight·corner * 0x80808081 ) >> 39        # i.e. /255
```

* `OCM_TonerSave`: **absent or TRUE** subtracts 10 from Φ; FALSE passes through. This is
  proven twice over by the corpus — `034-tonersave-true` and `035-tonersave-false` differ
  (422 252 vs 375 571 bytes), and the ~26 textured/gradient cases `003`–`028`, which do
  *not* set the option, only reproduce byte-exactly if "absent" subtracts 10.
* Sanity property, verified over all 256 uniform inputs:
  `Φ(g,g,g) = 255` for `g=0`, `0` for `g=255`, and `254 − g` otherwise. Hence a black
  input yields tone 245 and a white input tone 0.

### 7.4 The 33³ table generator (`src/lutgen.c`) — the newest work

See [§8](#8-the-cmm-table-generator-in-detail).

### 7.5 Halftone (`src/halftone.c`)

```
dot(x, y) = ( tone[x] >= screen[page_row % H][x % W] )     # unsigned compare
```

* **1-bit rasters never enter this stage.** The vendor copies their bits straight into the
  page image — bit set is ink — cropped to `Xd`×`Yd`, with the uncovered area dot-free and the
  colour space ignored (`cs=0` and `cs=3` behave identically). Measured on A6 (`Xd` 2475,
  `Yd` 3499): all-ink → 2475×3499 ink pixels, all-white → none, checker → half. `src/encode.c`
  branches on `bits_per_pixel == 1` before the CMM; the corpus is all 8bpp, so the fuzzer is
  what exercised this. The copy is exact while no row has ink in the two bits at or just past
  the image width (`x == Xd` / `Xd + 1`); when one does, the vendor degenerates into an
  unexplained deterministic halftone-like pattern (see `docs/DEVICE-STREAM.md` §3). The kit
  stays a copy and the fuzzer calls that a known vendor defect; this PPD is 8-bit anyway.
* screen `H=W=96`, from **plane 0** of `COPA.DEFAULT.600x600.hft`
  (`.hft` = `tHsG` magic, version 5, record at `0x0c` → offset `0x44`, sub-header
  `{u16 w, u16 h}` at `0x48`, data at `0x4c`; 9216 bytes, values 1…255, none zero);
* row phase is the **page row** (not the band row), column phase `x % 96`;
* consistency check: 82.87 % of the screen cells are ≤ 245, which matches the measured
  82.86 % dot coverage of a solid (black) page — i.e. `Φ(0,0,0)=255 → tone=245`;
* when the screen cannot be read `enc_open()` fails and the filter exits non-zero with an
  empty stdout — no partial job is ever emitted.

### 7.6 JBIG1 encoder (`src/jbig.c`)

* one bi-level image per page, cropped as in §7.2 (A4 raster 4958×7017 → image 4957×7016);
* 20-byte BIH (vendor layout):

  ```
  00 00 01 00              DL=0 D=0 planes=1
  Xd (u32 BE)              image width
  Yd (u32 BE)              image height
  l0 = 0x00000080          128 lines per stripe
  mx=00 my=00              no adaptive-template movement
  order  = 03              ILEAVE | SMID
  options= 0x48            LRLTWO | TPBON
  ```

* each 128-line stripe is QM-coded with the two-line LRLTWO template and TPBON typical
  prediction, terminated with the stripe marker `FF 02` (SDNORM); the context probability
  state **persists across stripes**;
* two behaviours are essential for byte-exactness: trailing zero bytes must **not** be
  stripped before `FF 02`, and the finished BIE is **padded to a multiple of 4**;
* the pad is the one place the kit knowingly diverges. The vendor fills those 0–3 bytes with
  uninitialised `malloc()`ed memory: on a real 2-page A4 document (page streams 77 476 and
  44 036 bytes — both under glibc's 128 KiB `mmap` threshold) page 2's pad bytes were `42 be`,
  exactly page 1's bytes at offsets 44 034–44 035, because page 2's allocation reused the
  block freed after page 1; in corpus cases 029/031/033 the streams are ≥ 128 KiB, which
  glibc serves with `mmap` (freshly zeroed), and the vendor's pad bytes were `00 00`. The
  bytes follow the last SDNORM and are never decoded, so `src/jbig.c` writes deterministic
  zeros. A byte-exact diff of a real document should mask the trailing
  `len % 4 ? 4 - len % 4 : 0` bytes of any page whose stream is not already 4-aligned;
* verified against jbigkit-2.1 as an oracle (never vendored) for the coder itself; the
  end-of-image bytes are compared against the vendor, since jbigkit's stream ends differently.

Measured page-image sizes: blank A4 = **132 bytes**, solid A4 = **132 452 bytes**
(the whole `.prn` is 518 / 132 939 bytes once PJL framing and `IMAGELEN` chunk headers are
included).

---

## 8. The CMM table generator in detail

This is the piece completed most recently; it is reproduced here in full because it is the
subtle one.

### 8.1 Why it exists at all

The vendor ships no colour-world table. Its CMM builds a colour conversion at start-up and
samples it into a 33³ byte grid. An `strace -f -e trace=openat` of the live filter shows
exactly one profile opened:

```
…/245igdi/Profiles/sGrayInv.icm
…/245igdi/Filters/245igdirf.ocm, …/Halftones/COPA.DEFAULT.600x600.hft
```

The **input** space (matrix + shaper TRCs) is compiled into the binary. It is a D50 sRGB,
and it agrees with the installed `sRGB.icm` to within a ULP of `float` — so the kit reads
`sRGB.icm` from disk rather than baking anything in.

### 8.2 The pipeline

```
        ┌──────────────── the vendor's conversion, per grid point ─────────────────┐
x = 8·i ─▶ rTRC(sRGB.icm) ─▶ colourant Y row ─▶ XYZ ─▶ f → L* ─▶ kTRC⁻¹(sGrayInv.icm) ─▶ byte
i=0..32     1024-entry          (3 values)        Y/Yn   cube root  256-entry           round
```

| # | stage | formula | source |
|---|---|---|---|
| 1 | input TRC ×3 | `t_k = curve(x_k)` | `sRGB.icm` `rTRC`/`gTRC`/`bTRC` (1024 `curv` entries, all identical) |
| 2 | matrix → XYZ (only Y is used) | `Y = Σ M[1][k]·t_k` | `sRGB.icm` `rXYZ`/`gXYZ`/`bXYZ`, Y components |
| 3 | XYZ → L\* | `fy = f(Y / 127.5)`, `L = (1.16·fy − 0.16)·255` | vendor constants |
| 4 | L\* → grey | `g = kTRC⁻¹_sGrayInv(L)` | `sGrayInv.icm` `kTRC` (256 `curv` entries, decreasing) |
| 5 | round | `v ≤ 0 → 0`; `v ≥ 254.5 → 255`; else `trunc(v + 0.5)` | `PCM_fVArrayCopy` |

with

```
f(w) = w^(1/3)                  for w >  0.008856452070176601
f(w) = (841·w)/108 + 16/116     otherwise
```

Only the **Y** row of the matrix matters: stage 4 takes the first PCS component, and
`PCM_lfPCS2Gray` with signature `'Lab '` returns L\* and nothing else — X and Z never
influence the table.

### 8.3 Single-precision semantics (the part that decides byte-exactness)

The engine's stages run in IEEE single precision (SSE). These details are all load-bearing
and are reproduced literally in `src/lutgen.c`:

* **TRC scaling.** A `curv` entry `v` becomes `(float)v * (255.0f/65535.0f)` — the vendor
  precomputes the reciprocal and *multiplies*. Computing `v*255.0/65535.0` instead is a ULP
  out on 5 of the 1024 entries (and 1 of the 256). With the reciprocal form the generator's
  tables are **identical to the vendor's, entry for entry**.
* **TRC lookup.** `w = (float)x / 255.0f`; clamp `w ≤ 0 → tab[0]`, `w ≥ 0.999999 → tab[n−1]`;
  else `p = (double)w · (n−1)`, index `i = trunc(p)`, `frac = p − i`; result
  `(float)( (double)tab[i] + frac · (double)(float)(tab[i+1] − tab[i]) )`. The difference is
  formed in **single**, the product/sum in **double**.
* **Matrix.** Accumulate left to right in `float`: `((0 + M[1][0]·t0) + M[1][1]·t1) + M[1][2]·t2`.
* **Matrix normalisation.** The colourant Y values are divided by their own sum, then by 2
  (Y rides the 0…127.5 scale): `M[1][k] = 0.5f · ((float)Y_k / ΣY)`. For `sRGB.icm` this
  gives `0.111247599, 0.358447522, 0.030304879` — the D50 sRGB luminance coefficients.
* **Lab.** Ratios and the L\* combination are `float`; the cube root is
  `(float)pow((double)w, 1.0/3.0)` — the platform's libm `pow`, exactly as the vendor calls
  it (the vendor links `libm.so.6`).
* **Inverse TRC.** `kTRC` is monotonically **decreasing**; find the bracket and compute
  `g = 1.0f/(a−b)`, `result = ( (a−v)·(i+1) + (v−b)·i )·g` in `float`.
* **Rounding.** As in stage 5 above.

Two build consequences are non-negotiable and are set in the `Makefile`:

* **`-ffp-contract=off`** — GCC/Clang must not fuse a multiply into an FMA; that would
  change the single-precision results.
* **`-lm`** — for `pow`, the same libm function the vendor calls.

`float` must be IEEE-754 binary32 (true on x86-64 and arm64, i.e. glibc and musl); no x87
excess precision is assumed.

### 8.4 Evidence trail

1. `PCM_fLUTCreate` / `PCM_fXFilter8Set` located by a hardware watchpoint on the element's
   grid (`ELEM+0x1404`). Element layout:
   `[0x4c0 header][corner offsets 2³·4][3·257·4][3·256][3·8][grid 33³]`.
2. Stage call order logged as
   `[init] → [appendA2B] → [addTRC] → [addMatrix] → [appendB2A] → [addPCS2Gray] → [addTRC] → [LUTCreate N=3 M=1 range=65536]`.
3. Every operand dumped and matched against the installed profiles (three identical input
   TRCs; the normalised colourant Y row; the inverse grey TRC).
4. The conversion re-evaluated in Python and in C reproduces the live grid exactly:
   **0 differing bytes of 35 937**, and the captured *float* table to < 5·10⁻⁵.
5. `PCM_fReal2Int` uses the magic `1.5·2³⁶` + `shr $0x10` → **truncation** for positives;
   rounding elsewhere is an explicit `+ 0.5`.

### 8.5 Failure modes and configuration

`acmm_generate()` returns `NULL` if either profile is missing, unreadable, oversized
(> 8 MiB), not an ICC file (`acsp` magic), lacks `rTRC` or the colourant `XYZ ` tags, carries
a non-decreasing `kTRC`, or yields a degenerate (zero-sum) matrix. `enc_open()` calls it
*before* the first PJL byte is written, so such a failure is fatal: the filter prints one line
to stderr and exits non-zero with **nothing on stdout**. The historical flat-tone
approximation is reachable only through the explicit `KONICA_FLAT_TONE=1` escape hatch.

| Variable | Meaning | Default |
|---|---|---|
| `KONICA_HFT` | halftone screen `.hft` | vendor install path |
| `KONICA_SRGB` | input matrix/shaper profile | `…/Profiles/sRGB.icm` |
| `KONICA_GRAY` | output grey profile | `…/Profiles/sGrayInv.icm` |
| `KONICA_LUT` | captured 33³ table — **override** | unset → table is regenerated |

---

## 9. Key vendor constants and addresses

Recovered from `245igdirf`'s `.rodata` and used literally in `src/lutgen.c`:

| Address | Value | Role |
|---|---|---|
| `0x468580` | `255.0f` | TRC input divisor |
| `0x4685a8` | `0.999999` (double) | TRC input clamp |
| `0x4685b0` | `255.0f` | VArrayCopy ceiling |
| `0x4685b4` | `0.5f` | round term |
| `0x4689b8/0x4689bc/0x4689c0` | `122.9355f / 127.5f / 105.1747f` | PCS white D50 × 127.5 |
| `0x4689e0` | `0.13793103f` | 16/116 |
| `0x4689ec` | `0.008856452070176601f` | (6/29)³ threshold |
| `0x4689f0` | `0.3333333333333333` (double) | cube-root exponent for `pow` |
| `0x4689f8 / 0x4689fc` | `841.0f / 108.0f` | linear toe |
| `0x468a00 / 0x468a04` | `1.16f / 0.16f` | L\* coefficients |
| `0x4689c8` | `255.0f` | L\* scale |

Other useful addresses / ABI facts:

| Symbol | Address | Note |
|---|---|---|
| `PCM_lfFilter8Eval3x1` | `0x4512d0` | Φ inner loop head at `0x4513d3` |
| `PCM_fXFilter8Set` | `0x4431cd` | builds the element |
| `PCM_fColorConversionInit` | `0x43e75e` | conversion stages |
| `PCM_fLUTCreate` | `0x445c41` | N=3 M=1 levels=33 |
| `PCM_fColorWorldCreateFromConversion` | `0x43634e` | world construction |
| `HalftoneImage1c1b` | `0x4133fd` | returns `0x41370e` |
| `ImoTextureImageEx` | `0x19b60` (ocm) | pointwise LUT then halftone |

`HalftoneImage1c1b` ABI: `rdi` = halftone record, `rsi` = tone/source descriptor,
`rdx` = bitmap/destination descriptor, `rcx` = phase struct; `arg4[0]` = column phase,
`arg4[1]` = page row. Descriptor fields: `+0x10` width, `+0x18` height, `+0x20` stride,
`+0x28` buffer pointer. Tone pointer is `*(long*)(rsi+0x28)`.

---

## 10. Reading the vendor binary (toolkit)

```sh
F=/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
O=/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/mtorf.ocm
nm "$F" | grep -iE 'halftone|imx' > /tmp/filter.syms
objdump -d --no-show-raw-insn "$F" > /tmp/filter.asm
objdump -d --no-show-raw-insn "$O" > /tmp/ocm.asm
```

Capture recipes:

* **LUT:** `tools/dump-lut.gdb` breaks at `0x4512d0`, walks the element at `ELEM+0x78` →
  `ELEM+0x1404`, and writes `/tmp/lut33.bin` (35 937 bytes).
* **Tone rows / bitmaps:** break at `*0x4513d3` (`r12` = source row, `*(long*)($rsp-0x58)` =
  destination row, `r8` = element); break at `*0x4512d0` for the descriptors.

Environment gotchas that cost time:

* gdb's `printf` does **not** support `%r` for floats — use `%f`.
* A gdb `printf` placed after `run` in a `-batch -x` script does not always execute; prefer
  `objdump -s -j .rodata` or breakpoints.
* `-lm` and `-ffp-contract=off` are required for the generator (see §8.3).

---

## 11. Test corpus

60 cases, all `deterministic=yes` (no masking anywhere):

* **Framing/geometry:** `001-simplex-blank`, `002-simplex-solid`, `003/004` gradients,
  `005` 8-step ramp, `006` row ids, `007` column ids, `008` frame;
* **Probe pattern:** `009`–`016` (single dots / a box at chosen coordinates — these pinned
  the band/stride/edge-wrap behaviour);
* **Texture:** `017`–`028` (h/v lines, checkers, noise at several frequencies);
* **Duplex / page counts:** `029`–`033` (2- and 3-page, long and short edge);
* **Options:** `034-tonersave-true`, `035-tonersave-false`, `036-res-600x600dpi`;
* **Paper sizes:** `037-simplex-16K` … `060-size-Tabloid` (all 24 supported sizes).

`036` is the only resolution case (600×600 dpi). The corpus exercises **8-bit grey** raster
only; the *reader* is additionally cross-checked on 1-bit grey and RGB/CMYK.

Two gaps in the corpus that real documents exposed (both now covered by tests, see §7.2, §7.6
and `docs/ALPINE-PORT-STATUS.md` §7.2):

* every case was built from the PPD with `cupsRasterInterpretPPD()`, so `PageSizeName` is
  always a real PPD value and the pixel width is the corpus width (4958 for A4). Real
  `pdftoraster` output is v3, 4961×7016 and self-described as `A4.Borderless`. A
  `PageSizeName`-keyed table therefore passed 60/60 while being wrong on every real page;
* every page stream in the corpus happens to be a multiple of 4 bytes, so the vendor's BIE
  padding behaviour was never exercised.

`tests/check-geometry.sh` now covers the first gap directly. The second is documented rather
than reproduced (it is uninitialised memory; see §7.6).

Three further gaps were found by the differential fuzzer (`tests/fuzz-difftest.sh`) after the
corpus and the real documents were already passing: the orientation-dependent
`DUPLEX`/`BINDING` rule (resolved once per job, from page 1 — §7.1), the 1bpp pass-through
(§7.5), and the point→pixel conversion for sizes that are not in the table (§7.2). Each is now
pinned by a test, and the fuzzer classifies heap-dependent vendor output separately (§12.5).

---

## 12. Known limitations / what is *not* verified

Honest statement of scope:

1. **Paper sizes, raster formats and option combinations absent from the corpus** are handled
   by the model but not exercised end-to-end by the *corpus*; the fuzzer and the real-document
   test cover many of them. The filter path converts 8-bit and 1-bit rasters (1bpp bypasses
   the CMM and the halftone entirely — §7.5); the corpus itself exercises 8-bit grey only.
2. **Non-600 dpi** resolutions are not exercised (`1200x600.hft` exists and has a 192×96
   plane 0, but only the 600×600 path is tested). The vendor forces `RESOLUTION=600` whatever
   the raster says (verified), so this is a fidelity limit of the device path, not of the kit.
3. **The LUT generator is verified against the vendor's own installed profiles.** A different
   profile pair would legitimately produce a different table; `make check-lutgen` covers the
   vendor set.
4. **CUPS installation** is now tested on Alpine **without a printer** (`docs/ALPINE.md`,
   `tools/step4-cups-chain.sh`): the chain, the native PPD, the fail-safe and an isolated
   `cupsd` all pass (§3.4 of `docs/ALPINE-PORT-STATUS.md`). **Hardware printing is still not
   attempted** — the kit has never sent a byte to a printer. One PPD-level finding: the
   generated PPD must say `*cupsManualCopies: True`, or `lp -n 2` prints a single copy.
5. **Two places where the vendor reads uninitialised heap** are deliberately not reproduced,
   because no deterministic filter can: (a) the 0–3 BIE pad bytes after the final SDNORM
   (§7.6); (b) when a raster row is not a multiple of 4 bytes, the byte just past the row,
   which the CMM's right-edge window reads, changing the page's **last column**. In both cases
   the vendor's own output moves with `MALLOC_PERTURB_`; the kit writes deterministic zeros,
   `tools/prnscan.py padcmp` masks the pad bytes, and `tests/fuzz-difftest.sh` reports a case
   whose vendor output is heap-dependent as vendor-nondeterministic rather than divergent.

There are **no known correctness gaps** inside the tested scope: every byte of every corpus
stream is reproduced, 2 of the 3 real documents are byte-identical end-to-end (the third differs
only in never-decoded pad bytes, §12.5), and the vendor's copies argument is verified ignored
(`COPIES=1` is faithful, not a gap).

---

## 13. Working in this environment (notes for an agent)

* The repo lives at `/home/spot/Downloads/konica-native-musl`; the corpus at
  `/root/konica-corpus-full`.
* The vendor tree is under `/usr/local/lib/konica/KonicaMinolta/245igdi/`.
* A **musl test rootfs** exists at `/initrd/mnt/tmpfs/tmp/opencode/alpine/rootfs`
  (Alpine 3.24.2, recreated 2026-10-16) and holds `/corpus`, the vendor profiles and the `.hft`;
  recreate it from `alpine-minirootfs-3.24.2-x86_64.tar.gz` as in
  `docs/ALPINE-PORT-STATUS.md` §2 if it is gone. `tools/step3-alpine.sh` runs the whole musl
  build + `make check` + `verify-on-target` in one shot. Build and test with:

  ```sh
  R=/initrd/mnt/tmpfs/tmp/opencode/alpine/rootfs
  tar -C <repo> -cf - --exclude=.git --exclude=konica-native . | tar -C "$R/work/knm" -xf -
  chroot "$R" /usr/bin/env -i PATH=/usr/bin:/bin HOME=/root sh -c \
    'cd /work/knm && make && tests/difftest.sh /corpus'
  ```

  Remove any stale **glibc** binaries (`tools/mkraster`, `tests/libcups_dump`,
  `konica-native`, `tools/lutgen-dump`) before copying — a glibc binary inside the musl
  chroot fails to run and makes `check-reader.sh` report `SKIP` instead of `PASS`.
* `tools/lutgen-dump`, `tools/mkraster`, `tests/libcups_dump` and `konica-native` are build
  artifacts and are in `.gitignore`. **Never commit them.**
* Prefer editing with small, focused changes and re-running `difftest` after each — it takes
  a couple of minutes but is the ground truth.

---

## 14. Suggested next steps (optional)

The project is complete. If work continues, these are the natural directions, in order of
value:

1. **Broaden the corpus** to cover the gaps in §12 (other resolutions, 1-bit and colour
   rasters through the *filter*, more duplex bindings) using `tools/capture-corpus.sh`
   against the vendor filter, then re-run `difftest.sh`. The fuzzer already explores these
   axes; promoting its interesting seeds into the corpus would make them permanent.
2. **Portability**: the model assumes IEEE binary32 `float`. If a 32-bit x87 target ever
   mattered, the generator would need explicit rounding control.
3. **Packaging**: a distro-ready `install` target and the Alpine CUPS integration described
   (but not tested) in `docs/ALPINE.md`.

---

## 15. Where to look

| Document | Contents |
|---|---|
| **`docs/COMPLETE-NOTES.md`** | the master record — the whole project end to end |
| `docs/LUT-GENERATOR.md` | the 33³ table generator: pipeline, constants, semantics, evidence |
| `docs/REVERSE-ENGINEERING.md` | long-form derivation and audit trail |
| `docs/DEVICE-STREAM.md` | condensed device-stream format spec |
| `docs/ALPINE.md`, `docs/ALPINE-PORT-STATUS.md` | musl/Alpine port status and recipe |
| `README.md` | project front page |
| `README-RE.md` | historical (pre-RE) README, kept for context |

---

## 16. Glossary

* **ACMM** — the vendor's colour-management module inside the filter.
* **BIE** — Bi-level Image Entity: the JBIG1 header + entropy-coded data.
* **CMM** — Colour Management Module; here, the code that builds and samples the 33³ table.
* **`curv` / `XYZ `** — ICC tag types (uInt16 curve table / s15Fixed16 XYZ triple).
* **LRLTWO, TPBON** — JBIG1 coding options (two-line template; typical prediction).
* **`OCM_TonerSave`** — a CUPS option that lowers tone by 10 before halftoning; **absent
  behaves as TRUE**.
* **Φ (Phi)** — the recovered 3-input/1-output colour function, implemented as tetrahedral
  interpolation of a 33³ byte table.
* **SDNORM** — the JBIG1 stripe-termination marker (`FF 02`).
* **UEL** — Universal Exit Language (`\x1b%-12345X`), the PJL job delimiter.
