#!/usr/bin/env python3
"""Write the SVG bundle that tools/svgrast.c rasterises into build/gen/{icons,appicons,fileicons}.c.

  mkicons.py OUT.bundle
The icon lists come from common/icons.h. Bundle records: "KIND ID SIZE LEN\\n" + LEN bytes of SVG, KIND S/A/F.
Symbolic icons get their stroke scaled so 16 px draws 1.5 px lines and 20 px 1.75 px (the mockups' ico()).
"""
import os, re, sys
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import appicons as AI  # noqa: E402

LUC = os.path.join(ROOT, "third_party/lucide/icons")
hdr = open(os.path.join(ROOT, "common/icons.h")).read()


def lst(name):
    body = hdr.split(f"#define {name}(X)")[1].split("\n\n")[0]
    return re.findall(r'X\((\w+), "([^"]+)"', body)


def lucide(name):
    s = open(os.path.join(LUC, name + ".svg")).read()
    return re.search(r"<svg[^>]*>(.*)</svg>", s, re.S).group(1).strip()


def symbolic(name, size):
    sw = (1.5 if size <= 16 else 1.75) * 24 / size
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 24 24">'
            f'<g fill="none" stroke="#FFFFFF" stroke-width="{sw:.4f}" stroke-linecap="round" stroke-linejoin="round">'
            f'{lucide(name)}</g></svg>')


I = AI.build(AI.Style("tone", on_dark=True))
# direction A's folder is slate; emblems are a Lucide glyph on its front (build.py folder_icon)
C, CD = "#7C8A97", AI.SLATE
fold = I["folder"].replace(AI.SAND_D, CD).replace(AI.SAND, C).replace(AI.mix(AI.SAND, "#FFFFFF", .55), AI.mix(C, "#FFFFFF", .55))
fold = fold.replace(AI.mix(AI.SAND_D, "#000000", .30), AI.mix(CD, "#000000", .30))
I["files"] = I["folder"] = fold
for kind, em in (("home", "house"), ("desktop", "monitor"), ("documents", "file-text"), ("downloads", "download"),
                 ("music", "music"), ("pictures", "image"), ("projects", "code"), ("trash", "trash-2")):
    I["folder-" + kind] = fold + (f'<g transform="translate(18 24) scale(.5)" fill="none" stroke="#C9D2DA" stroke-width="3" '
                                  f'stroke-linecap="round" stroke-linejoin="round">{lucide(em)}</g>')
I["text"] = I["doc"]


def colour(key, size):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 48 48">'
            f'{I[key]}</svg>')


out = bytearray()
def rec(kind, id_, size, svg):
    b = svg.encode()
    out.extend(f"{kind} {id_} {size} {len(b)}\n".encode() + b)

for id_, f in lst("ICON_LIST"):
    for s in (16, 20): rec("S", id_, s, symbolic(f, s))
for kind, name in (("A", "APPICON_LIST"), ("F", "FILEICON_LIST")):
    for id_, key in lst(name):
        for s in (24, 32, 48): rec(kind, id_, s, colour(key, s))
open(sys.argv[1], "wb").write(out)
