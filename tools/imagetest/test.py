#!/usr/bin/env python3
"""Host test for user/lib/image.c. usage: test.py IMAGETEST_BINARY

Decoder: writes PNGs of every colour type / bit depth / interlace / filter / zlib mode with a small
pure-python writer (Pillow can't write interlaced or most 16-bit/low-depth files), plus files Pillow
itself writes (PNG, BMP, PPM), and compares our decode with Pillow's decode of the same file.
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
for f in ("c6d8i1_33x17.png", "c3d4i1_13x11.png", "c0d16i0_33x17.png", "pil_RGB_0.png", "bmp2400.bmp", "pil_RGB_5.ppm"):
    count += 1
    r = run("fuzz", os.path.join(tmp, f))
    if r.returncode: fails += 1; print(f"FAIL fuzz {f}: {r.stderr[:400]}")

print(f"{count - fails}/{count} image tests passed")
sys.exit(1 if fails else 0)
