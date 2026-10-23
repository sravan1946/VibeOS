#!/usr/bin/env python3
"""Bake the vendored fonts (third_party/fonts: Inter 4.001 VF, JetBrains Mono 2.211 VF, Noto Sans VF; all OFL) into 8-bit
alpha glyph tables: common/fonts.c + font.h (UI ramp, default mono) and common/termfonts.c + .h (terminal zoom).

Run from the repo root when changing fonts (needs Pillow built with raqm, for Inter's tabular digits); the output
is committed, so the build needs no TTFs.  Glyph y offsets are relative to the top of the line box, a fixed line
height per ramp face with the baseline where CSS puts it (half-leading around the hhea ascender + descender).
"""
import os
from PIL import Image, ImageDraw, ImageFont, features

D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party/fonts/")
INTER, JB, NOTO = D + "Inter-VF.ttf", D + "JetBrainsMono-VF.ttf", D + "NotoSans-VF.ttf"
assert features.check("raqm"), "Pillow needs raqm for tnum"
# non-ASCII extras, kept small: every GUI binary links these tables statically
COMMON = ("".join(chr(c) for c in range(0xA1, 0x100)) + "–—‘’“”•…‹›←↑→↓↔↵⇧−≈≠≤≥∞√"
          "✓✗★☆♥●○◆◇■□▲▼▶◀►◄⌫⏎░▒▓█▀▄▌▐")
SMALL = "×÷−±·°•…→←✓✗–—‘’“”"
LATIN = "".join(chr(c) for c in range(0xA1, 0x100)) + SMALL
# (c name, ttf, px, wght, line height, extra advance px, extras); docs/DESIGN.md 3.1 is the binding list.
# lh None = the pre-redesign Noto faces, baked as before (hinted, tight ASCII box) until their callers move to T_*.
FONTS = [
    ("font_caption",      INTER, 11, 400, 14, .5, COMMON),
    ("font_caption_bold", INTER, 11, 600, 14, .3, COMMON),
    ("font_body",         INTER, 13, 400, 18, 0, COMMON),
    ("font_body_bold",    INTER, 13, 600, 18, 0, COMMON),
    ("font_heading",      INTER, 15, 600, 20, 0, COMMON),
    ("font_title",        INTER, 20, 600, 26, 0, LATIN),
    ("font_display",      INTER, 28, 600, 34, 0, SMALL),
    ("font_mono",         JB, 13, 400, 18, 0, COMMON),
    ("font_mono_bold",    JB, 13, 700, 18, 0, COMMON),
    ("font_ui",           NOTO, 15, 400, None, 0, COMMON),
    ("font_ui_bold",      NOTO, 15, 700, None, 0, COMMON),
    ("font_big",          NOTO, 52, 500, None, 0, "×÷−±·°"),
]
# extra terminal sizes for Ctrl+= / Ctrl+- (termfonts.c is linked into the kernel only; 13 px lives in fonts.c)
ZOOM = [(f"{n}_{px}", JB, px, w, round(px * 18 / 13), 0, COMMON)
        for px in (11, 15, 18) for n, w in (("font_mono", 400), ("font_mono_bold", 700))]


SS = 4   # glyphs are drawn at SS x and box-filtered: exact coverage, no hinting distortion


def load(path, px, wght, ss=1):
    f = ImageFont.truetype(path, px * ss, layout_engine=ImageFont.Layout.RAQM)
    axes = [a["name"] for a in f.get_variation_axes()]
    f.set_variation_by_axes([min(32, max(14, px)) if a == b"Optical size" else 100 if a == b"Width" else wght
                             for a in axes])                   # opsz = px clamped, as CSS auto
    return f


FEAT = ["tnum"]      # tabular digits (Inter has tnum; JetBrains Mono's digits are already fixed-width)


def render_sig(f, ch):
    m = f.getmask(ch, features=FEAT)
    return f.getlength(ch, features=FEAT), m.size, bytes(m)


def bake(name, path, px, wght, lh, track, extras):
    f = load(path, px, wght)
    asc, desc = f.getmetrics()
    glyphs, alpha = [], bytearray()
    others = [p for p in (INTER, JB) if p != path]            # the other faces fill the gaps (box drawing, ✓)
    fonts = [f] + [load(p, px, wght) for p in others]
    ss = SS if lh else 1                                       # legacy faces stay hinted 1x, as they were
    big = [load(p, px, wght, ss) for p in [path] + others]
    nodef = [render_sig(ff, "\U0010fffd") for ff in fonts]     # what .notdef looks like in each
    src = {c: 0 for c in range(32, 127)}                       # codepoint -> font that has it
    for c in sorted({ord(ch) for ch in extras}):
        for i, ff in enumerate(fonts):
            if render_sig(ff, chr(c)) != nodef[i]: src[c] = i; break
    xcps = sorted(c for c in src if c >= 127)
    base = round((lh - asc - desc) / 2 + asc) if lh else asc
    for code in list(range(32, 127)) + xcps:
        ch, ff = chr(code), big[src[code]]
        fadv = ff.getlength(ch, features=FEAT) / ss + track
        X0, Y0, X1, Y1 = ff.getbbox(ch, anchor="ls", features=FEAT)   # baseline-relative, so fallbacks line up
        x0, y0, x1, y1 = X0 // ss, Y0 // ss, -(-X1 // ss), -(-Y1 // ss)
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        off = len(alpha)
        if w and h:
            im = Image.new("L", (w * ss, h * ss), 0)
            ImageDraw.Draw(im).text((-x0 * ss, -y0 * ss), ch, font=ff, fill=255, anchor="ls", features=FEAT)
            im = im.reduce(ss)
            alpha += im.tobytes()
        glyphs.append([w, h, x0, y0 + base, round(fadv), round(fadv * 64), off])
    if lh is None:   # tighten the line box to the real ASCII extents (font metrics can be huge)
        ymin = min(g[3] for g in glyphs[:95] if g[1])
        ymax = max(g[3] + g[1] for g in glyphs[:95] if g[1])
        pad = round(px * 0.2)
        for g in glyphs:
            g[3] = g[3] - ymin + pad // 2
        base, lh = asc - ymin + pad // 2, ymax - ymin + pad
    out = [f"static const uint8_t {name}_alpha[{max(1, len(alpha))}] = {{"]
    for i in range(0, len(alpha), 24):
        out.append("    " + ",".join(str(b) for b in alpha[i:i + 24]) + ",")
    out.append("};")
    n = len(xcps)
    out.append(f"static const uint32_t {name}_xcp[{max(1, n)}] = {{ " + ",".join(map(str, xcps)) + " };")
    out.append(f"static const struct glyph {name}_xg[{max(1, n)}] = {{")
    for g in glyphs[95:]:
        out.append("    {%d,%d,%d,%d,%d,%d,%d}," % tuple(g))
    out.append("};")
    out.append(f"const struct font {name} = {{ {lh}, {base}, {name}_alpha, {{")
    for g in glyphs[:95]:
        out.append("    {%d,%d,%d,%d,%d,%d,%d}," % tuple(g))
    out.append(f"}}, {n}, {name}_xcp, {name}_xg }};\n")
    return "\n".join(out)


src = ['/* generated by tools/mkfont.py from OFL fonts (Inter, JetBrains Mono, Noto Sans; third_party/fonts) - do not edit */',
       '#include "font.h"', ""]
hdr = ["/* generated by tools/mkfont.py */", "#pragma once", "#include <stdint.h>", "",
       "/* adv: whole-pixel advance; adv64: the exact one in 1/64 px (for a fractional pen: x = (pen64 + 32) >> 6) */",
       "struct glyph { uint8_t w, h; int8_t xoff, yoff; uint8_t adv; uint16_t adv64; uint32_t off; };",
       "/* g[] covers ASCII 32..126; xcp/xg are sorted extra codepoints (see font_glyph) */",
       "struct font { int height, ascent; const uint8_t *alpha; struct glyph g[95];",
       "              int nx; const uint32_t *xcp; const struct glyph *xg; };", "",
       "/* type ramp (docs/DESIGN.md 3.1; design.h's T_* roles): Inter with tabular digits; height = line box,",
       " * ascent = baseline in it",
       " *   font_caption       11/14 400 (+0.5 px tracking)   font_caption_bold  11/14 600 (+0.3 px)",
       " *   font_body          13/18 400                      font_body_bold     13/18 600",
       " *   font_heading       15/20 600                      font_title         20/26 600",
       " *   font_display       28/34 600",
       " *   font_mono          JetBrains Mono 13/18 400       font_mono_bold     13/18 700 (zoom sizes: termfonts.h)",
       " * Not ramp, kept until callers move to T_*: font_ui / font_ui_bold (Noto Sans 15), font_big (Noto 52 digits) */",
       "#define FONT_RAMP 1"]
for name, *a in FONTS:
    src.append(bake(name, *a))
    hdr.append(f"extern const struct font {name};")
zsrc = ['/* generated by tools/mkfont.py: terminal zoom sizes (kernel only) - do not edit */', '#include "font.h"', ""]
zhdr = ["/* generated by tools/mkfont.py */", "#pragma once", '#include "font.h"', ""]
for name, *a in ZOOM:
    zsrc.append(bake(name, *a))
    zhdr.append(f"extern const struct font {name};")
open("common/termfonts.c", "w").write("\n".join(zsrc))
open("common/termfonts.h", "w").write("\n".join(zhdr) + "\n")
open("common/fonts.c", "w").write("\n".join(src))
open("common/font.h", "w").write("\n".join(hdr) + "\n")
print("ok", os.path.getsize("common/fonts.c"), os.path.getsize("common/termfonts.c"))
