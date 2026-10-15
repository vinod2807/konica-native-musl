# konica-native-musl — reverse-engineering kit for the 245igdirf filter

> **STALE — read `docs/REVERSE-ENGINEERING.md`, `docs/DEVICE-STREAM.md`,
> `docs/ALPINE.md` and `docs/ALPINE-PORT-STATUS.md` first; they supersede this file.**
> This README describes the project *before* the device-stream side was reverse-engineered.
> Current facts: the **whole pipeline is implemented and byte-exact on all 60 corpus cases**
> (`src/encode.c`, `src/acmm.c`, `src/halftone.c`, `src/jbig.c`), verified on glibc **and**
> Alpine/musl; the `OCM_TonerSave` default is **resolved** (absent behaves as TRUE); the only
> non-self-contained piece is the 33³ colour-world LUT, which the kit loads from a runtime
> dump (`KONICA_LUT`, see `docs/REVERSE-ENGINEERING.md` §9.5). The sections below are kept
> for history.

**Status.** The raster-in side is done and verified; the device-stream side (everything
the vendor filter does between raster and printer bytes) is **not implemented** and cannot
be until there are real captures to study. This kit builds those captures and the tools to
read them. Nothing here prints yet.

## What was verified (in a sandbox, not on your hardware)

| Piece | Verified how |
|---|---|
| `tools/mkraster.c` | Builds against libcups 2.4; writes v2 (RLE) and v3 (uncompressed) rasters; 1-bit, 8-bit gray, RGB, CMYK |
| `src/raster.c` (libc-only reader) | Identical headers and pixel hashes to libcups on 27 synthetic cases: 9 colour/depth combos x {v3, v2 RLE, v1} (`make check-reader`) |
| `tools/capture-corpus.sh` | Run end-to-end with a **stand-in** filter and a minimal PPD (35 cases, determinism rerun, manifest, tarball) |
| `tools/prnscan.py` | Run on a synthetic stream: found UEL/PJL lines, a zlib payload, a length field, and a one-byte diff |
| `tests/difftest.sh` | 35/35 PASS with a perfect candidate, 0/35 with the stub (as expected) |

**Not verified (originally):** the capture script against the real vendor filter and the real
206 PPD (`cupsRasterInterpretPPD` may reject something in it); building `konica-native` under
musl (it is plain C11 with no dependencies, but no musl toolchain was available here).
**Now resolved:** the musl build *is* verified — see `docs/ALPINE-PORT-STATUS.md`
(Alpine + musl, identical **60/60** corpus result to glibc).

## Step 1 — build the corpus (on the Ubuntu host where the printer works)

```sh
sudo apt install build-essential libcups2-dev
QUICK=1 tools/capture-corpus.sh /path/to/KonicaMinolta-206-fullbleed.ppd ~/konica-corpus
python3 tools/prnscan.py manifest ~/konica-corpus/manifest.tsv
```

It runs the vendor filter with stdout going to a **file**, never to the USB backend, so no
paper is used and the printer is not touched. Drop `QUICK=1` for the full matrix (every
resolution and page size in the PPD). Each case is run twice to find bytes that vary between
runs (timestamps, counters). A/B at 600 dpi is about 35 MB per raw page, so `.ras` files are
RLE-compressed by default (`RAS_UNCOMPRESSED=1` to change it).

**Fidelity check (worth five minutes).** `BASE_OPTS` mirrors the options seen in your logs
(`Duplexer=true MediaType=Plain back-side-orientation=Rotated`). Set `LogLevel debug` in
`cupsd.conf`, print one real job, and compare its `argv[5]` with the options in
`manifest.tsv`; set `BASE_OPTS` to match if they differ.

## Step 2 — read the captures

```sh
python3 tools/prnscan.py scan   corpus/001-simplex-blank.prn    # PJL lines, binary segments, zlib, length fields
python3 tools/prnscan.py common corpus/00[1-8]-simplex-*.prn    # fixed prefix/suffix = job framing
python3 tools/prnscan.py diff   corpus/001-simplex-blank.prn corpus/002-simplex-solid.prn
python3 tools/prnscan.py diff   corpus/045-probe-dot_0_0.prn corpus/046-probe-dot_1_0.prn
```

The cases are designed so each comparison answers one question: blank vs solid (page
payload and its size), one-pixel probes at x = 0, 1, 7, 8 (bit/byte packing), hlines/vlines/
checker (compression behaviour), `rowid`/`colid` (row and column addressing), duplex vs
simplex (the PJL differences already seen in README.md), toner save on/off, and each
resolution and page size.

## Step 3 — implement, rung by rung

Work in `src/encode.c` (`enc_job_begin`, `enc_page`, `enc_job_end`) and check against the
corpus after every change:

```sh
make && tests/difftest.sh ~/konica-corpus
```

| Rung | Target | Done when |
|---|---|---|
| L0 | capture corpus | this kit has produced it |
| L1 | monochrome A4 simplex | `difftest` passes the `simplex-*`, `probe-*`, `tex-*` cases byte for byte |
| L2 | duplex | `duplex-*` cases pass, incl. `*cupsBackSide: Rotated` orientation |
| L3 | colour | needs halftone/ICC work; the 206 is monochrome so this may not apply |
| L4 | paper path, recovery, toner save | open-ended |

Cases marked `deterministic=no` contain vendor-side varying bytes (timestamps, counters).
`capture-corpus.sh` reruns each case `RERUNS` times (default 4) and writes the union of the
differing byte ranges to `corpus/<id>.mask`. `difftest.sh` ignores masked ranges and reports
`PASS(masked)`, which proves everything else matches. The masked bytes still need an
explanation before the filter is complete, and a byte that coincides across every rerun is not
masked, so raise `RERUNS` if a masked case still fails on a tiny diff. Variable-length vendor
output cannot be masked byte-wise and stays FAIL.

### Resolved: toner save default

`OCM_TonerSave` absent from the options now resolves to **TRUE** (`j->toner_save = 1`).
Corpus-proven: case `005` (option absent) is byte-identical to `034-tonersave-true`, while
`035-tonersave-false` differs. (Toner save only changes the tone baseline; it does not affect
the spatial halftone stage, so this default is safe for byte-exactness.)

## What would speed this up

Uploading the vendor `245igdirf` and `mtorf.ocm` would let the symbol table (halftone,
colour-management and PJL-related names) guide where to look in the captures. They are not
needed for L1, and the captures come first either way.

## Licensing

Reimplementing a protocol for hardware you own is usually treated as interoperability work,
but the rules depend on jurisdiction and the vendor's licence. Read `COPYING` in the Konica
deb for any clause on reverse engineering before you start, and do not redistribute the
vendor's halftone, colour-world or ICC files. I am not a lawyer; this is a pointer, not advice.

## Layout

```
tools/mkraster.c          synthetic CUPS raster generator (needs libcups headers)
tools/capture-corpus.sh   L0 capture, run on the working host
tools/prnscan.py          scan / diff / common / manifest
src/raster.[ch]           libc-only CUPS raster reader (v1, v2 RLE, v3); sync word only on page 1
src/encode.[ch]           PJL envelope + page assembly + paper table (implemented)
src/jbig.[ch]             self-contained JBIG1 encoder (validated 60/60 BIEs)
src/halftone.[ch]         .hft screen loader + per-pixel dither (flat tones exact)
src/main.c                CUPS filter entry point, plus --dump
tests/check-reader.sh     reader vs libcups
tests/difftest.sh         candidate vs vendor, byte for byte
```
