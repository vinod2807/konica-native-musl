#!/usr/bin/env python3
"""prnscan — inspect and compare printer data streams captured from a raster filter.

    prnscan.py scan   FILE            structure: UEL, PJL lines, binary segments, zlib probes
    prnscan.py diff   A B [-n N]      differing byte ranges, with context
    prnscan.py ranges A B             differing ranges as "start,end" lines (a mask for varying bytes)
    prnscan.py cmpmask OUT REF MASK   equal outside the mask? exit 0 = yes (sizes must match)
    prnscan.py common FILE [FILE...]  common prefix/suffix across captures (the fixed framing)
    prnscan.py manifest MANIFEST.tsv  summary of a capture-corpus manifest (determinism, sizes)

Standard library only. It makes no assumption about the payload encoding: it reports
what is observable (text commands, binary regions, entropy, length-like fields,
decompressible zlib streams) and leaves the decoding to the analyst.
"""
import argparse
import math
import re
import struct
import sys
import zlib

UEL = b"\x1b%-12345X"
PJL_RE = re.compile(rb"@PJL[^\r\n]*(?:\r\n|\n|\r)?")


def entropy(b: bytes) -> float:
    if not b:
        return 0.0
    counts = [0] * 256
    for x in b:
        counts[x] += 1
    n = len(b)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def hexs(b: bytes, n: int = 24) -> str:
    h = b[:n].hex(" ")
    return h + (" ..." if len(b) > n else "")


def find_regions(data: bytes):
    """Split into ('uel'|'pjl'|'bin', start, end) regions, in order."""
    marks = []
    for m in PJL_RE.finditer(data):
        marks.append(("pjl", m.start(), m.end()))
    pos = 0
    while True:
        i = data.find(UEL, pos)
        if i < 0:
            break
        marks.append(("uel", i, i + len(UEL)))
        pos = i + len(UEL)
    marks.sort(key=lambda t: t[1])
    out, cur = [], 0
    for kind, s, e in marks:
        if s < cur:           # overlapping match; skip
            continue
        if s > cur:
            out.append(("bin", cur, s))
        out.append((kind, s, e))
        cur = e
    if cur < len(data):
        out.append(("bin", cur, len(data)))
    return out


def zlib_probe(seg: bytes, limit: int = 12):
    """Find offsets inside seg where a valid zlib stream starts and report its size."""
    hits = []
    mv = memoryview(seg)
    i = 0
    n = len(seg) - 2
    while i < n and len(hits) < limit:
        cmf, flg = seg[i], seg[i + 1]
        # valid zlib header: deflate (CM=8), window <= 32K, (CMF*256+FLG) % 31 == 0, no preset dict
        if (cmf & 0x0F) == 8 and (cmf >> 4) <= 7 and ((cmf << 8) | flg) % 31 == 0 and not flg & 0x20:
            d = zlib.decompressobj()
            try:
                out = d.decompress(mv[i:])
            except zlib.error:
                i += 1
                continue
            if d.eof and len(out) > 0:
                used = len(seg) - i - len(d.unused_data)
                hits.append((i, used, len(out)))
                i += max(used, 1)
                continue
        i += 1
    return hits


def length_candidates(seg: bytes, total_after: int):
    """Look for a count in the first 16 bytes that matches the segment's own length."""
    res = []
    for off in range(0, min(16, len(seg))):
        for fmt, w in (("<H", 2), ("<I", 4), (">H", 2), (">I", 4)):
            if off + w > len(seg):
                continue
            v = struct.unpack_from(fmt, seg, off)[0]
            if v in (len(seg) - off - w, len(seg) - off, len(seg), total_after) and v > 16:
                res.append(f"{fmt} at +{off} = {v}")
    return res


def cmd_scan(a):
    data = open(a.file, "rb").read()
    print(f"{a.file}: {len(data)} bytes, entropy {entropy(data):.3f} bits/byte")
    regions = find_regions(data)
    uels = [s for k, s, _ in regions if k == "uel"]
    print(f"UEL (ESC%-12345X) at: {uels if uels else 'none'}")
    print()
    print(f"{'offset':>9} {'len':>9}  kind  detail")
    for kind, s, e in regions:
        seg = data[s:e]
        if kind == "uel":
            print(f"{s:9d} {e - s:9d}  UEL")
        elif kind == "pjl":
            print(f"{s:9d} {e - s:9d}  PJL   {seg.rstrip().decode('latin1')[:100]}")
        else:
            h = hexs(seg)
            print(f"{s:9d} {e - s:9d}  BIN   H={entropy(seg):.2f}  0x00:{seg.count(0) / len(seg):.0%} "
                  f"0xFF:{seg.count(255) / len(seg):.0%}  head: {h}")
            for z in zlib_probe(seg):
                print(f"{'':>20}zlib stream at +{z[0]}: {z[1]} B compressed -> {z[2]} B")
            lc = length_candidates(seg, len(data) - e)
            if lc:
                print(f"{'':>20}length-like fields: {'; '.join(lc[:4])}")
    pjl = [data[s:e].strip() for k, s, e in regions if k == "pjl"]
    if pjl:
        print("\nPJL commands in order:")
        for p in pjl:
            print("  ", p.decode("latin1"))


def diff_ranges(x: bytes, y: bytes):
    n = min(len(x), len(y))
    ranges, i = [], 0
    while i < n:
        if x[i] != y[i]:
            j = i
            while j < n and x[j] != y[j]:
                j += 1
            ranges.append((i, j))
            i = j
        else:
            i += 1
    return ranges


def cmd_diff(a):
    x, y = open(a.a, "rb").read(), open(a.b, "rb").read()
    print(f"A: {len(x)} B   B: {len(y)} B")
    r = diff_ranges(x, y)
    if len(x) != len(y):
        pre = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))
        print(f"sizes differ by {abs(len(x) - len(y))} B; common prefix {pre} B")
    if not r and len(x) == len(y):
        print("IDENTICAL")
        return 0
    print(f"{len(r)} differing range(s) in the overlapping {min(len(x), len(y))} B; showing {min(len(r), a.n)}")
    for s, e in r[: a.n]:
        c = 8
        print(f"  [{s}..{e}) {e - s} B")
        print(f"     A: {x[max(0, s - c):s].hex(' ')} | {x[s:e][:24].hex(' ')} | {x[e:e + c].hex(' ')}")
        print(f"     B: {y[max(0, s - c):s].hex(' ')} | {y[s:e][:24].hex(' ')} | {y[e:e + c].hex(' ')}")
    return 1


def cmd_ranges(a):
    x, y = open(a.a, "rb").read(), open(a.b, "rb").read()
    if len(x) != len(y):
        print("# size-differs")          # variable-length output cannot be masked byte-wise
    for s, e in diff_ranges(x, y):
        print(f"{s},{e}")


def cmd_cmpmask(a):
    out, ref = open(a.out, "rb").read(), open(a.ref, "rb").read()
    lines = [l.strip() for l in open(a.mask) if l.strip()]
    if any(l.startswith("#") for l in lines):
        print("mask marks the vendor output as variable-length; cannot compare byte-wise")
        return 2
    masked = bytearray(len(ref))
    spans = []
    for l in lines:
        s, e = (int(v) for v in l.split(","))
        masked[s:e] = b"\x01" * (e - s)
        spans.append((s, e))
    if len(out) != len(ref):
        print(f"size differs: ours {len(out)} B, vendor {len(ref)} B")
        return 1
    bad = [i for i in range(len(ref)) if out[i] != ref[i] and not masked[i]]
    if bad:
        print(f"{len(bad)} byte(s) differ outside the mask; first at offset {bad[0]}")
        return 1
    print(f"masked {len(spans)} range(s), {sum(e - s for s, e in spans)} B")
    return 0


def parse_prn(d: bytes):
    """Split a device stream into (pjl_text, pages).

    Two streams can then be compared structurally instead of byte-for-byte: the PJL
    envelope must match exactly, and each page's BIE is compared on its own.
    """
    i = 0
    pjl = bytearray()
    pages, cur = [], None
    while i < len(d):
        if d[i:i + 9] == b"\x1b%-12345X":
            pjl += d[i:i + 9]; i += 9; continue
        m = re.match(rb"@PJL SET IMAGELEN=(\d+)\r\n", d[i:])
        if m:
            n = int(m.group(1)); s = i + m.end()
            if cur is None:
                cur = {"bie": bytearray(), "last_end": s}
            cur["bie"] += d[s:s + n]
            cur["last_end"] = s + n
            pjl += d[i:s]
            i = s + n
            continue
        m = re.match(rb"@PJL[^\r\n]*\r\n", d[i:]) or re.match(rb"\r\n", d[i:])
        if not m:
            raise ValueError(f"unparsed byte {d[i]:#02x} at offset {i}")
        line = d[i:i + m.end()]
        if b"PAGESTATUS=START" in line:
            if cur:
                pages.append(cur)
            cur = {"bie": bytearray(), "last_end": 0}
        pjl += line
        i += m.end()
    if cur:
        pages.append(cur)
    return bytes(pjl), pages


def pad_start(bie: bytes):
    """Offset of the first byte after the last SDNORM marker, or None.

    Everything from here to the end of the BIE is 4-byte alignment padding.  The device
    stops decoding at the final SDNORM, so those bytes are not part of the image.
    """
    k = bie.rfind(b"\xff\x02")
    if k < 0:
        return None
    return k + 2


def cmd_padcmp(a):
    """Compare our stream with the vendor's, tolerating the never-decoded pad bytes.

    Exits 0 when the streams are byte-identical *or* differ only in the 0-3 alignment
    bytes after the last SDNORM of one or more pages, 1 otherwise.
    """
    out, ref = open(a.out, "rb").read(), open(a.ref, "rb").read()
    if len(out) != len(ref):
        print(f"FAIL: size differs: ours {len(out)} B, vendor {len(ref)} B")
        return 1
    try:
        pjl_o, pages_o = parse_prn(out)
        pjl_r, pages_r = parse_prn(ref)
    except ValueError as e:
        print(f"FAIL: cannot parse the stream: {e}")
        return 1
    if pjl_o != pjl_r:
        print("FAIL: the PJL envelope differs")
        return 1
    if len(pages_o) != len(pages_r):
        print(f"FAIL: page count differs: ours {len(pages_o)}, vendor {len(pages_r)}")
        return 1

    padded = []          # pages that differ only in the pad
    for n, (po, pr) in enumerate(zip(pages_o, pages_r), 1):
        bo, br = bytes(po["bie"]), bytes(pr["bie"])
        if bo == br:
            continue
        if len(bo) != len(br):
            print(f"FAIL: page {n}: BIE length differs: ours {len(bo)} B, vendor {len(br)} B")
            return 1
        so, sr = pad_start(bo), pad_start(br)
        if so is None or sr is None:
            print(f"FAIL: page {n}: no SDNORM marker found")
            return 1
        first = min(i for i in range(len(bo)) if bo[i] != br[i])
        if first < so or first < sr:
            print(f"FAIL: page {n}: differs at BIE offset {first}, before the pad "
                  f"(our pad starts at {so}, vendor's at {sr})")
            bad = [i for i in range(len(bo)) if bo[i] != br[i] and i < so and i < sr]
            print(f"      {len(bad)} byte(s) outside the pad, first at {bad[0]}")
            return 1
        padded.append((n, len(bo) - max(so, sr), len(bo), first))

    if not padded:
        print("PASS: byte-identical")
        return 0
    tot = sum(x[1] for x in padded)
    print(f"PASS: identical apart from {tot} alignment byte(s) after the last SDNORM "
          f"({len(padded)} page(s))")
    for n, p, ln, first in padded:
        print(f"      page {n}: BIE {ln} B, pad {p} B, first differing offset {first}")
    return 0


def cmd_bih(a):
    """Print each page's Bi-level Image Header: the frame size the filter *encoded*.

    Comparing this with a reference tells you whether a difference is in the geometry the
    filter chose or in the pixels it put there."""
    d = open(a.stream, "rb").read()
    _, pages = parse_prn(d)
    print(f"{'page':>4} {'BIE':>9}  {'Xd':>6} {'Yd':>6} {'L0':>5} {'MX':>4} {'MY':>4} ord opts")
    for n, p in enumerate(pages, 1):
        b = bytes(p["bie"])
        if len(b) < 20:
            print(f"{n:>4} {len(b):>9}  (too short for a BIH)")
            continue
        if b[:4] != b"\x00\x00\x01\x00":
            print(f"{n:>4} {len(b):>9}  (BIH magic {b[:4].hex(' ')})")
            continue
        xd, yd, l0 = struct.unpack_from(">3I", b, 4)
        print(f"{n:>4} {len(b):>9}  {xd:>6} {yd:>6} {l0:>5} {b[16]:>4} {b[17]:>4} "
              f"{b[18]:>3}  {b[19]:#04x}")
    return 0


def cmd_bie(a):
    """Cut each page's BIE out of a device stream: bie <stream> <out-prefix>."""
    d = open(a.stream, "rb").read()
    try:
        _, pages = parse_prn(d)
    except ValueError as e:
        print(f"cannot parse {a.stream}: {e}", file=sys.stderr)
        return 1
    for n, p in enumerate(pages, 1):
        path = f"{a.prefix}.{n}.bie"
        open(path, "wb").write(bytes(p["bie"]))
        print(f"page {n}: {len(p['bie'])} B -> {path}")
    return 0


def cmd_common(a):
    blobs = [open(f, "rb").read() for f in a.files]
    m = min(len(b) for b in blobs)
    pre = 0
    while pre < m and all(b[pre] == blobs[0][pre] for b in blobs):
        pre += 1
    suf = 0
    while suf < m - pre and all(b[-1 - suf] == blobs[0][-1 - suf] for b in blobs):
        suf += 1
    print(f"{len(blobs)} files; sizes {min(len(b) for b in blobs)}..{max(len(b) for b in blobs)} B")
    print(f"common prefix: {pre} B")
    if pre:
        print("  " + blobs[0][:pre][:256].decode("latin1").encode("unicode_escape").decode()[:600])
    print(f"common suffix: {suf} B")
    if suf:
        print("  " + blobs[0][-suf:][-256:].decode("latin1").encode("unicode_escape").decode()[-600:])


def cmd_manifest(a):
    rows = [l.rstrip("\n").split("\t") for l in open(a.manifest)]
    hdr, rows = rows[0], rows[1:]
    ix = {k: i for i, k in enumerate(hdr)}
    print(f"{len(rows)} cases")
    nondet = [r[ix["id"]] for r in rows if r[ix["deterministic"]] != "yes"]
    zero = [r[ix["id"]] for r in rows if r[ix["prn_bytes"]] == "0" or r[ix["exit"]] != "0"]
    print(f"non-deterministic: {len(nondet)} {nondet[:10]}")
    print(f"empty or failed:   {len(zero)} {zero[:10]}")
    print(f"{'id':36} {'ras B':>10} {'prn B':>9}")
    for r in rows:
        print(f"{r[ix['id']]:36} {r[ix['ras_bytes']]:>10} {r[ix['prn_bytes']]:>9}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("scan"); s.add_argument("file"); s.set_defaults(fn=cmd_scan)
    d = sp.add_parser("diff"); d.add_argument("a"); d.add_argument("b"); d.add_argument("-n", type=int, default=20); d.set_defaults(fn=cmd_diff)
    rg = sp.add_parser("ranges"); rg.add_argument("a"); rg.add_argument("b"); rg.set_defaults(fn=cmd_ranges)
    cm = sp.add_parser("cmpmask"); cm.add_argument("out"); cm.add_argument("ref"); cm.add_argument("mask"); cm.set_defaults(fn=cmd_cmpmask)
    pc = sp.add_parser("padcmp", help="compare tolerating the never-decoded BIE pad bytes")
    pc.add_argument("out"); pc.add_argument("ref"); pc.set_defaults(fn=cmd_padcmp)
    pb = sp.add_parser("bie", help="write each page's BIE to <prefix>.<n>.bie")
    pb.add_argument("stream"); pb.add_argument("prefix"); pb.set_defaults(fn=cmd_bie)
    ph = sp.add_parser("bih", help="print each page's BIH (encoded frame size)")
    ph.add_argument("stream"); ph.set_defaults(fn=cmd_bih)
    c = sp.add_parser("common"); c.add_argument("files", nargs="+"); c.set_defaults(fn=cmd_common)
    m = sp.add_parser("manifest"); m.add_argument("manifest"); m.set_defaults(fn=cmd_manifest)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
