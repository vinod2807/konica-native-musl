# The Konica 206 device stream — reverse-engineered format

Everything the vendor filter `245igdirf` emits between the CUPS raster it reads and the
bytes the USB backend receives. Derived from the 60-page capture corpus
(`../../konica-corpus-full`) and from the disassembly of the installed filter and its
OCM plugin (`mtorf.ocm`). No byte here is guessed; each item is cross-checked against
captures, except where explicitly marked **OPEN** at the end.

Implemented in `src/encode.c` (framing + page assembly), `src/jbig.c` (JBIG encoder),
`src/halftone.c` (screen).

## 1. PJL envelope

The job is a sequence of PJL lines, `\r\n`-terminated, wrapped in a UEL.

```
\x1b%-12345X@PJL\r\n
@PJL SET COMPRESS=JBIG\r\n
@PJL SET COVER=OFF\r\n
@PJL SET HOLD=OFF\r\n
@PJL SET SECTION=OFF\r\n
@PJL SET COPIES=1\r\n
```

`COMPRESS..COPIES` occur **once per job**. Each page then repeats:

```
@PJL SET PAGESTATUS=START\r\n
@PJL SET DUPLEX=OFF\r\n                 (or ON + BINDING=SHORTEDGE|LONGEDGE)
@PJL SET PAPER=<code>\r\n
@PJL SET PAPERWIDTH=<w>\r\n
@PJL SET PAPERLENGTH=<h>\r\n
@PJL SET MEDIASOURCE=AUTO\r\n
@PJL SET MEDIATYPE=PLAIN\r\n
@PJL SET RESOLUTION=600\r\n
@PJL SET IMAGELEN=<n>\r\n<n raw bytes>       (repeated; one per chunk)
@PJL SET PAGESTATUS=END\r\n
```

The job ends with:

```
@PJL EOJ\r\n\x1b%-12345X\r\n
```

Mapping (from `manifest.tsv` + captures):

| CUPS option | PJL |
|---|---|
| `Duplex=DuplexNoTumble` (long edge) | `DUPLEX=ON` + `BINDING=SHORTEDGE` |
| `Duplex=DuplexTumble` (short edge) | `DUPLEX=ON` + `BINDING=LONGEDGE` |
| `Duplex=None` | `DUPLEX=OFF` (no BINDING line) |
| `PageSize` | `PAPER=<code>` (see table) |
| `Resolution` | `RESOLUTION=<dpi>` |
| `MediaType=Plain` | `MEDIATYPE=PLAIN`, `MEDIASOURCE=AUTO` |

`PAPERWIDTH`/`PAPERLENGTH` are the PPD page geometry at the print resolution
(`floor(points * dpi / 72)`), and the PJL paper *code* is a Konica-internal name
(`A4`, `A3`, `MLTM16K`, `JISB4`, `C63OF4`, `COM10`, `FLS3`, …). The exact per-size
values used by the 600 dpi path are tabulated in `src/encode.c` (`papers[]`).

`OCM_TonerSave` never appears in the envelope; it only changes page pixels. The
corpus confirms that **absent behaves as TRUE** (identical stream to `OCM_TonerSave=TRUE`).

## 2. Page image = one JBIG1 BIE, chunked

The page payload is a single **JBIG1 (ITU-T T.82)** bi-level image, split into
`IMAGELEN` chunks of at most **32768** bytes (the last chunk is the remainder). Each
chunk is prefixed by its own `IMAGELEN=<n>` line.

The image is the raster **cropped by one column and one row** (e.g. A4 raster
4958×7017 → image 4957×7016). Blank A4 = 132 bytes; full solid = 132452 bytes.

### Bi-level Image Header (20 bytes, vendor layout)

```
00 00 01 00                 DL=0 D=0 planes=1, reserved
Xd  (4 bytes, big endian)   = image width
Yd  (4 bytes, big endian)   = image height
l0  (4 bytes, big endian)   = 0x00000080  (128 lines per stripe)
mx=00 my=00                 no adaptive-template movement
order = 03                  ILEAVE | SMID (irrelevant: single plane/layer)
options = 0x48              LRLTWO (0x40) | TPBON (0x08)
```

Note the vendor stores **Xd/Yd as the width/height directly**; jbigkit stores `width`
too, but the important quirk is that some paper sizes produce an image one pixel
smaller than the raster in x, y, or both. The per-size (Xd,Yd) values are in
`papers[]`.

### Entropy coding

Each 128-line stripe is entropy-coded with the JBIG arithmetic (QM) coder using the
two-line **LRLTWO** template and **TPBON** typical prediction, then terminated with
the stripe marker `FF 02` (SDNORM). The coder state (context probability table)
persists across stripes. Two behaviours are essential for byte-exactness:

* the coder must **not** strip trailing zero bytes from a stripe before `FF 02`
  (jbigkit's `jbg_buf_remove_zeros` must be disabled), and
* the finished **BIE is zero-padded to a multiple of 4 bytes**.

`src/jbig.c` reimplements this from the T.82 tables. It reproduces all 60 corpus BIEs
byte-for-byte when fed the vendor's own 1-bit image (`tests/difftest.sh`).

## 3. Halftone (8-bit raster → 1-bit image)

The filter maps each 8-bit gray sample to one output bit through the `COPA` screen.

* The screen file is `Halftones/COPA.DEFAULT.600x600.hft`. It is a small header plus
  five planes; **plane 0** is the 96×96 threshold array the 600×600 path uses
  (values 1..255). (`1200x600.hft` has a 192×96 plane 0.)
* Per-pixel rule recovered from the disassembly of `HalftoneImage1c1b`:

  ```
  dot(x,y) = ( LUT(gray(x,y)) >= screen[y % H][x % W] )
  ```

  with the source mapped through a tone table. Measured from uniform captures, the
  table is `LUT(g) = 245 - g` (so a black input gives the 82.86 % dot coverage seen
  for a solid page, and white gives none). Implemented in `src/halftone.c`.

Flat tones, solid and blank pages are reproduced **byte-for-byte** this way (e.g.
`002-simplex-solid` and `001-simplex-blank` pass `difftest.sh`).

### The neighbourhood stage (solved)

For non-uniform input the tone is *not* `245 - gray`: before halftoning, the vendor runs the
page through an ACMM colour-world filter element, `PCM_lfFilter8Eval3x1` — a **3-tap
tetrahedral interpolation of a 33³ byte LUT** over the contiguous source image (stride
`S = round_up(width,4)`, taps wrapping into the next row), with a pointwise `-10` under
`OCM_TonerSave` (on by default):

```
tone[x] = max(0, Φ(src[x], src[x+1], src[x+2]) - (OCM_TonerSave ? 10 : 0))
```

`Φ` is implemented in `src/acmm.c`; the 33³ table is a vendor runtime table loaded from a
dump via `KONICA_LUT` (see `docs/REVERSE-ENGINEERING.md` §9). With it the whole pipeline is
byte-exact.

## 4. Differential-test status

Against `konica-corpus-full`: **60/60 byte-exact** (glibc and Alpine/musl), with the runtime
LUT loaded via `KONICA_LUT`. The framing, the ACMM stage, the screen and the JBIG coder are
each verified in isolation as well as end-to-end. Without `KONICA_LUT` the kit falls back to
the flat-tone approximation and reproduces the historical 7/60 subset.
