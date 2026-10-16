#!/usr/bin/env python3
"""mkras.py -- synthetic CUPS raster writer (pure Python, no libcups).

tools/mkraster fills the header with cupsRasterInterpretPPD(), so every raster it writes has
the shape a PPD-driven job produces: a real PageSizeName, RLE compression, and pixel
dimensions that follow the PPD.  Real jobs from pdftoraster do *not*: they are v3
(uncompressed), the page-size name can disagree with the page size in points, and the image
is the borderless canvas (a couple of pixels wider and a row or two shorter than the points
imply).  Those shapes are where the geometry and padding defects lived.

This writer sets the header field by field, matching the on-disk layout of
cups_page_header2_t (see the offsets in src/raster.c).

Usage:
    mkras.py --seed 42 -o out.ras [--meta meta.tsv]     # randomised case
    mkras.py --version 3 --size 4961x7016 --points 595x842 --name A4.Borderless -o out.ras

--meta writes "seed<TAB>options<TAB>description" for the fuzzer to consume.
"""
import argparse
import random
import struct
import sys

# Points (width x length) the vendor's paper table recognises.
PAPERS = {
    "16K": (553, 765), "8K": (757, 1100), "A3": (842, 1191), "A4": (595, 842),
    "A5": (420, 595), "A6": (297, 420), "B4": (729, 1032), "B5": (516, 729),
    "B6": (363, 516), "Comm10": (297, 684), "EnvC6": (323, 459), "EnvDL": (312, 624),
    "EnvPersonal": (261, 468), "Executive": (522, 756), "FLS3": (612, 935),
    "FLS4": (624, 935), "FLS5": (584, 955), "FLS2": (595, 935), "FLS6": (612, 972),
    "FLS1": (575, 935), "Legal": (612, 1008), "Letter": (612, 792),
    "Statement": (396, 612), "Tabloid": (792, 1224),
}

HDR_SIZE = {1: 420, 2: 1796, 3: 1796}
# CUPS writes the magic as the reverse of the familiar "RaS2"/"RaS3" spelling; verified against
# a corpus raster ("2SaR") and a real pdftoraster raster ("3SaR"), and against src/raster.c's
# 0x52615333/0x52615332/0x52615374 constants read back on a little-endian host.
SYNC = {1: b"tSaR", 2: b"2SaR", 3: b"3SaR"}


def ink_at(pat, x, y, w, h, seed):
    """Ink 0..255 at (x, y).  0 = bare paper, matching tools/mkraster."""
    kind = pat[0]
    a = pat[1:]
    if kind == "blank":
        return 0
    if kind == "solid":
        return 255
    if kind == "hgrad":
        return x * 255 // (w - 1) if w > 1 else 0
    if kind == "vgrad":
        return y * 255 // (h - 1) if h > 1 else 0
    if kind == "hlines":
        per, t = (a + (4, 1))[:2]
        return 255 if per and y % per < t else 0
    if kind == "vlines":
        per, t = (a + (4, 1))[:2]
        return 255 if per and x % per < t else 0
    if kind == "checker":
        s = a[0] if a else 1
        return 255 if ((x // s) + (y // s)) & 1 else 0
    if kind == "box":
        x0, y0, bw, bh = (a + (0, 0, 1, 1))[:4]
        return 255 if x0 <= x < x0 + bw and y0 <= y < y0 + bh else 0
    if kind == "frame":
        return 255 if x in (0, w - 1) or y in (0, h - 1) else 0
    if kind == "rowid":
        return (y * 251) & 0xFF
    if kind == "colid":
        return (x * 251) & 0xFF
    if kind == "noise":
        s = a[0] if a else 1
        hsh = (x * 0x9E3779B1 ^ (y + 0x7F4A7C15) * 0x85EBCA6B ^ s * 0xC2B2AE35 ^ seed * 0x27D4EB2F) & 0xFFFFFFFF
        hsh ^= hsh >> 15
        hsh = (hsh * 0x2C1B3C6D) & 0xFFFFFFFF
        hsh ^= hsh >> 12
        return hsh & 0xFF
    raise SystemExit(f"mkras: unknown pattern {kind}")


def make_rows(pat, w, h, bpp, cs, seed):
    """Yield one bytes object per row (bytes_per_line each)."""
    bpl = (w * bpp + 7) // 8
    for y in range(h):
        vals = [ink_at(pat, x, y, w, h, seed) for x in range(w)]
        if bpp == 8:
            row = bytes(vals)
        else:                                   # 1 bpp: ink -> bit set
            bits = bytearray(bpl)
            for x, v in enumerate(vals):
                if v > 127:
                    bits[x >> 3] |= 0x80 >> (x & 7)
            row = bytes(bits)
        yield row + b"\x00" * (bpl - len(row))


def rle_line(row, eol):
    """One run-encoded line.  Byte 128 means "fill the rest of the line with `eol`"."""
    enc = bytearray()
    i, n = 0, len(row)
    while i < n:
        j = i
        while j < n and row[j] == row[i]:
            j += 1
        if row[i] == eol and all(b == eol for b in row[i:]):
            enc.append(128)                      # clear to end of line
            break
        run = j - i
        while run:
            take = min(run, 128)
            enc.append(take - 1)
            enc.append(row[i])
            run -= take
        i = j
    return enc


def rle_page(rows, cs):
    """CUPS v2 RLE, the exact inverse of ras_read_line().

    The layout is [repeat][runs] per line, where `repeat` counts *extra* copies of the line
    it precedes (the reader replays r->repeat after decoding that line)."""
    eol = 0x00 if cs in (3, 4, 5, 6, 7, 8, 9, 10, 11) else 0xFF
    out = bytearray()
    i, n = 0, len(rows)
    while i < n:
        j = i
        while j + 1 < n and rows[j + 1] == rows[i]:
            j += 1
        copies, first = j - i, True
        while first or copies > 0:
            out.append(min(copies, 255))
            copies -= min(copies, 255)
            out.extend(rle_line(rows[i], eol))
            first = False
        i = j + 1
    return bytes(out)


def build(args, seed):
    rng = random.Random(seed)
    host = sys.byteorder
    end = "<" if host == "little" else ">"

    pages = []
    for p in range(args.pages):
        if args.random:
            # The page size in points is what the vendor keys on, so vary it independently
            # of the name -- including the disagreements real jobs produce.
            name = rng.choice(list(PAPERS))
            pw, ph = PAPERS[name]
            style = rng.choice(["exact", "exact", "exact", "borderless-name", "no-name",
                                "mismatch", "off-by-one", "swapped", "unknown"])
            points = (pw, ph)
            psn = name
            if style == "borderless-name":
                psn = name + ".Borderless"
            elif style == "no-name":
                psn = ""
            elif style == "mismatch":
                other = rng.choice([k for k in PAPERS if k != name])
                psn = other + (".Borderless" if rng.random() < 0.5 else "")
            elif style == "off-by-one":
                points = (pw + rng.choice([-1, 1]), ph)
            elif style == "swapped":
                points = (ph, pw)
            elif style == "unknown":
                points = (rng.randrange(200, 900), rng.randrange(200, 1200))
                psn = rng.choice(["", "Nonexistent.Size", name])
            res = rng.choice([600, 600, 600, 600, 300, 1200])
            scale = res / 72.0
            jitter_x = rng.choice([0, 0, 0, 1, 2, -1, -2, 7])
            jitter_y = rng.choice([0, 0, 0, -1, -2, 1, 2, -7])
            w = max(1, int(points[0] * scale) + jitter_x)
            h = max(1, int(points[1] * scale) + jitter_y)
            bpp = rng.choice([8, 8, 8, 8, 8, 1])
            cs = rng.choice([0, 0, 0, 3]) if bpp == 1 else 0
            pat = rng.choice([("solid",), ("blank",), ("noise", rng.randrange(1, 999)),
                              ("hlines", rng.randrange(2, 9), rng.randrange(1, 3)),
                              ("checker", rng.randrange(1, 6)), ("hgrad",), ("vgrad",),
                              ("rowid",), ("frame",),
                              ("box", rng.randrange(0, 50), rng.randrange(0, 50),
                               rng.randrange(1, 60), rng.randrange(1, 60))])
        else:
            points = args.points
            name = args.name
            psn = args.name
            res, w, h, bpp, cs = args.res, args.w, args.h, args.bpp, args.cs
            pat = (args.pattern,)
        pages.append(dict(points=points, psn=psn, res=res, w=w, h=h, bpp=bpp, cs=cs, pat=pat))

    version = args.version if args.version else (3 if not args.random else rng.choice([3, 3, 3, 3, 3, 3, 2, 2, 1]))
    duplex = args.duplex if args.duplex is not None else (
        rng.choice([0, 1]) if args.random else 0)
    media = args.media or ("Plain" if not args.random else rng.choice(["Plain", "Plain", "Auto", "Transparency"]))

    out = bytearray(SYNC[version])
    for pg in pages:
        hdr = bytearray(HDR_SIZE[version])
        hdr[128:128 + len(media) + 1] = media.encode() + b"\x00"
        struct.pack_into(end + "I", hdr, 272, duplex)
        struct.pack_into(end + "I", hdr, 276, pg["res"])
        struct.pack_into(end + "I", hdr, 280, pg["res"])
        struct.pack_into(end + "I", hdr, 292, pg["points"][0])   # ImagingBoundingBox
        struct.pack_into(end + "I", hdr, 296, pg["points"][1])
        struct.pack_into(end + "I", hdr, 340, 1)                 # NumCopies
        struct.pack_into(end + "I", hdr, 344, 0)                 # Orientation
        struct.pack_into(end + "I", hdr, 352, pg["points"][0])   # PageSize, points
        struct.pack_into(end + "I", hdr, 356, pg["points"][1])
        struct.pack_into(end + "I", hdr, 368, 0)                 # Tumble
        struct.pack_into(end + "I", hdr, 372, pg["w"])           # cupsWidth
        struct.pack_into(end + "I", hdr, 376, pg["h"])           # cupsHeight
        struct.pack_into(end + "I", hdr, 380, 0)                 # cupsMediaType
        struct.pack_into(end + "I", hdr, 384, pg["bpp"])         # cupsBitsPerColor
        struct.pack_into(end + "I", hdr, 388, pg["bpp"])         # cupsBitsPerPixel
        struct.pack_into(end + "I", hdr, 392, (pg["w"] * pg["bpp"] + 7) // 8)  # BytesPerLine
        struct.pack_into(end + "I", hdr, 400, pg["cs"])          # cupsColorSpace
        struct.pack_into(end + "I", hdr, 404, 1 if version == 2 else 0)        # Compression
        if version != 1:                                         # v1's header stops at 420
            struct.pack_into(end + "I", hdr, 420, 1)             # cupsNumColors
            struct.pack_into(end + "f", hdr, 424, 1.0)           # BorderlessScalingFactor
            struct.pack_into(end + "f", hdr, 428, float(pg["points"][0]))
            struct.pack_into(end + "f", hdr, 432, float(pg["points"][1]))
            struct.pack_into(end + "f", hdr, 436, 0.0)
            struct.pack_into(end + "f", hdr, 440, 0.0)
            struct.pack_into(end + "f", hdr, 444, float(pg["points"][0]))
            struct.pack_into(end + "f", hdr, 448, float(pg["points"][1]))
            hdr[1668:1673] = b"auto\x00"
            hdr[1732:1732 + len(pg["psn"]) + 1] = pg["psn"].encode() + b"\x00"
        out += hdr
        rows = list(make_rows(pg["pat"], pg["w"], pg["h"], pg["bpp"], pg["cs"], seed))
        out += rle_page(rows, pg["cs"]) if version == 2 else b"".join(rows)

    return bytes(out), pages, version, duplex, media


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--meta", help="write seed/options/description here")
    ap.add_argument("--random", action="store_true", help="randomise the whole case from --seed")
    ap.add_argument("--version", type=int, choices=(1, 2, 3))
    ap.add_argument("--pages", type=int, default=1)
    ap.add_argument("--points", default="595x842", help="page size in points, WxH")
    ap.add_argument("--name", default="A4")
    ap.add_argument("--res", type=int, default=600)
    ap.add_argument("--size", default="4961x7016",
                    help="image size in pixels, WxH (default 4961x7016 = A4 borderless at 600 dpi)")
    ap.add_argument("-b", "--bpp", type=int, default=8, choices=(1, 8))
    ap.add_argument("-c", "--cs", type=int, default=0, help="CUPS colour space")
    ap.add_argument("-P", "--pattern", default="checker")
    ap.add_argument("--duplex", type=int, choices=(0, 1), default=None)
    ap.add_argument("--media", default=None)
    a = ap.parse_args()
    a.points = tuple(int(v) for v in a.points.lower().split("x"))
    a.w, a.h = (int(v) for v in a.size.lower().split("x"))
    if a.random:
        a.version = None                 # let the case pick v1/v2/v3

    data, pages, version, duplex, media = build(a, a.seed)
    with open(a.out, "wb") as f:
        f.write(data)
    opts = "PageSize=%s media=%s Duplex=%s sides=%s Duplexer=true MediaType=Plain back-side-orientation=Rotated" % (
        pages[0]["psn"].split(".")[0] or "A4", media,
        "DuplexNoTumble" if duplex else "None",
        "two-sided-long-edge" if duplex else "one-sided")
    desc = "; ".join("v%d %dx%d px %d dpi %dbpp cs=%d %s points=%dx%d" %
                     (version, p["w"], p["h"], p["res"], p["bpp"], p["cs"],
                      "+".join(str(x) for x in p["pat"]), p["points"][0], p["points"][1])
                     for p in pages)
    if a.meta:
        with open(a.meta, "w") as f:
            f.write(f"{a.seed}\t{opts}\t{desc}\n")
    print(f"{a.seed}\t{opts}\t{desc}")


if __name__ == "__main__":
    main()
