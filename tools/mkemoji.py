#!/usr/bin/env python3
"""Bakes the browser's colour emoji into root/usr/share/vibeos/emoji16.bin.gz (Noto Color Emoji, SIL OFL 1.1, Pillow).

usage: tools/mkemoji.py [OUT]

File (gzip): "VEMO", u32 n, n x u32 codepoint (sorted), then n x 512 bytes: 16x16 pixels, row-major, u16 little endian ARGB4444
(not premultiplied). The browser (user/bin/browser/uni.c) draws them as 16 px colour glyphs through gfx.c's font_fallback.
Only single-codepoint emoji above U+1F000 plus the BMP ones the monochrome faces lack; ZWJ sequences, skin tones and flags show their
first/base emoji (the joiners and selectors are zero width).
"""
import gzip, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "root/usr/share/vibeos/emoji16.bin.gz"
f = ImageFont.truetype("/usr/share/fonts/noto/NotoColorEmoji.ttf", 109)
ranges = [(0x1F004, 0x1F004), (0x1F0CF, 0x1F0CF), (0x1F170, 0x1F251), (0x1F300, 0x1F64F), (0x1F680, 0x1F6FF), (0x1F7E0, 0x1F7EB),
          (0x1F90C, 0x1F9FF), (0x1FA70, 0x1FAFF)]
bmp = [0x231A, 0x231B, 0x23E9, 0x23EA, 0x23EB, 0x23EC, 0x23F0, 0x23F3, 0x25FD, 0x25FE, 0x2614, 0x2615, 0x267F, 0x2693, 0x26A1,
       0x26AA, 0x26AB, 0x26BD, 0x26BE, 0x26C4, 0x26C5, 0x26CE, 0x26D4, 0x26EA, 0x26F2, 0x26F3, 0x26F5, 0x26FA, 0x26FD, 0x2705,
       0x270A, 0x270B, 0x2728, 0x274C, 0x274E, 0x2753, 0x2754, 0x2755, 0x2757, 0x2795, 0x2796, 0x2797, 0x27B0, 0x27BF, 0x2B1B,
       0x2B1C, 0x2B50, 0x2B55] + list(range(0x2648, 0x2654))
cps = sorted({c for a, b in ranges for c in range(a, b + 1)} | set(bmp))


def render(cp):
    im = Image.new("RGBA", (136, 128), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((0, 0), chr(cp), font=f, embedded_color=True)
    return im


notdef = render(0x10FFFD).tobytes()
out, keep = [], []
for cp in cps:
    im = render(cp)
    if im.tobytes() == notdef or not im.getbbox():
        continue
    px = im.convert("RGBa").resize((16, 16), Image.BOX).convert("RGBA")   # premultiplied resize: no dark fringes
    row = b""
    for r, g, b, a in px.get_flattened_data() if hasattr(px, "get_flattened_data") else px.getdata():
        row += struct.pack("<H", (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4))
    keep.append(cp); out.append(row)
blob = b"VEMO" + struct.pack("<I", len(keep)) + struct.pack(f"<{len(keep)}I", *keep) + b"".join(out)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as fo:
    fo.write(gzip.compress(blob, 9, mtime=0))
print(len(keep), "emoji,", len(blob), "bytes raw,", os.path.getsize(OUT), "gz")
