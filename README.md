# konica-native-musl

A small, dependency-free C reimplementation of the device-stream side of the Konica
Minolta bizhub 206 CUPS filter (`245igdirf`). It reads a CUPS raster page on stdin and
writes the exact byte stream the printer expects (PJL envelope + one JBIG1 bitmap per
page) to stdout.

The whole pipeline — page framing, page geometry, colour conversion, halftoning and JBIG1
entropy coding — was reconstructed from disassembly and runtime captures, and is
**byte-exact on all 60 cases** of the captured differential corpus.

It is also byte-exact on real `pdftoraster` output: three real PDFs (1, 4 and 2 pages) were
run through the vendor filter and through this kit and compared byte for byte. Two are
identical; the third differs in exactly 2 bytes, which are the 4-byte alignment padding at
the end of a bitmap — bytes the device never decodes, and which the vendor fills with
uninitialised heap memory (see `docs/ALPINE-PORT-STATUS.md` §7.2). No printer was involved
in any of this: both filters wrote to a file.

* Pure C11, libc only — no third-party libraries, builds on glibc and musl/Alpine.
* Self-contained: no GPL/third-party encoder is vendored (the JBIG1 QM coder is
  implemented from the ITU-T T.82 tables in `src/jbig.c`).
* Fails safe: output goes to stdout, so the caller can redirect it to a file or a
  printer; the filter itself never opens a device.

## Build

```sh
make
```

## Test

```sh
make check                # reader, CMM table, geometry, MEDIATYPE, hardening — no vendor needed
make check-reader         # reader vs libcups: 27/27
make check-lutgen         # regenerated CMM table vs the vendor's
make check-geometry       # points -> PAPER code + pixels, CUSTOM bounds, forced 600 dpi
make check-mediatype      # PJL MEDIATYPE table (golden, vendor-free)
make check-errors         # a broken runtime install must fail with nothing on stdout
make check-determinism    # CORPUS=...: same raster twice, once with MALLOC_PERTURB_
make check-difftest       # CORPUS=...: byte-exact against the captured corpus
make check-real           # real PDFs through pdftopdf + pdftoraster
make check-fuzz           # randomised differential fuzzing (FUZZ_CASES, FUZZ_SEED)
make check-asan           # AddressSanitizer + UBSan build over the vendor-free tests
```

`make check` needs neither the vendor filter nor a corpus. The tests that compare against
the vendor need it installed (and a readable PPD); `CORPUS=` points the corpus tests at a
captured corpus.

To compare a real document by hand — and keep the raster so a second machine can re-check it —
use `tools/compare-real.sh` on the host where the vendor filter runs:

```sh
PPD=/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd
tools/compare-real.sh mydoc.pdf "$PPD" ./real-compare   # writes .ras/.opts/.vendor.prn/.native.prn
tools/verify-on-target.sh ./real-compare ./konica-native # re-check the captured rasters anywhere
```

`compare-real.sh` runs `pdftopdf` → `pdftoraster` → both filters and compares; `PASS` means
byte-identical or differing only in the never-decoded BIE pad bytes (`prnscan.py padcmp`). A
non-default `OPTS` is folded into the output stem so two option sets on the same PDF (e.g. long-
vs short-edge duplex) do not overwrite each other. Nothing is ever sent to a printer.

The differential test reproduces the vendor filter's captured output byte-for-byte:
`PASS: 60   FAIL: 0` (verified on glibc and on Alpine/musl). It needs no captured
table: the colour-management table is regenerated from the installed ICC profiles.

The fuzzer generates rasters that vary along the axes where defects actually lived — raster
version, page size in points vs the page-size *name*, dpi, bit depth, pages of mixed
geometry — and diffs both filters. It found the orientation-dependent `DUPLEX`/`BINDING`
rule (which the vendor resolves once per job, from page 1), the 1bpp pass-through, and the
point→pixel conversion for sizes outside the table. Two kinds of case are reported separately
rather than as divergences, because no deterministic filter can match the vendor there: a case
whose *vendor* output moves under `MALLOC_PERTURB_` (the vendor reads uninitialised heap), and
a 1bpp raster wider than the image with ink in the two bits at or just past the image width
(a vendor defect — the vendor abandons the copy and emits an unexplained pattern). Neither can
arise with this device: its PPD declares `cupsBitsPerColor 8`.

## Runtime data

Two things are loaded at runtime rather than compiled in:

| Variable | What | Default |
|---|---|---|
| `KONICA_HFT` | the halftone screen `.hft` | the vendor install path |
| `KONICA_SRGB` | input matrix/shaper profile `sRGB.icm` | the vendor install path |
| `KONICA_GRAY` | output grey profile `sGrayInv.icm` | the vendor install path |
| `KONICA_LUT` | a captured 33³ table (optional override) | *unset* — the table is generated |
| `KONICA_FLAT_TONE` | accept the old flat-tone approximation instead of failing | *unset* |

The CMM table is not a file the vendor ships: its colour engine generates it from
the two profiles at start-up, and `src/lutgen.c` reproduces that generator exactly
(byte-identical on glibc and musl). `KONICA_LUT` remains as an explicit override and
cross-check; `tools/dump-lut.gdb` captures the vendor's own table.

The screen and the table are both loaded in `enc_open()`, **before** the first byte is
written to stdout. If either cannot be read the filter prints the reason and exits
non-zero with **nothing on stdout**, so a broken install cannot produce a plausible-looking
half-correct job. The historical flat-tone approximation is still available, but only
explicitly, with `KONICA_FLAT_TONE=1`; see `docs/LUT-GENERATOR.md`.

## Layout

```
src/encode.c      PJL framing, page assembly, bands, right-edge wrap, OCM_TonerSave
src/acmm.c        the ACMM 3-tap colour-world stage (33^3 LUT tetrahedral interpolation)
src/lutgen.c      regenerates the 33^3 table from the installed ICC profiles
src/halftone.c    .hft screen loader + per-pixel dot rule
src/jbig.c        self-contained JBIG1 encoder
src/raster.c      pure-libc CUPS raster reader (v1 / v2-RLE / v3)
docs/             format spec and full reverse-engineering notes
tools/            corpus capture, LUT dump recipe, raster generator, stream scanner,
                  real-document comparison (compare-real.sh, verify-on-target.sh)
tests/            differential + reader cross-check harnesses
```

## Documentation

* `HANDOFF.md` — **self-contained handoff report** for an AI assistant or a new engineer:
  status, full recovered model, exact verification commands, constraints and gaps.
* `docs/COMPLETE-NOTES.md` — **the master record**: the whole project end to end (start here).
* `docs/REVERSE-ENGINEERING.md` — the long-form derivation and evidence trail.
* `docs/LUT-GENERATOR.md` — the recovered 33³ CMM table generator.
* `docs/DEVICE-STREAM.md` — the condensed device-stream format spec.
* `docs/ALPINE.md`, `docs/ALPINE-PORT-STATUS.md` — musl/Alpine port status.
* `README-RE.md` — historical project README (superseded by `docs/`).
