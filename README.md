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
KONICA_LUT=/path/to/lut33.bin tests/difftest.sh /path/to/konica-corpus
```

The differential test reproduces the vendor filter's captured output byte-for-byte:
`PASS: 60   FAIL: 0` (verified on glibc and on Alpine/musl).

## Runtime data

Two things are loaded at runtime rather than compiled in, because they are vendor
runtime tables (not redistributable, and not reimplemented):

| Variable | What | Default |
|---|---|---|
| `KONICA_HFT` | the halftone screen `.hft` | the vendor install path |
| `KONICA_LUT` | the 33³ colour-world lookup table | *none* — a flat-tone fallback is used when unset |

`tools/dump-lut.gdb` reproduces `KONICA_LUT` from the running vendor filter. When it is
not supplied the filter still runs, but only flat-tone pages are exact (the historical
7/60 subset). See `docs/REVERSE-ENGINEERING.md` §9.5.

## Layout

```
src/encode.c      PJL framing, page assembly, bands, right-edge wrap, OCM_TonerSave
src/acmm.c        the ACMM 3-tap colour-world stage (33^3 LUT tetrahedral interpolation)
src/halftone.c    .hft screen loader + per-pixel dot rule
src/jbig.c        self-contained JBIG1 encoder
src/raster.c      pure-libc CUPS raster reader (v1 / v2-RLE / v3)
docs/             format spec and full reverse-engineering notes
tools/            corpus capture, LUT dump recipe, raster generator, stream scanner
tests/            differential + reader cross-check harnesses
```

## Documentation

* `docs/REVERSE-ENGINEERING.md` — how every stage was derived and verified (start here).
* `docs/DEVICE-STREAM.md` — the condensed device-stream format spec.
* `docs/ALPINE.md`, `docs/ALPINE-PORT-STATUS.md` — musl/Alpine port status.
* `README-RE.md` — historical project README (superseded by `docs/`).
