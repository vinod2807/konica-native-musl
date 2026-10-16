#!/usr/bin/env python3
"""Write a small, self-contained PDF used by tests/real-docs.sh.

Why generate rather than ship a PDF: the real documents that exposed the geometry bug are
private (a CV, a letter), and rasterising them needs poppler/cups-filters anyway.  This
generates a deterministic two-page A4 document with the properties the test needs:

  page 1  small text at the top, then white space -> a page image that ends in long runs of
          blank stripes, which is where the vendor's BIE padding behaviour shows up;
  page 2  filled rectangles plus a stepped gradient -> dense, non-uniform halftone content.

Both pages are ordinary PDF drawing operators on standard 14 fonts, so pdftopdf/pdftoraster
render them without any embedded resources.
"""
import sys


def page1_content():
    out = ["0 0 0 rg", "BT /F1 11 Tf 72 770 Td 14 TL"]
    for i in range(28):
        out.append(f"(Line {i + 1:02d}: the quick brown fox jumps over the lazy dog, 0123456789) Tj T*")
    out += ["ET", "1 0 0 RG 2 w 72 60 m 523 60 l S"]
    return "\n".join(out)


def page2_content():
    out = ["0 0 0 rg"]
    for i in range(16):
        out.append(f"{i * 16} 0 0 rg {72 + i * 28} 500 24 200 re f")
    out += ["0.6 g", "72 200 452 30 re f", "0 0 0 rg"]
    for i in range(90):
        g = i / 89.0
        out.append(f"{g:.4f} g {72 + i * 5} 120 5 40 re f")
    out += ["BT /F1 18 Tf 72 80 Td (gradient + blocks) Tj ET"]
    return "\n".join(out)


def build(pages_content):
    objs = {}
    kids = []
    for i, content in enumerate(pages_content):
        page_no = 3 + i * 2
        cont_no = page_no + 1
        kids.append(f"{page_no} 0 R")
        objs[page_no] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
            f"/Resources << /Font << /F1 {3 + 2 * len(pages_content)} 0 R >> >> "
            f"/Contents {cont_no} 0 R >>"
        )
        objs[cont_no] = ("<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content))
    objs[1] = "<< /Type /Catalog /Pages 2 0 R >>"
    objs[2] = f"<< /Type /Pages /Kids [{' '.join(kids)}] /Count {len(pages_content)} >>"
    objs[3 + 2 * len(pages_content)] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for num in sorted(objs):
        offsets[num] = len(out)
        out += f"{num} 0 obj\n{objs[num]}\nendobj\n".encode("latin1")
    xref = len(out)
    size = max(objs) + 1
    out += f"xref\n0 {size}\n".encode()
    out += b"0000000000 65535 f \n"
    for num in range(1, size):
        out += (f"{offsets[num]:010d} 00000 n \n" if num in offsets else "0000000000 65535 f \n").encode()
    out += f"trailer\n<< /Size {size} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mkpdf.py out.pdf")
    open(sys.argv[1], "wb").write(build([page1_content(), page2_content()]))
