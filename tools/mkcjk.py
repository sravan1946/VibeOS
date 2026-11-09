#!/usr/bin/env python3
"""Bakes the browser's fallback glyphs (CJK, Hangul, Hebrew, Thai) into root/usr/share/vibeos/uni15.bin.gz.

usage: tools/mkcjk.py [OUT]     (needs Pillow and the Noto fonts below; the output is committed, like common/fontsw.c)

The browser's baked faces (common/fontsw.c, tools/mkfont.py) cover Latin/Greek/Cyrillic only; user/bin/browser/uni.c loads this
file the first time a page uses anything else and hands the glyphs to gfx.c (font_fallback). One 15 px size, 4-bit coverage,
drawn at 4x and box-filtered like mkfont's faces (exact coverage). Noto Sans CJK / Hebrew / Thai are SIL OFL 1.1.

File (gzip): "VUNI", u32 n, then n x u32 codepoint (sorted), n x u32 offset into the data section, then the data.
Glyph at offset: u8 w, u8 h, i8 xoff, i8 ybase (top edge relative to the baseline; the browser adds the face's ascent), u8 adv,
then ceil(w*h/2) bytes of 4-bit rows (high nibble first, rows packed without padding).
"""
import gzip, struct, sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "root/usr/share/vibeos/uni15.bin.gz"
PX, SS = 15, 4
CJK = "/home/sravan/.local/share/fonts/NotoSansCJK/NotoSansCJK-Regular.ttc"
HEB = "/usr/share/fonts/noto/NotoSansHebrew-Regular.ttf"
THAI = "/usr/share/fonts/noto/NotoSansThai-Regular.ttf"


def dbcs(enc, lead, trail):
    s = set()
    for a in lead:
        for b in trail:
            try: s.add(bytes([a, b]).decode(enc))
            except UnicodeDecodeError: pass
    return s


b2 = list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
hanzi = (dbcs("gb2312", range(0xB0, 0xF8), range(0xA1, 0xFF))              # simplified: GB2312 (6763)
         | dbcs("big5", range(0xA4, 0xC7), b2)                               # traditional: Big5 level 1 (the 5401 common ones)
         | dbcs("shift_jis", list(range(0x88, 0xA0)) + list(range(0xE0, 0xEB)), range(0x40, 0xFD)))   # JIS X 0208 kanji
hangul = dbcs("euc-kr", range(0xB0, 0xC9), range(0xA1, 0xFF))               # KS X 1001 syllables (2350)
cjk = {ord(c) for c in hanzi | hangul}
for a, b in [(0x3000, 0x303F), (0x3041, 0x30FF), (0x3105, 0x312F), (0x3131, 0x318E), (0x31F0, 0x31FF),
             (0x3200, 0x321E), (0x3220, 0x3243), (0xFF01, 0xFF9F), (0xFFE0, 0xFFE6)]:
    cjk |= set(range(a, b + 1))
heb = set(range(0x5D0, 0x5EB)) | {0x5F0, 0x5F1, 0x5F2, 0x5F3, 0x5F4}
thai = set(range(0xE01, 0xE3B)) | set(range(0xE3F, 0xE5C))

faces = [(ImageFont.truetype(CJK, PX * SS, index=2), cjk), (ImageFont.truetype(HEB, PX * SS), heb),
         (ImageFont.truetype(THAI, PX * SS), thai)]
glyphs = {}
for f, cps in faces:
    for cp in sorted(cps):
        ch = chr(cp)
        try:
            adv = f.getlength(ch) / SS
            X0, Y0, X1, Y1 = f.getbbox(ch, anchor="ls")
        except Exception:
            continue
        x0, y0, x1, y1 = X0 // SS, Y0 // SS, -(-X1 // SS), -(-Y1 // SS)
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if cp > 0x3000 and not w and cp not in (0x3000,):
            continue                                                   # not in the font
        pix = b""
        if w and h:
            im = Image.new("L", (w * SS, h * SS), 0)
            ImageDraw.Draw(im).text((-x0 * SS, -y0 * SS), ch, font=f, fill=255, anchor="ls")
            pix = im.reduce(SS).tobytes()
        nib = [(p + 8) // 17 for p in pix]
        if len(nib) & 1: nib.append(0)
        data = bytes(nib[i] << 4 | nib[i + 1] for i in range(0, len(nib), 2))
        assert w < 256 and h < 256 and -128 <= x0 < 128 and -128 <= y0 < 128
        glyphs[cp] = struct.pack("<BBbbB", w, h, x0, y0, round(adv)) + data

cps = sorted(glyphs)
offs, body = [], b""
for cp in cps:
    offs.append(len(body)); body += glyphs[cp]
blob = b"VUNI" + struct.pack("<I", len(cps)) + struct.pack(f"<{len(cps)}I", *cps) + struct.pack(f"<{len(cps)}I", *offs) + body
import os
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as fo:
    fo.write(gzip.compress(blob, 9, mtime=0))
print(len(cps), "glyphs,", len(blob), "bytes raw,", os.path.getsize(OUT), "gz")
