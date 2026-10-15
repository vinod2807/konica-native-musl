# konica-native-musl

A small, dependency-free C reimplementation of the device-stream side of the Konica
Minolta bizhub 206 CUPS filter (`245igdirf`). It reads a CUPS raster page on stdin and
writes the exact byte stream the printer expects (PJL envelope + one JBIG1 bitmap per
page) to stdout.

The whole pipeline — page framing, page geometry, colour conversion, halftoning and JBIG1
entropy coding — was reconstructed from disassembly and runtime captures, and is
**byte-exact on all 60 cases** of the captured differential corpus.

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
make check-reader                                  # reader vs libcups: 27/27
make check-lutgen                                  # regenerated CMM table vs the vendor's
tests/difftest.sh /path/to/konica-corpus
```

The differential test reproduces the vendor filter's captured output byte-for-byte:
`PASS: 60   FAIL: 0` (verified on glibc and on Alpine/musl). It needs no captured
table: the colour-management table is regenerated from the installed ICC profiles.

## Runtime data

Two things are loaded at runtime rather than compiled in:

| Variable | What | Default |
|---|---|---|
| `KONICA_HFT` | the halftone screen `.hft` | the vendor install path |
| `KONICA_SRGB` | input matrix/shaper profile `sRGB.icm` | the vendor install path |
| `KONICA_GRAY` | output grey profile `sGrayInv.icm` | the vendor install path |
| `KONICA_LUT` | a captured 33³ table (optional override) | *unset* — the table is generated |

The CMM table is not a file the vendor ships: its colour engine generates it from
the two profiles at start-up, and `src/lutgen.c` reproduces that generator exactly
(byte-identical on glibc and musl). `KONICA_LUT` remains as an explicit override and
cross-check; `tools/dump-lut.gdb` captures the vendor's own table.

If the profiles cannot be read, the filter still runs but falls back to the
flat-tone approximation (the historical 7/60 subset), printing the reason once.
See `docs/LUT-GENERATOR.md`.

## Layout

```
src/encode.c      PJL framing, page assembly, bands, right-edge wrap, OCM_TonerSave
src/acmm.c        the ACMM 3-tap colour-world stage (33^3 LUT tetrahedral interpolation)
src/lutgen.c      regenerates the 33^3 table from the installed ICC profiles
src/halftone.c    .hft screen loader + per-pixel dot rule
src/jbig.c        self-contained JBIG1 encoder
src/raster.c      pure-libc CUPS raster reader (v1 / v2-RLE / v3)
docs/             format spec and full reverse-engineering notes
tools/            corpus capture, LUT dump recipe, raster generator, stream scanner
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
