#!/usr/bin/env python3
"""Host test for user/lib/image.c. usage: test.py IMAGETEST_BINARY

Decoder: writes PNGs of every colour type / bit depth / interlace / filter / zlib mode with a small
pure-python writer (Pillow can't write interlaced or most 16-bit/low-depth files), plus files Pillow
itself writes (PNG, BMP, PPM), and compares our decode with Pillow's decode of the same file.
JPEG: Pillow-written baseline/progressive files (sizes, subsamplings, qualities, restart intervals, optimised
tables, greyscale, EXIF orientation) must match Pillow's libjpeg decode within JPEG_TOL per channel.
GIF: Pillow-written and hand-written (own LZW encoder: offsets, local palettes, interlace, disposal 1/2/3,
transparency) animations must match Pillow's composited frames exactly, delays included.
Encoder: round-trips images through image_png_encode and opens them with Pillow (which checks CRCs).
"""
import os, random, struct, subprocess, sys, tempfile, zlib
from PIL import Image

BIN = sys.argv[1]
rnd = random.Random(7)
tmp = tempfile.mkdtemp()
fails = 0
count = 0

def run(*args):
    return subprocess.run([BIN, *args], capture_output=True, text=True)

def ref_from(im, path):
    im = im.convert("RGBA")
    open(path, "wb").write(struct.pack("<II", *im.size) + im.tobytes())

def check_dec(name, file, im):
    global fails, count
    count += 1
    ref = os.path.join(tmp, "ref")
    ref_from(im, ref)
    r = run("dec", file, ref)
    if r.returncode:
        fails += 1
        print(f"FAIL decode {name}: {r.stdout.strip()}")

# ---------- pure-python PNG writer ----------
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

def pack_row(samples, depth):
    if depth == 16: return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8: return bytes(samples)
    out, acc, nb = bytearray(), 0, 0
    for s in samples:
        acc = acc << depth | s; nb += depth
        if nb == 8: out.append(acc); acc = nb = 0
    if nb: out.append(acc << (8 - nb))
    return bytes(out)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c

def filt(row, prev, bpp, f):
    o = bytearray()
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[f]
        o.append((x - pred) & 255)
    return bytes(o)

ADAM = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CH = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}

def write_png(path, w, h, ct, depth, px, interlace=0, plte=None, trns=None, zmode="default", filters=None):
    """px[y][x] = tuple of samples. Returns nothing; writes the file."""
    ch = CH[ct]; bpp = max(1, ch * depth // 8)
    passes = ADAM if interlace else [(0, 0, 1, 1)]
    raw = bytearray()
    for x0, y0, dx, dy in passes:
        if x0 >= w or y0 >= h: continue
        prev = None
        for y in range(y0, h, dy):
            row = pack_row([s for x in range(x0, w, dx) for s in px[y][x]], depth)
            prev = prev if prev is not None else bytes(len(row))
            f = rnd.randrange(5) if filters is None else filters
            raw += bytes([f]) + filt(row, prev, bpp, f)
            prev = row
    if zmode == "stored": z = zlib.compress(bytes(raw), 0)
    elif zmode == "fixed":
        c = zlib.compressobj(6, zlib.DEFLATED, 15, 8, zlib.Z_FIXED); z = c.compress(bytes(raw)) + c.flush()
    else: z = zlib.compress(bytes(raw), 9)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace))
    if plte: out += chunk(b"PLTE", bytes(v for c in plte for v in c))
    if trns is not None: out += chunk(b"tRNS", trns)
    if len(z) > 50: out += chunk(b"IDAT", z[:len(z) // 3]) + chunk(b"IDAT", z[len(z) // 3:])   # split IDATs
    else: out += chunk(b"IDAT", z)
    out += chunk(b"IEND", b"")
    open(path, "wb").write(out)

def rand_px(w, h, ct, depth):
    mx = (1 << depth) - 1; ch = CH[ct]
    npal = 1 << min(depth, 8)
    return [[tuple(rnd.randint(0, (npal - 1) if ct == 3 else mx) if rnd.random() < .6 else ((x * 7 + y * 3 + k * 50) & mx)
                   for k in range(ch)) for x in range(w)] for y in range(h)]

def pillow_ref(path, ct, depth):
    im = Image.open(path)
    if im.mode.startswith("I;16"):                        # 16-bit gray: Pillow keeps 16 bits; we keep the high byte
        g = Image.new("L", im.size)
        g.putdata([v >> 8 for v in im.get_flattened_data()])
        return g
    if depth == 16 and ct == 4:                           # gray+alpha 16
        pass
    return im

SIZES = [(1, 1), (7, 5), (13, 11), (33, 17), (3, 20)]
valid = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}
for ct, depths in valid.items():
    for depth in depths:
        for inter in (0, 1):
            for w, h in SIZES:
                px = rand_px(w, h, ct, depth)
                plte = [tuple(rnd.randrange(256) for _ in range(3)) for _ in range(1 << min(depth, 8))] if ct == 3 else None
                trns = None
                if ct == 3 and rnd.random() < .5: trns = bytes(rnd.randrange(256) for _ in range(len(plte) - rnd.randrange(len(plte))))
                p = os.path.join(tmp, f"c{ct}d{depth}i{inter}_{w}x{h}.png")
                write_png(p, w, h, ct, depth, px, inter, plte, trns, rnd.choice(["default", "stored", "fixed"]))
                check_dec(os.path.basename(p), p, pillow_ref(p, ct, depth))

# colour-key transparency (tRNS on gray / RGB 8)
for ct, key in ((0, 120), (2, (10, 200, 30))):
    w, h = 21, 9
    px = [[(120,) if ct == 0 and (x + y) % 3 == 0 else ((10, 200, 30) if ct == 2 and (x + y) % 3 == 0 else tuple(rnd.randrange(256) for _ in range(CH[ct]))) for x in range(w)] for y in range(h)]
    t = struct.pack(">H", key) if ct == 0 else struct.pack(">HHH", *key)
    p = os.path.join(tmp, f"key{ct}.png")
    write_png(p, w, h, ct, 8, px, 0, None, t)
    check_dec(f"key{ct}", p, Image.open(p))

# every filter type forced on every row, big enough for multi-block zlib
for f in range(5):
    w, h = 300, 40
    px = [[((x * 5 + y) & 255, (x ^ y) & 255, (x * y) & 255) for x in range(w)] for y in range(h)]
    p = os.path.join(tmp, f"filter{f}.png")
    write_png(p, w, h, 2, 8, px, 0, filters=f)
    check_dec(f"filter{f}", p, Image.open(p))

# files Pillow writes itself
def photoish(w, h, mode):
    im = Image.new("RGBA", (w, h))
    im.putdata([((x * 255) // w, (y * 255) // h, ((x + y) * 3) & 255, 255 if mode != "RGBA" else (x * 4) & 255) for y in range(h) for x in range(w)])
    return im
for mode in ("RGB", "RGBA", "L", "LA", "P", "1"):
    im = photoish(61, 47, mode).convert(mode if mode != "P" else "RGB")
    if mode == "P": im = im.quantize(64)
    for opt in ({}, {"optimize": True}, {"compress_level": 0}, {"compress_level": 1}):
        p = os.path.join(tmp, f"pil_{mode}_{len(opt)}{opt.get('compress_level', '')}.png")
        im.save(p, **opt)
        check_dec(os.path.basename(p), p, Image.open(p))
big = photoish(700, 300, "RGBA"); p = os.path.join(tmp, "big.png"); big.save(p); check_dec("big", p, Image.open(p))
for mode, ext in (("RGB", "bmp"), ("RGBA", "bmp"), ("RGB", "ppm"), ("L", "ppm")):
    for w in (2, 3, 4, 5, 31):                          # row padding for BMP
        im = photoish(w, 9, mode).convert(mode)
        p = os.path.join(tmp, f"pil_{mode}_{w}.{ext}"); im.save(p)
        ref = Image.open(p)
        if mode == "RGBA" and ext == "bmp": ref = im          # Pillow reads 32-bit BI_RGB as BGRX; we honour the alpha byte
        check_dec(os.path.basename(p), p, ref)
# bottom-up and top-down BMP, 32-bit with alpha and BI_RGB with no alpha
def bmp(path, w, h, bpp, top, alpha):
    rb = (w * bpp // 8 + 3) & ~3
    body = bytearray()
    rows = [b"".join(bytes(((x * 9 + y) & 255, (x * 3) & 255, (y * 11) & 255) + ((200 if alpha else 0,) if bpp == 32 else ()))
                     for x in range(w)) for y in range(h)]
    for y in (range(h) if top else reversed(range(h))): body += rows[y] + bytes(rb - len(rows[y]))
    open(path, "wb").write(b"BM" + struct.pack("<IHHI", 54 + len(body), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, -h if top else h, 1, bpp, 0, len(body), 0, 0, 0, 0) + body)
for bpp, top, alpha in ((24, 0, 0), (24, 1, 0), (32, 0, 1), (32, 1, 0)):
    p = os.path.join(tmp, f"bmp{bpp}{top}{alpha}.bmp"); bmp(p, 7, 5, bpp, top, alpha)
    im = Image.open(p)
    im = im.convert("RGB")                               # Pillow drops the 32-bit alpha byte; we honour it
    if alpha: im.putalpha(200)
    check_dec(f"bmp{bpp}{top}{alpha}", p, im)
# PPM with comments and maxval 65535
p = os.path.join(tmp, "c16.ppm")
open(p, "wb").write(b"P6\n# hi\n2 1\n65535\n" + struct.pack(">HHHHHH", 65535, 0, 32768, 0, 65535, 256))
check_dec("ppm16", p, Image.frombytes("RGB", (2, 1), bytes([255, 0, 128, 0, 255, 1])))

# ---------- JPEG ----------
JPEG_TOL = int(os.environ.get("JPEG_TOL", 1))

def check_jpeg(name, file, im, tol=JPEG_TOL):
    global fails, count
    count += 1
    ref = os.path.join(tmp, "ref")
    ref_from(im, ref)
    r = run("dec", file, ref, str(tol))
    if r.returncode:
        fails += 1
        print(f"FAIL jpeg {name}: {r.stdout.strip()} {r.stderr[:300]}")

def scene(w, h, mode="RGB"):
    """smooth gradients + hard edges + noise: exercises chroma upsampling and every coefficient range"""
    from PIL import ImageDraw
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255 // max(1, w - 1)), (y * 255 // max(1, h - 1)), ((x + y) * 255 // max(1, w + h - 2)))
    d = ImageDraw.Draw(im)
    d.ellipse([w // 5, h // 5, w * 4 // 5, h * 3 // 4], fill=(230, 30, 40))
    d.rectangle([w // 2, h // 3, w - 1, h // 2 + 2], fill=(20, 200, 60))
    d.line([(0, 0), (w, h)], fill=(255, 255, 255), width=1 + w // 60)
    for _ in range(w * h // 12): px[rnd.randrange(w), rnd.randrange(h)] = (rnd.randrange(256),) * 3
    return im.convert(mode)

SIZES = [(1, 1), (7, 9), (8, 8), (16, 16), (17, 33), (31, 5), (100, 75), (129, 255), (333, 211)]
for (w, h) in SIZES:
    im = scene(w, h)
    for sub in (0, 1, 2):
        for prog in (False, True):
            q = (5, 40, 75, 95, 100)[(w + h + sub) % 5] if (w, h) != (100, 75) else 75
            p = os.path.join(tmp, f"j_{w}x{h}_s{sub}_p{int(prog)}_q{q}.jpg")
            try: im.save(p, quality=q, subsampling=sub, progressive=prog)
            except OSError: print("skip (Pillow can't write)", os.path.basename(p)); continue
            check_jpeg(os.path.basename(p), p, Image.open(p))
for sub in ("4:4:0", "4:1:1"):
    try:
        im = scene(45, 38); p = os.path.join(tmp, f"j_sub{sub.replace(':', '')}.jpg")
        im.save(p, quality=80, subsampling=sub); check_jpeg(f"sub{sub}", p, Image.open(p))
    except Exception as e:
        print("skip subsampling", sub, e)
for (w, h) in ((64, 48), (101, 67)):
    im = scene(w, h)
    for kw in ({"restart_marker_blocks": 1}, {"restart_marker_blocks": 5}, {"restart_marker_rows": 1}, {"optimize": True},
               {"progressive": True, "optimize": True}, {"progressive": True, "restart_marker_blocks": 3},
               {"subsampling": 0, "restart_marker_blocks": 2}, {"quality": 1}, {"qtables": "web_high"}):
        p = os.path.join(tmp, f"jk_{w}_{'_'.join(f'{k}{v}' for k, v in kw.items())}.jpg")
        kw = dict(kw); kw.setdefault("quality", 85)
        im.save(p, **kw)
        check_jpeg(os.path.basename(p), p, Image.open(p))
    g = scene(w, h, "L")
    for prog in (False, True):
        p = os.path.join(tmp, f"jg_{w}_{int(prog)}.jpg"); g.save(p, quality=90, progressive=prog)
        check_jpeg(os.path.basename(p), p, Image.open(p))
big = photoish(1280, 800, "RGB").convert("RGB"); p = os.path.join(tmp, "jbig.jpg"); big.save(p, quality=85); check_jpeg("big", p, Image.open(p))
p = os.path.join(tmp, "jbig_p.jpg"); big.save(p, quality=85, progressive=True); check_jpeg("big progressive", p, Image.open(p))
from PIL import ImageOps
for o in range(1, 9):                                  # EXIF orientation, applied like ImageOps.exif_transpose
    im = scene(40, 24)
    ex = Image.Exif(); ex[0x0112] = o
    p = os.path.join(tmp, f"jo{o}.jpg"); im.save(p, quality=95, exif=ex.tobytes(), subsampling=0)
    check_jpeg(f"orient{o}", p, ImageOps.exif_transpose(Image.open(p)))
    p = os.path.join(tmp, f"jo{o}_le.jpg")
    mm = b"Exif\0\0MM\0*\0\0\0\x08\0\x01\x01\x12\0\x03\0\0\0\x01" + struct.pack(">H", o) + b"\0\0\0\0\0\0"   # big-endian TIFF
    im.save(p, quality=95, exif=mm, subsampling=0)
    check_jpeg(f"orient{o}b", p, ImageOps.exif_transpose(Image.open(p)))

# ---------- GIF ----------
def gif_ref(path, out):
    im = Image.open(path)
    n = getattr(im, "n_frames", 1)
    b = bytearray(struct.pack("<III", im.size[0], im.size[1], n))
    for i in range(n):
        im.seek(i)
        b += struct.pack("<I", im.info.get("duration", 0)) + im.convert("RGBA").tobytes()
    open(out, "wb").write(b)

def check_gif(name, file):
    global fails, count
    count += 1
    ref = os.path.join(tmp, "gref")
    gif_ref(file, ref)
    r = run("anim", file, ref)
    if r.returncode:
        fails += 1
        print(f"FAIL gif {name}: {r.stdout.strip()}")
        if os.environ.get("KEEP"): import shutil; shutil.copy(file, os.environ["KEEP"])
    im = Image.open(file); im.seek(0)                  # image_decode (first frame only) must equal frame 0
    check_dec(name + " first", file, im)

def lzw_enc(data, mb):
    clear, eoi = 1 << mb, (1 << mb) + 1
    out, acc, nb = bytearray(), 0, 0
    def put(c, size):
        nonlocal acc, nb
        acc |= c << nb; nb += size
        while nb >= 8: out.append(acc & 255); acc >>= 8; nb -= 8
    size, nxt, tab, w = mb + 1, eoi + 1, {}, b""
    put(clear, size)
    for c in data:
        wc = w + bytes([c])
        if len(wc) == 1 or wc in tab: w = wc; continue
        put(tab[w] if len(w) > 1 else w[0], size)
        tab[wc] = nxt; nxt += 1
        if nxt - 1 == 1 << size and size < 12: size += 1
        if nxt == 4096: put(clear, size); tab, nxt, size = {}, eoi + 1, mb + 1
        w = bytes([c])
    if w: put(tab[w] if len(w) > 1 else w[0], size)
    put(eoi, size)
    if nb: out.append(acc & 255)
    return bytes(out)

def sub_blocks(b, step=255):
    o = bytearray()
    for i in range(0, len(b), step): o += bytes([min(step, len(b) - i)]) + b[i:i + step]
    return bytes(o) + b"\0"

def write_gif(path, W, H, frames, gpal=None, bg=0, loop=None):
    """frame: dict(x, y, w, h, px=bytes row-major palette indices, pal=None|[(r,g,b)..], interlace, disp, tr, delay, mb)"""
    o = bytearray(b"GIF89a" + struct.pack("<HH", W, H))
    if gpal:
        bits = max(1, (len(gpal) - 1).bit_length())
        o += bytes([0x80 | (bits - 1) << 4 | (bits - 1), bg, 0]) + b"".join(bytes(c) for c in gpal) + bytes(3 * ((1 << bits) - len(gpal)))
    else: o += bytes([0, bg, 0])
    if loop is not None: o += b"\x21\xff\x0bNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\0"
    for f in frames:
        o += b"\x21\xf9\x04" + bytes([f.get("disp", 0) << 2 | (1 if f.get("tr") is not None else 0)]) + struct.pack("<H", f.get("delay", 0)) + bytes([f.get("tr") or 0, 0])
        px, w, h = f["px"], f["w"], f["h"]
        if f.get("interlace"):
            rows = [r for st, step in ((0, 8), (4, 8), (2, 4), (1, 2)) for r in range(st, h, step)]
            px = b"".join(px[r * w:(r + 1) * w] for r in rows)
        pal = f.get("pal"); flags = 0x40 if f.get("interlace") else 0
        pb = b""
        if pal:
            bits = max(1, (len(pal) - 1).bit_length()); flags |= 0x80 | (bits - 1)
            pb = b"".join(bytes(c) for c in pal) + bytes(3 * ((1 << bits) - len(pal)))
        mb = f.get("mb", 2)
        o += b"\x2c" + struct.pack("<HHHH", f["x"], f["y"], w, h) + bytes([flags]) + pb + bytes([mb]) + sub_blocks(lzw_enc(px, mb), f.get("chunk", 255))
    o += b"\x3b"
    open(path, "wb").write(o)

def rpal(n): return [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(n)]
def rpx(w, h, n, runs=True):
    b = bytearray()
    while len(b) < w * h:
        v = rnd.randrange(n); b += bytes([v]) * (rnd.randrange(1, 9) if runs else 1)
    return bytes(b[:w * h])

from PIL import ImageDraw
def anim_frames(w, h, n):
    fr = []
    for i in range(n):
        im = Image.new("RGB", (w, h), (10 * i, 40, 90))
        ImageDraw.Draw(im).ellipse([i * 3, i * 2, i * 3 + w // 2, i * 2 + h // 2], fill=(250 - 20 * i, 200, 30 * i % 256))
        fr.append(im)
    return fr

for (w, h) in ((1, 1), (9, 7), (33, 17), (64, 64), (130, 70)):          # Pillow-written
    fr = anim_frames(w, h, 4)
    p = os.path.join(tmp, f"g_{w}x{h}.gif"); fr[0].save(p, save_all=True, append_images=fr[1:], duration=[30, 50, 70, 0], loop=0)
    check_gif(os.path.basename(p), p)
    p = os.path.join(tmp, f"g_{w}x{h}_ni.gif"); fr[0].save(p, save_all=True, append_images=fr[1:], duration=40, interlace=False, optimize=False)
    check_gif(os.path.basename(p), p)
    p = os.path.join(tmp, f"g_{w}x{h}_still.gif"); fr[0].save(p); check_gif(os.path.basename(p), p)
fr = anim_frames(50, 40, 5)
for disp in (0, 1, 2, 3):                                   # with transparency, disposal is unambiguous
    q = [f.convert("P", palette=Image.ADAPTIVE, colors=64) for f in fr]
    p = os.path.join(tmp, f"g_disp{disp}.gif")
    q[0].save(p, save_all=True, append_images=q[1:], duration=20, disposal=disp, transparency=0)
    check_gif(os.path.basename(p), p)
rgba = []
for i in range(4):
    im = Image.new("RGBA", (40, 30), (0, 0, 0, 0))
    ImageDraw.Draw(im).rectangle([i * 6, 4, i * 6 + 12, 24], fill=(200, 40 * i, 90, 255))
    rgba.append(im)
p = os.path.join(tmp, "g_rgba.gif"); rgba[0].save(p, save_all=True, append_images=rgba[1:], duration=60, disposal=2, loop=3)
check_gif("rgba disposal2", p)

def compose(W, H, gp, frames):
    """independent GIF compositor (spec semantics, disposal 2 -> transparent, 3 -> state before the frame): reference blob"""
    cv, saved, prev, out = [(0, 0, 0, 0)] * (W * H), None, None, []
    for f in frames:
        if prev and prev["disp"] == 2:
            for y in range(prev["y"], prev["y"] + prev["h"]):
                for x in range(prev["x"], prev["x"] + prev["w"]): cv[y * W + x] = (0, 0, 0, 0)
        elif prev and prev["disp"] == 3: cv = list(saved)
        if f["disp"] == 3: saved = list(cv)
        pal = f.get("pal") or gp
        for i, v in enumerate(f["px"]):
            if v != f.get("tr"): cv[(f["y"] + i // f["w"]) * W + f["x"] + i % f["w"]] = tuple(pal[v]) + (255,)
        out.append(struct.pack("<I", f["delay"] * 10) + b"".join(bytes(c) for c in cv))
        prev = f
    return struct.pack("<III", W, H, len(frames)) + b"".join(out)

def check_gif_own(name, file, W, H, gp, frames):
    global fails, count
    count += 1
    ref = os.path.join(tmp, "gref2")
    open(ref, "wb").write(compose(W, H, gp, frames))
    r = run("anim", file, ref)
    if r.returncode:
        fails += 1
        print(f"FAIL gif {name} (own reference): {r.stdout.strip()}")

# hand-written: offsets, local palettes, interlace, disposal 1/2/3, transparency, odd sub-block sizes. Checked against
# our own compositor above (Pillow disposes to the palette background and lags disposal 3, so it is only the oracle
# for files with disposal 0/1)
for case in range(24):
    W, H = rnd.randrange(1, 70), rnd.randrange(1, 60)
    gp = rpal(rnd.choice((2, 4, 16, 256)))
    frames = []
    for i in range(rnd.randrange(1, 6)):
        fw, fh = rnd.randrange(1, W + 1), rnd.randrange(1, H + 1)
        fx, fy = rnd.randrange(0, W - fw + 1), rnd.randrange(0, H - fh + 1)
        local = rpal(rnd.choice((2, 8, 32))) if rnd.random() < .4 else None
        n = len(local or gp)
        tr = rnd.randrange(n) if rnd.random() < .5 else None
        disp = rnd.choice((0, 1, 3)) if tr is None else rnd.choice((0, 1, 2, 3))
        mb = max(2, (n - 1).bit_length())
        frames.append(dict(x=fx, y=fy, w=fw, h=fh, px=rpx(fw, fh, n, rnd.random() < .7), pal=local, interlace=rnd.random() < .5,
                           disp=disp, tr=tr, delay=rnd.randrange(0, 30), mb=rnd.choice((mb, 8)), chunk=rnd.choice((255, 7, 1))))
    f0 = frames[0]                                          # first frame covers the screen (Pillow fills the rest with the background)
    f0.update(x=0, y=0, w=W, h=H, px=rpx(W, H, len(f0.get("pal") or gp)))
    p = os.path.join(tmp, f"gh{case}.gif"); write_gif(p, W, H, frames, gp, loop=rnd.choice((None, 0, 2)))
    check_gif_own(f"hand{case}", p, W, H, gp, frames)
    if all(f["disp"] < 2 for f in frames): check_gif(f"hand{case} vs Pillow", p)
W, H = 200, 120                                             # incompressible: the 4096-entry table fills and resets
p = os.path.join(tmp, "gh_big.gif"); write_gif(p, W, H, [dict(x=0, y=0, w=W, h=H, px=rpx(W, H, 256, False), mb=8)], rpal(256))
check_gif("lzw table full", p)

# ---------- encoder ----------
def check_enc(name, im):
    global fails, count
    count += 1
    ref, out = os.path.join(tmp, "eref"), os.path.join(tmp, "eout.png")
    ref_from(im, ref)
    r = run("enc", ref, out)
    if r.returncode: fails += 1; print(f"FAIL encode {name}: rc {r.returncode}"); return
    try:
        Image.open(out).verify()                          # checks every CRC
        back = Image.open(out); back.load()
        data = open(out, "rb").read()
        idat = b""; i = 8
        while i < len(data):
            n, = struct.unpack(">I", data[i:i + 4]); t = data[i + 4:i + 8]
            if t == b"IDAT": idat += data[i + 8:i + 8 + n]
            i += 12 + n
        zlib.decompress(idat)                             # checks the Adler-32
        assert back.convert("RGBA").tobytes() == im.convert("RGBA").tobytes(), "pixels differ"
        assert back.mode == ("RGB" if all(p[3] == 255 for p in im.convert("RGBA").get_flattened_data()) else "RGBA"), back.mode
    except Exception as e:
        fails += 1; print(f"FAIL encode {name}: {e}")

check_enc("1x1", Image.new("RGBA", (1, 1), (1, 2, 3, 255)))
check_enc("flat", Image.new("RGBA", (500, 400), (9, 99, 199, 255)))
check_enc("wide", photoish(1000, 1, "RGB")); check_enc("tall", photoish(1, 777, "RGB"))
check_enc("grad", photoish(300, 200, "RGB")); check_enc("alpha", photoish(123, 77, "RGBA"))
check_enc("noise", Image.frombytes("RGBA", (200, 150), bytes(rnd.randrange(256) for _ in range(200 * 150 * 4))))
check_enc("noise-opaque", Image.frombytes("RGB", (97, 83), bytes(rnd.randrange(256) for _ in range(97 * 83 * 3))))

# ---------- resampler ----------
count += 1
r = run("sample")
if r.returncode: fails += 1; print("FAIL sample:", r.stdout.strip(), r.stderr[:300])

# ---------- robustness: corrupted input must not crash (ASan/UBSan when the binary has them) ----------
for f in ("c6d8i1_33x17.png", "c3d4i1_13x11.png", "c0d16i0_33x17.png", "pil_RGB_0.png", "bmp2400.bmp", "pil_RGB_5.ppm", "j_100x75_s2_p0_q75.jpg", "j_100x75_s2_p1_q75.jpg", "jk_64_restart_marker_blocks5.jpg",
          "jg_101_1.jpg", "jo6.jpg", "g_64x64.gif", "g_disp3.gif", "gh3.gif", "gh_big.gif"):
    count += 1
    r = run("fuzz", os.path.join(tmp, f))
    if r.returncode: fails += 1; print(f"FAIL fuzz {f}: {r.stderr[:400]}")

print(f"{count - fails}/{count} image tests passed")
sys.exit(1 if fails else 0)
