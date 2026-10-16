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
| `PageSize=` | *(ignored)*: `PAPER`/`PAPERWIDTH`/`PAPERLENGTH` come from the header's points |
| `Resolution=` | *(ignored)*: `RESOLUTION=600` always |
| `MediaType=` | *(ignored)*: `MEDIATYPE=` comes from the header's `MediaType` string |
| *(fixed)* | `MEDIASOURCE=AUTO` |

`DUPLEX`/`BINDING` are **not** simply the `Duplex=` option: the vendor combines that option
with the raster header's own `Duplex` and `Tumble` fields and with `sides=`, and it names the
edge from the *page orientation* — a landscape page (points wider than tall) swaps
`SHORTEDGE` and `LONGEDGE`. Measured against the running filter over a 256-combination grid
of header fields × option spellings (see `docs/REVERSE-ENGINEERING.md`):

The pair is resolved **once per job, from the first page**, and the same `DUPLEX`/`BINDING`
lines are printed on every page: a two-page job whose pages differ in orientation, or in
their header `Duplex`/`Tumble` fields, still gets one pair, derived from page 1.

| `DUPLEX` is ON when | |
|---|---|
| `Duplex=` present and case-insensitively `DuplexNoTumble`/`DuplexTumble` | always ON |
| `Duplex=` present but anything else (`None`, `none`, `Foo`, …) | header `Duplex != 0` |
| `Duplex=` absent | header `Duplex != 0` **or** `sides=` is exactly `two-sided-long-edge`/`two-sided-short-edge` |

`BINDING` (emitted only with `DUPLEX=ON`) is `LONGEDGE` when `tumble ^ landscape`, else
`SHORTEDGE`, where `tumble` is:

| `tumble` | |
|---|---|
| `Duplex=` case-insensitively `DuplexNoTumble` | 0 |
| `Duplex=` case-insensitively `DuplexTumble` | 1 |
| `Duplex=` present but neither (e.g. `None`) | header `Tumble != 0` |
| `Duplex=` absent, `sides=two-sided-long-edge` | 0 |
| `Duplex=` absent, `sides=two-sided-short-edge` | 1 |
| `Duplex=` absent, otherwise | header `Tumble != 0` |

Two vendor quirks are reproduced faithfully: `Duplex=` is matched **case-insensitively** while
`sides=` is matched **case-sensitively**, and `sides=two-sided-long-edge` becomes `SHORTEDGE`
(the opposite of CUPS' naming).

`RESOLUTION` is **not** derived from the raster: rewriting a raster's `HWResolution` to 300
or 1200 dpi still yields a byte-identical stream with `RESOLUTION=600`, because the device is
600 dpi. The kit emits 600 unconditionally (and warns on stderr if the raster disagrees).

`PAPERWIDTH`/`PAPERLENGTH` and the image crop are **keyed on the raster header's `PageSize`
in points**, not on the `PageSize=` option, not on `PageSizeName` and not on the raster's
pixel dimensions (verified by rewriting those fields one at a time and re-running the vendor
filter). This is why a real `pdftoraster` A4 page — 4961×7016 pixels, self-described as
`A4.Borderless`, a name absent from the PPD — still yields the A4 entry: `4958/7016` with a
`4957×7016` image, exactly as the corpus A4 raster (4958×7017, name `A4`) does.

The point pair selects a table entry by matching **either orientation** (`842×595` picks the
same entry as `595×842`). A pair that matches nothing becomes `PAPER=CUSTOM` with the shorter
side as the width; the vendor accepts a custom page only while the shorter side is 72..864
points and the longer side at most 1224 points, and otherwise exits 2 having written nothing
(the kit refuses it with exit 1 and an empty stdout). Each point count becomes two pixel
counts — the published `PAPERWIDTH`/`PAPERLENGTH` and the encoded BIH `Xd`/`Yd` — and they are
not a plain `points*600/72`: with `base = floor(points*600/72)` the published value is
`base + (points % 9 == 8)` and the encoded value is `base - (points % 9 ∈ {1,3,6})`. That
reproduces all 24 `papers[]` entries (96 numbers) and a 69-point sweep of custom sizes
(`src/encode.c: dim_from_points()`). `tests/check-geometry.sh` pins the whole lookup.

`PAPER=<code>` is a Konica-internal name (`A4`, `A3`, `MLTM16K`, `JISB4`, `C63OF4`, `COM10`,
`FLS3`, …).

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
* the finished **BIE is padded to a multiple of 4 bytes**.

The padding bytes are the one place where the kit knowingly differs from the vendor. They
follow the last `FF 02` and the device never decodes them, and the vendor does not write a
defined value: it pads from the uninitialised tail of a freshly `malloc()`ed per-page buffer.
On a real 2-page document whose page streams are 77476 and 44036 bytes — both under glibc's
128 KiB `mmap` threshold — page 2's allocation reused the block freed after page 1, so its pad
bytes were page 1's bytes at the same offsets (`42 be`); in corpus cases 029/031/033 the page
streams are ≥ 128 KiB, which glibc serves with `mmap` (freshly zeroed), and the vendor's pad
bytes were `00 00` even though stale bytes were available. Emulating the allocator is not
portable, so `src/jbig.c` writes deterministic zeros. A byte-exact diff of a real document
should mask the trailing `len % 4 ? 4 - len % 4 : 0` bytes of any page whose stream is not
already 4-aligned.

`src/jbig.c` reimplements the coder from the T.82 tables. It reproduces all 60 corpus BIEs
byte-for-byte when fed the vendor's own 1-bit image (`tests/difftest.sh`).

## 3. Halftone (8-bit raster → 1-bit image)

The filter maps each 8-bit gray sample to one output bit through the `COPA` screen.

**1-bit rasters do not go through this pipeline at all.** The vendor copies a 1bpp raster's
bits straight into the page image — bit set is ink — cropped to `Xd`×`Yd`, leaving everything
the raster does not cover dot-free and ignoring the raster's colour space (`cs=0` and `cs=3`
behave identically). Measured on A6 (`Xd` 2475, `Yd` 3499): an all-ink 1bpp raster yields
exactly 2475×3499 ink pixels, an all-white one yields none, a checker yields half, and a
rectangle drawn in the raster lands on the same pixels. `src/encode.c` has a separate path for
this; the corpus is all 8bpp, so only the differential fuzzer (`tests/fuzz-difftest.sh`)
exercised it.

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

`Φ` is implemented in `src/acmm.c`; the 33³ table itself is not a file the vendor ships —
its CMM generates it at run time from the installed ICC profiles, and `src/lutgen.c`
reproduces that generator exactly (see `docs/LUT-GENERATOR.md`). The whole pipeline is
byte-exact.

## 4. Differential-test status

Against `konica-corpus-full`: **60/60 byte-exact** (glibc and Alpine/musl), with the 33³
table regenerated from the profiles at startup — no captured dump needed. The framing, the
ACMM stage, the screen and the JBIG coder are each verified in isolation as well as
end-to-end. `KONICA_LUT` can force a captured dump instead.

The corpus is all v2/RLE rasters built from the PPD, so it exercises neither real
`pdftoraster` headers (v3, `A4.Borderless`, 4961×7016) nor BIE padding: every one of its 60
page streams is already a multiple of 4 bytes. Real-document runs found both gaps — see
`docs/ALPINE-PORT-STATUS.md` §7.2 and `tests/check-geometry.sh`.

Three more gaps were found by the differential fuzzer (`tests/fuzz-difftest.sh`), which
generates rasters varying version, dpi, bit depth, page size vs. size name and content, and
diffs both filters: the orientation-dependent `DUPLEX`/`BINDING` rule, the 1bpp pass-through,
and the point→pixel conversion for sizes that are not in the table. `tests/real-docs.sh` runs
whole PDFs through the real `pdftopdf`+`pdftoraster` chain, `tests/check-mediatype.sh` and
`tests/check-errors.sh` need no vendor filter, and `tests/check-determinism.sh` re-runs a
corpus with `MALLOC_PERTURB_` set.

Runtime data (screen + ICC profiles) is loaded in `enc_open()` before the first byte reaches
stdout. If it cannot be read the filter exits non-zero with **nothing on stdout**; the old
flat-tone approximation is available only via `KONICA_FLAT_TONE=1`.
