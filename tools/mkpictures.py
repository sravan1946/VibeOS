#!/usr/bin/env python3
"""Regenerates the procedural sample images in root/home/Pictures (Pillow, deterministic, no assets):
PNGs, JPEGs (baseline, progressive, EXIF-rotated) and animated GIFs."""
import math, os, random
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "root", "home", "Pictures")
os.makedirs(OUT, exist_ok=True)
CALM = [(201, 164, 106), (184, 120, 96), (143, 169, 138), (122, 144, 168), (150, 132, 150), (168, 162, 150)]   # muted: sit with Graphite
def mixc(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def mandelbrot(w=640, h=400, it=60):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            c = complex(-2.3 + 3.4 * x / w, -1.2 + 2.4 * y / h)
            z, n = 0j, 0
            while n < it and abs(z) <= 2: z = z * z + c; n += 1
            if n == it: px[x, y] = (20, 21, 23)
            else: px[x, y] = mixc((40, 42, 46), CALM[0] if n % 2 else CALM[5], min(1, n / 18) ** 0.8)
    return im

def dunes(w=800, h=480):                       # layered sine ridges under a gradient sky
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=mixc((43, 45, 49), (150, 124, 94), t ** 1.3))
    d.ellipse([w * .62, h * .14, w * .62 + 90, h * .14 + 90], fill=(222, 204, 168))
    rnd = random.Random(3)
    for i in range(5):
        base = h * (.45 + .1 * i); amp = 22 + 8 * i; ph = [rnd.random() * 6 for _ in range(3)]
        pts = [(x, base + amp * math.sin(x / (90 - 8 * i) + ph[0]) + amp / 2 * math.sin(x / 31 + ph[1])) for x in range(0, w + 4, 4)]
        k = i / 4
        d.polygon(pts + [(w, h), (0, h)], fill=mixc((132, 106, 80), (34, 30, 28), k))
    return im

def shapes(s=480):                             # RGBA with soft-edged transparency: shows the checkerboard
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    rnd = random.Random(11)
    for i in range(14):
        layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        r = rnd.randint(40, 120); cx, cy = rnd.randint(r, s - r), rnd.randint(r, s - r)
        col = CALM[int(rnd.random() * len(CALM))]
        ImageDraw.Draw(layer).ellipse([cx - r, cy - r, cx + r, cy + r], fill=col + (rnd.randint(120, 220),))
        im = Image.alpha_composite(im, layer.filter(ImageFilter.GaussianBlur(2)))
    return im

def orbits(s=160, n=16):                       # balls circling on a transparent canvas: an animated GIF with disposal 2
    frames = []
    for i in range(n):
        im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        d.ellipse([s // 2 - 14, s // 2 - 14, s // 2 + 14, s // 2 + 14], fill=CALM[0] + (255,))
        for k in range(5):
            a = 2 * math.pi * (i / n + k / 5)
            r = 28 + 12 * k
            x, y = s / 2 + r * math.cos(a * (1 + k % 2)), s / 2 + r * math.sin(a * (1 + k % 2))
            d.ellipse([x - 9, y - 9, x + 9, y + 9], fill=CALM[1 + k] + (255,))
        frames.append(im)
    return frames

mandelbrot().save(os.path.join(OUT, "mandelbrot.png"), optimize=True)
dunes().save(os.path.join(OUT, "dunes.png"), optimize=True)
dunes().save(os.path.join(OUT, "..", "Desktop", "Dunes.png"), optimize=True)                                  # the desktop copy
shapes().save(os.path.join(OUT, "bubbles.png"), optimize=True)
dunes().save(os.path.join(OUT, "dunes.jpg"), quality=72, optimize=True)                                    # 4:2:0 baseline
mandelbrot(480, 300).save(os.path.join(OUT, "mandelbrot-progressive.jpg"), quality=60, progressive=True)     # progressive
ex = Image.Exif(); ex[0x0112] = 6                                                                           # stored sideways, EXIF says rotate
dunes(360, 240).rotate(90, expand=True).save(os.path.join(OUT, "dunes-rotated.jpg"), quality=75, subsampling=1, exif=ex.tobytes())
fr = orbits()
fr[0].save(os.path.join(OUT, "orbits.gif"), save_all=True, append_images=fr[1:], duration=60, loop=0, disposal=2)
for f in sorted(os.listdir(OUT)): print(f, os.path.getsize(os.path.join(OUT, f)))
