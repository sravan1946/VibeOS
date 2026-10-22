#!/usr/bin/env python3
"""Regenerates the procedural sample images in root/home/Pictures (Pillow, deterministic, no assets)."""
import colorsys, math, os, random
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "root", "home", "Pictures")
os.makedirs(OUT, exist_ok=True)

def mandelbrot(w=640, h=400, it=60):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            c = complex(-2.3 + 3.4 * x / w, -1.2 + 2.4 * y / h)
            z, n = 0j, 0
            while n < it and abs(z) <= 2: z = z * z + c; n += 1
            if n == it: px[x, y] = (12, 10, 26)
            else:
                r, g, b = colorsys.hsv_to_rgb((0.72 + n / 40) % 1, 0.75, 0.35 + 0.65 * min(1, n / 18))
                px[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    return im

def dunes(w=800, h=480):                       # layered sine ridges under a gradient sky
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=(int(40 + 215 * t), int(20 + 110 * t ** 1.3), int(90 + 60 * (1 - t))))
    d.ellipse([w * .62, h * .14, w * .62 + 90, h * .14 + 90], fill=(255, 236, 190))
    rnd = random.Random(3)
    for i in range(5):
        base = h * (.45 + .1 * i); amp = 22 + 8 * i; ph = [rnd.random() * 6 for _ in range(3)]
        pts = [(x, base + amp * math.sin(x / (90 - 8 * i) + ph[0]) + amp / 2 * math.sin(x / 31 + ph[1])) for x in range(0, w + 4, 4)]
        k = i / 4
        d.polygon(pts + [(w, h), (0, h)], fill=(int(70 - 55 * k), int(30 - 20 * k), int(110 - 70 * k)))
    return im

def shapes(s=480):                             # RGBA with soft-edged transparency: shows the checkerboard
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    rnd = random.Random(11)
    for i in range(14):
        layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        r = rnd.randint(40, 120); cx, cy = rnd.randint(r, s - r), rnd.randint(r, s - r)
        rr, gg, bb = colorsys.hsv_to_rgb(rnd.random(), .6, 1)
        ImageDraw.Draw(layer).ellipse([cx - r, cy - r, cx + r, cy + r], fill=(int(rr * 255), int(gg * 255), int(bb * 255), rnd.randint(120, 220)))
        im = Image.alpha_composite(im, layer.filter(ImageFilter.GaussianBlur(2)))
    return im

mandelbrot().save(os.path.join(OUT, "mandelbrot.png"), optimize=True)
dunes().save(os.path.join(OUT, "dunes.png"), optimize=True)
shapes().save(os.path.join(OUT, "bubbles.png"), optimize=True)
for f in sorted(os.listdir(OUT)): print(f, os.path.getsize(os.path.join(OUT, f)))
