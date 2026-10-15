# The CMM table generator — recovering the 33³ colour-world LUT

The vendor filter ships **no** colour-world table. Its CMM builds one at run
time from the installed ICC profiles and samples it into a 33×33×33 byte grid;
the 3×1 neighbourhood stage (`PCM_lfFilter8Eval3x1`) then tetrahedral-interpolates
that grid. This document records the generator in full — pipeline, constants,
single-precision semantics and evidence — and how `src/lutgen.c` reproduces it
**byte for byte**, so the kit needs no captured dump.

```
                    ┌──────────────── the vendor's conversion, per grid point ────────────────┐
  x = 8·i  ──▶ rTRC(sRGB) ──▶ 3×3 matrix ──▶ XYZ ──▶ Lab (f) ──▶ L (0..255) ──▶ kTRC⁻¹(sGrayInv) ──▶ byte
  i = 0..32    1024-entry      Y row          Y/Yn     cuberoot     ×2.55        256-entry            round
```

Result: `sha256 = 84a424f5cccdbe7070b3f926fe9834b515db69391440ccac1dc5b046622c6d6f`
— identical to a dump taken from the live vendor filter, on glibc **and** musl.

## 1. Where the table comes from

The vendor's filter (`245igdirf`) links the ACMM colour engine. At start-up it
creates a colour world and asks the engine to sample it into a lookup table:

```
PCM_fXFilter8Set            (0x4431cd)   builds the 3×1 filter element
  PCM_fColorConversionInit  (0x43e75e)   ← append A2B, add TRC, add matrix,
                                            append B2A, add PCS→Gray, add inverse TRC
  PCM_fLUTCreate            (0x445c41)   N=3 inputs, M=1 output, 33 levels, 0x10000 range
  PCM_fLUTEntries + PCM_fVArrayCopy      ← the 33³ grid, stored inline in the element
```

An `strace` of the running filter shows only **one** profile is opened:

```
/usr/lib/cups/filter/KonicaMinolta/245igdi/Profiles/sGrayInv.icm   →  …/Profiles/sGrayInv.icm
/usr/lib/…/245igdirf.ocm, /usr/lib/…/Halftones/COPA.DEFAULT.600x600.hft
```

So the *output* space is the installed `sGrayInv.icm`, while the *input* space
(matrix + shaper TRCs) is built into the binary. That built-in space is a D50 sRGB:
its 1024-entry curve and its colourant matrix match the installed
`Profiles/sRGB.icm` to within a single ULP of `float` (§5), so `src/lutgen.c`
reads that profile at run time instead of baking anything in.

## 2. The conversion, stage by stage

Inputs are the raw 8-bit source samples. Each grid axis is sampled at
`x = 8·i`, `i = 0..32`, i.e. the sample value itself (so the grid covers 0…256).

| # | stage | formula | source |
|---|-------|---------|--------|
| 1 | input TRC ×3 | `t_k = curve(x_k)` | `sRGB.icm` `rTRC`/`gTRC`/`bTRC` (1024 `curv` entries) |
| 2 | matrix → XYZ (Y only is used) | `Y = Σ M[1][k]·t_k` | `sRGB.icm` `rXYZ`/`gXYZ`/`bXYZ` (Y components) |
| 3 | XYZ → L\* | `fy = f(Y / 127.5)`, `L = (1.16·fy − 0.16)·255` | vendor constants |
| 4 | L\* → grey | `g = kTRC⁻¹_sGrayInv(L)` | `sGrayInv.icm` `kTRC` (256 `curv` entries) |
| 5 | round | `byte = round(g)`, clamped to 0…255 | `PCM_fVArrayCopy` |

with

```
f(w) = w^(1/3)                    for w >  0.008856452070176601
f(w) = (841·w)/108 + 16/116       otherwise
```

Only the **Y** row of the matrix is used: stage 4 takes component 0 of the PCS
value, and `PCM_lfPCS2Gray` with signature `'Lab '` returns L\* and nothing else,
so X and Z never influence the table.

## 3. Vendor constants (transcribed from `.rodata`)

| addr | value | role |
|------|-------|------|
| `0x4685a8` | `0.999999` (double) | TRC input clamp (normalised ≥ 1) |
| `0x4685b0` | `255.0f` | VArrayCopy VRAM ceiling |
| `0x4685b4` | `0.5f` | VArrayCopy+TRC round term |
| `0x4689b8` | `122.9355f` | PCS white X = 0.9642·127.5 |
| `0x4689bc` | `127.5f` | PCS white Y (D50 scaled) |
| `0x4689c0` | `105.1747f` | PCS white Z = 0.8249·127.5 |
| `0x4689e0` | `0.13793103f` | 16/116 |
| `0x4689ec` | `0.008856452f` | (6/29)³ threshold |
| `0x4689f0` | `0.3333333333333333` (double) | cube-root exponent (libm `pow`) |
| `0x4689f8` | `841.0f` | linear-toe numerator |
| `0x4689fc` | `108.0f` | linear-toe denominator |
| `0x468a00` | `1.16f` | L coefficient (=116/100) |
| `0x468a04` | `0.16f` | L offset (=16/100) |
| `0x4689c8` | `255.0f` | L scale |

The white point is D50 `(0.9642, 1.0, 0.8249)` scaled by 127.5, i.e. the ICC PCS
white in the 0…255 convention the engine uses internally. The 127.5 scale and the
matrix's implicit ½ factor cancel into `Y/Yn = M_norm·(t/255)`.

## 4. Single-precision semantics

The engine's stages run in **IEEE single precision** (SSE), and the grid is only
32 kB, so several details matter at the 1-ULP level and are reproduced literally:

* **TRC scaling.** A `curv` entry `v` becomes `(float)v * (255.0f/65535.0f)`. The
  reciprocal is precomputed and *multiplied*; computing `v*255.0/65535.0` instead
  is a ULP out on 5 of 1024 entries. With the reciprocal form the generator's
  tables are identical to the vendor's, entry for entry.
* **TRC lookup** (`PCM_lfTRCEval`). `w = (float)x / 255.0f`, clamp to
  `[0, n−1]`, then `p = (double)w · (n−1)`; index `i = trunc(p)`;
  `frac = p − i`; result
  `(float)( (double)tab[i] + frac · (double)(float)(tab[i+1] − tab[i]) )`.
  The difference is formed in single precision, the product/sum in double.
* **Matrix** (`PCM_lfMatrixEval`). Accumulate left to right in `float`:
  `((0 + M[1][0]·t0) + M[1][1]·t1) + M[1][2]·t2`.
* **Matrix normalisation.** The colourant Y values are divided by their own sum
  and by 2 (`Y` is carried on the 0…127.5 scale): `M[1][k] = 0.5f·((float)Yk / Σ Y)`.
* **Lab** (`PCM_fXYZ2Lab`). Ratios and the L* combination are `float`; the cube
  root is `(float)pow((double)w, 1.0/3.0)`, i.e. the platform's libm `pow`, exactly
  as the vendor calls it (the vendor links `libm.so.6`).
* **Inverse TRC** (`PCM_lfTRCInversEval`). `kTRC` is monotonically **decreasing**;
  the bracket is located, then
  `g = 1.0f/(tab[i]−tab[i+1])` and the result is
  `( (a−v)·(i+1) + (v−b)·i )·g` in `float`, where `a = tab[i]`, `b = tab[i+1]`.
* **Rounding** (`PCM_fVArrayCopy`). `v ≤ 0 → 0`; `v ≥ 254.5 → 255`; else
  `trunc(v + 0.5)`.

Two build-level consequences are therefore required and are set in the `Makefile`:

* `-ffp-contract=off` — GCC/Clang must not fuse a multiply into an FMA, which
  would change the single-precision results;
* `-lm` — for `pow`, the same libm function the vendor calls.

`float` must be IEEE-754 binary32 (true on x86-64 and arm64, i.e. glibc and musl);
no x87 excess precision is assumed.

## 5. Evidence

The generator was not guessed. It was recovered by instrumenting the live filter
and reading its stages directly, then confirmed against dumps:

1. `PCM_fLUTCreate` / `PCM_fXFilter8Set` located by hardware watchpoint on the
   element's grid (`ELEM+0x1404`); the element layout is
   `[0x4c0 header][corner offsets 2³·4][3·257·4][3·256][3·8][grid 33³]`.
2. The stage call order logged as
   `SERIAL → PARALLEL(TRC,TRC,TRC) → Matrix → Functor(PCM_fXYZ2Lab) → PCM_lfPCS2Gray('Lab ') → PCM_lfTRCInversEval`.
3. Every operand dumped and checked against the installed profiles:
   * the three input TRCs are identical, 1024 `float` entries — `sRGB.icm` `rTRC`
     under `v·(255/65535)` (1019/1024 exact for the double form, **1024/1024** for
     the reciprocal form);
   * the matrix equals the `sRGB.icm` colourants, normalised
     (`M[1] = 0.111247599, 0.358447522, 0.030304879`, i.e. the D50 sRGB luminance
     coefficients);
   * the inverse TRC equals `sGrayInv.icm` `kTRC` under the same scaling
     (255/256 exact for the double form, **256/256** for the reciprocal form);
   * `PCM_lfPCS2Gray` returns L\* for signature `'Lab '`, and the engine's L\* is
     `116·fy − 16` scaled by 255/100.
4. The whole conversion re-evaluated in Python and in C reproduces the live grid
   exactly: `0` differing bytes out of `35937`, and the captured *float* table to
   `< 5·10⁻⁵`.

The one thing deliberately re-derived rather than copied is the input profile: the
vendor embeds a D50 sRGB, while the kit reads the installed `sRGB.icm`. The two
agree to a ULP, and the resulting table is bit-identical, so the kit embeds no
vendor data at all — it reads both profiles from disk.

## 6. Verification

```sh
make check-lutgen                 # regenerated table vs the vendor's (sha256 / dumped file)
tests/check-lutgen.sh dump.bin    #        … vs an explicit reference
KONICA_HFT=… tests/difftest.sh /path/to/konica-corpus   # 60/60, no KONICA_LUT
make check-reader                 # 27/27
```

| check | glibc | Alpine 3.24.2 / musl |
|-------|:-----:|:--------------------:|
| `check-lutgen` (sha256) | PASS | PASS |
| `difftest` (LUT generated, no dump) | 60 / 60 | 60 / 60 |
| `check-reader` | 27 / 27 | 27 / 27 |
| strict build (`-Wconversion -Wshadow`) | clean | — |

## 7. Failure modes

`acmm_generate()` returns NULL — and the caller falls back to the historical
flat-tone approximation, printing a one-line reason — if either profile is
missing, unreadable, oversized (> 8 MiB), not an ICC file (`acsp`), lacks `rTRC`
or the colourant `XYZ ` tags, or carries a `kTRC` that is not monotonically
decreasing. The profile paths are overridable:

| variable | default |
|----------|---------|
| `KONICA_SRGB` | `/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/sRGB.icm` |
| `KONICA_GRAY` | `/usr/local/lib/konica/KonicaMinolta/245igdi/Profiles/sGrayInv.icm` |
| `KONICA_LUT`  | *unset* — when set, a captured 35937-byte dump is used instead (`tools/dump-lut.gdb`) |

## 8. Reproducing the reference dump

To re-capture the vendor's own table for comparison:

```sh
F=/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
gdb -q -batch -x tools/dump-lut.gdb --args "$F" 1 u t 1 "$OPTS" < page.ras
# writes /tmp/lut33.bin
tests/check-lutgen.sh /tmp/lut33.bin
```
