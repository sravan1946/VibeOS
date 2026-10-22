#!/usr/bin/env python3
"""wavcheck.py [FILE.wav] - list the sounds in a capture (default build/audio.wav): one line per
non-silent stretch with start, length, RMS and zero-crossing frequency (good for single tones; polyphonic
stretches report a rough average, use --peak to find the strongest frequency with a small DFT instead; --tones N lists the N strongest
spectral peaks per stretch, which is how to see several simultaneous sources).
    python3 tools/wavcheck.py [--gap MS] [--peak] [--tones N] [FILE]"""
import cmath, math, os, struct, sys

def load(path):
    # QEMU's wav backend leaves the RIFF/data sizes 0 when the VM is killed, so don't trust them
    d = open(path, "rb").read()
    ch, rate = struct.unpack_from("<HI", d, 22)
    i = d.index(b"data") + 8
    n = (len(d) - i) // (2 * ch)
    raw = struct.unpack_from("<%dh" % (n * ch), d, i)
    return [sum(raw[k * ch:(k + 1) * ch]) / ch / 32768 for k in range(n)], rate

def zc_freq(s, rate):
    z = [i for i in range(1, len(s)) if s[i - 1] < 0 <= s[i]]
    return (len(z) - 1) * rate / (z[-1] - z[0]) if len(z) > 2 else 0.0

def peak_freq(s, rate, lo=40, hi=4000):
    s = s[:8192:4]                                     # decimated: a brute-force DFT over lo..hi Hz
    return max(range(lo, hi, 2), key=lambda f: abs(sum(x * cmath.rect(1, -2 * math.pi * f * i * 4 / rate) for i, x in enumerate(s))))

def top_tones(s, rate, n, lo=40, hi=6000):
    """the n strongest spectral peaks (Hz, relative level) in s, >= 40 Hz apart: a Goertzel-style scan over a Hann-windowed block"""
    s = s[len(s) // 2:][:16384:2]                      # 8192 samples (decimated x2) from the middle
    r = rate / 2
    w = [x * (0.5 - 0.5 * math.cos(2 * math.pi * i / (len(s) - 1))) for i, x in enumerate(s)]
    mag = []
    for f in range(lo, hi, 5):
        c = 2 * math.cos(2 * math.pi * f / r)
        a = b = 0.0
        for x in w: a, b = x + c * a - b, a
        mag.append((a * a + b * b - c * a * b, f))
    out = []
    for m, f in sorted(mag, reverse=True):
        if all(abs(f - g) >= 40 for _, g in out): out.append((m, f))
        if len(out) == n: break
    top = out[0][0] or 1
    return [(f, math.sqrt(m / top)) for m, f in out]

def segments(s, rate, gap_ms=60, thr=0.01):
    win = rate // 100                                  # 10 ms RMS windows
    act = [math.sqrt(sum(x * x for x in s[i:i + win]) / win) > thr for i in range(0, len(s) - win, win)]
    segs, start, quiet = [], None, 0
    for i, a in enumerate(act):
        if a:
            if start is None: start = i
            quiet = 0
        elif start is not None:
            quiet += 1
            if quiet * 10 >= gap_ms: segs.append((start, i - quiet + 1)); start = None
    if start is not None: segs.append((start, len(act)))
    return [(a * win, b * win) for a, b in segs]

def main():
    a = sys.argv[1:]
    gap = int(a[a.index("--gap") + 1]) if "--gap" in a else 60
    peak = "--peak" in a
    ntones = int(a[a.index("--tones") + 1]) if "--tones" in a else 0
    files = [x for i, x in enumerate(a) if not x.startswith("--") and (i == 0 or a[i - 1] not in ("--gap", "--tones"))]
    path = files[0] if files else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "audio.wav")
    s, rate = load(path)
    print(f"{path}: {len(s) / rate:.2f} s at {rate} Hz")
    for a0, b0 in segments(s, rate, gap):
        seg = s[a0:b0]
        rms = math.sqrt(sum(x * x for x in seg) / len(seg))
        f = peak_freq(seg[len(seg) // 8:], rate) if peak else zc_freq(seg[len(seg) // 8: -len(seg) // 8 or None], rate)
        print(f"  {a0 / rate:7.3f}s  len {len(seg) / rate * 1000:7.0f} ms  rms {rms:.3f}  {'peak' if peak else 'zc'} {f:8.1f} Hz")
        if ntones: print("      tones: " + ", ".join(f"{hz} Hz ({lv:.2f})" for hz, lv in top_tones(seg, rate, ntones)))

if __name__ == "__main__":
    main()
