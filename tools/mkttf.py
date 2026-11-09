#!/usr/bin/env python3
"""mkttf.py - static, subsetted instances of the variable fonts in third_party/fonts for the browser's vector text (user/bin/browser/ttf.c;
stb_truetype can't do variable axes): root/usr/share/vibeos/fonts/inter-W.ttf (opsz 28, wght W) and jbmono-W.ttf. Needs fonttools:
  python3 -m venv v && v/bin/pip install fonttools && v/bin/python tools/mkttf.py"""
import os
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools import subset
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
out = os.path.join(root, "root/usr/share/vibeos/fonts"); os.makedirs(out, exist_ok=True)
UNI = list(range(0x20, 0x7F)) + list(range(0xA0, 0x250)) + list(range(0x370, 0x400)) + list(range(0x400, 0x460)) + list(range(0x2010, 0x2070)) + \
      list(range(0x20A0, 0x20C0)) + list(range(0x2190, 0x2200)) + list(range(0x2500, 0x2580)) + list(range(0x25A0, 0x2800)) + [0x2122, 0x2116, 0x2713, 0x2717, 0x2736, 0xFFFD]
def make(src, name, axes):
    f = TTFont(os.path.join(root, "third_party/fonts", src))
    f = instantiateVariableFont(f, axes, inplace=False)
    opt = subset.Options(); opt.layout_features = ["kern"]; opt.hinting = False; opt.name_IDs = [1, 2]; opt.notdef_outline = True
    s = subset.Subsetter(opt); s.populate(unicodes=UNI); s.subset(f)
    f.save(os.path.join(out, name)); print(name, os.path.getsize(os.path.join(out, name)))
for w in (400, 500, 600, 700, 800): make("Inter-VF.ttf", "inter-%d.ttf" % w, {"wght": w, "opsz": 28})
for w in (400, 700): make("JetBrainsMono-VF.ttf", "jbmono-%d.ttf" % w, {"wght": w})
# symbol fallback (stars, dingbats, arrows the text faces lack): Noto Sans Symbols 2 (SIL OFL, same licence text as NotoSans-OFL.txt), subset
f = TTFont("/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf")
opt = subset.Options(); opt.layout_features = []; opt.hinting = False; opt.name_IDs = [1, 2]; opt.notdef_outline = False
s = subset.Subsetter(opt); s.populate(unicodes=list(range(0x2190, 0x2200)) + list(range(0x2300, 0x2400)) + list(range(0x25A0, 0x2800)) + list(range(0x2B00, 0x2C00))); s.subset(f)
f.save(os.path.join(out, "symbols.ttf")); print("symbols.ttf", os.path.getsize(os.path.join(out, "symbols.ttf")))
