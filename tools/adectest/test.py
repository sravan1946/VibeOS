#!/usr/bin/env python3
"""test.py ADECTEST MUSICDIR: encode test signals with ffmpeg, decode them with adectest (user/music/adec.c) and with
ffmpeg itself, and compare. Files at 48 kHz need no resampling, so they must match ffmpeg's decoder to rounding
noise (SNR > 60 dB); other rates also go through our linear resampler against ffmpeg's, so the bar is lower.
Skips (exit 0) when ffmpeg isn't installed."""
import array, math, os, shutil, struct, subprocess, sys, tempfile, wave

if not shutil.which("ffmpeg"):
    print("adectest: ffmpeg not found, skipped"); sys.exit(0)
exe, music = sys.argv[1], sys.argv[2]
fails = 0

def run(*a, **kw): return subprocess.run(a, check=True, capture_output=True, **kw)

def tone_wav(path, rate, secs, chans, bits=16):
    """two tones plus a slow sweep, different on each channel, so channel mix-ups and drift show"""
    n = int(rate * secs)
    fr = []
    for i in range(n):
        t = i / rate
        l = 0.3 * math.sin(6.2832 * 440 * t) + 0.2 * math.sin(6.2832 * (300 + 200 * t) * t)
        r = 0.3 * math.sin(6.2832 * 660 * t) + 0.2 * math.sin(6.2832 * 1200 * t)
        fr.append((l, r) if chans == 2 else (l,))
    with wave.open(path, "wb") as w:
        w.setnchannels(chans); w.setsampwidth(bits // 8); w.setframerate(rate)
        if bits == 16: w.writeframes(b"".join(struct.pack("<" + "h" * chans, *[int(x * 32767) for x in f]) for f in fr))
        else: w.writeframes(bytes(int(x * 127) + 128 for f in fr for x in f))

def ours(path, seek=None):
    p = subprocess.run([exe, path] + ([str(seek)] if seek is not None else []), capture_output=True)
    if p.returncode: raise SystemExit(f"adectest {path}: {p.stderr.decode()}")
    meta = dict(l.split("=", 1) for l in p.stderr.decode().splitlines() if "=" in l)
    return array.array("h", p.stdout), meta

def ref(path, rate, chans=2):
    """ffmpeg's decode at the file's own rate, then resampled to 48 kHz the way adec.c says it does (linear)"""
    af = ["-af", "pan=stereo|c0=c0|c1=c0"] if chans == 1 else []
    p = run("ffmpeg", "-v", "error", "-i", path, *af, "-f", "s16le", "-ar", str(rate), "-ac", "2", "-")
    x = array.array("h", p.stdout)
    if rate == 48000: return x
    n = (len(x) // 2 - 1) * 48000 // rate
    out = array.array("h", bytes(4 * n))
    for i in range(n):
        pos = i * rate / 48000
        k = int(pos); fr = pos - k
        for c in (0, 1): out[2 * i + c] = int(x[2 * k + c] + (x[2 * k + 2 + c] - x[2 * k + c]) * fr)
    return out

def snr(a, b, lags=range(-3, 4)):
    """SNR of a against reference b over the common length, trying a few lags (decoder delay differences)"""
    best = -99
    for lag in lags:
        x, y = (a[2 * lag:], b) if lag >= 0 else (a, b[-2 * lag:])
        n = min(len(x), len(y)) - 4000
        e = sum((x[i] - y[i]) ** 2 for i in range(2000, n, 3)) or 1
        s = sum(y[i] ** 2 for i in range(2000, n, 3))
        best = max(best, 10 * math.log10(s / e) if s else 0)
    return best

def check(name, ok, detail):
    global fails
    print(("ok   " if ok else "FAIL ") + name + "  " + detail)
    fails += not ok

tmp = tempfile.mkdtemp()
try:
    for rate in (48000, 44100, 22050):
        for chans in (2, 1):
            src = f"{tmp}/s{rate}{chans}.wav"
            tone_wav(src, rate, 4, chans)
            for ext, enc in (("mp3", ["-c:a", "libmp3lame", "-b:a", "128k", "-write_xing", "0"]), ("ogg", ["-c:a", "libvorbis", "-q:a", "5"])):
                f = f"{tmp}/t{rate}{chans}.{ext}"
                run("ffmpeg", "-v", "error", "-i", src, *enc, f)
                a, meta = ours(f)
                r = ref(f, rate, chans)
                need = 60 if rate == 48000 else 50
                s = snr(a, r)
                check(f"{ext} {rate} Hz {'stereo' if chans == 2 else 'mono'}", s > need and abs(len(a) - len(r)) < 6000,
                      f"snr {s:.1f} dB (> {need}), {len(a) // 2} vs {len(r) // 2} frames, len {meta['len_ms']} ms, {meta['info']}")
            if chans == 2:
                a, meta = ours(src); r = ref(src, rate)               # WAV, 16-bit
                s = snr(a, r)
                check(f"wav {rate} Hz stereo", s > (60 if rate == 48000 else 50), f"snr {s:.1f} dB, {meta['info']}")
    # 8-bit WAV
    tone_wav(f"{tmp}/w8.wav", 22050, 2, 1, 8)
    a, meta = ours(f"{tmp}/w8.wav"); r = ref(f"{tmp}/w8.wav", 22050, 1)
    check("wav 22050 Hz mono 8-bit", snr(a, r) > 40, f"snr {snr(a, r):.1f} dB")
    # seeking: decoding from 1500 ms must equal the tail of the full decode
    for ext in ("mp3", "ogg", "wav"):
        f = f"{tmp}/t480002.{ext}" if ext != "wav" else f"{tmp}/s480002.wav"
        full, _ = ours(f)
        part, _ = ours(f, 1500)
        k = 1500 * 48
        tail = full[2 * k:]
        n = min(len(tail), len(part)) - 4000
        e = sum((part[i] - tail[i]) ** 2 for i in range(2000, n, 3)) or 1
        s = sum(tail[i] ** 2 for i in range(2000, n, 3))
        v = 10 * math.log10(s / e)
        check(f"seek {ext}", v > 40 and abs(len(tail) - len(part)) < 3000, f"snr {v:.1f} dB vs the tail of the full decode, {len(part) // 2} vs {len(tail) // 2} frames")
    # the shipped samples: tags and embedded art
    for name, title, art in (("low-tide.mp3", "Low Tide", True), ("pixel-rain.ogg", "Pixel Rain", False)):
        a, meta = ours(os.path.join(music, name))
        r = ref(os.path.join(music, name), 44100)
        s = snr(a, r, range(-3, 4) if name.endswith("ogg") else range(1198, 1210))   # ffmpeg trims the MP3 encoder delay (1105 samples at 44.1 kHz); we play it, so the resampling phase differs by a fraction of a sample (hence the lower bar)
        check(name, meta["title"] == title and meta["artist"] == "VibeOS" and (int(meta["art"]) > 1000) == art and s > 30,
              f"title {meta['title']!r} artist {meta['artist']!r} art {meta['art']} B, snr {s:.1f} dB, {meta['len_ms']} ms")
    # junk must fail cleanly
    open(f"{tmp}/junk.mp3", "wb").write(os.urandom(5000))
    open(f"{tmp}/trunc.ogg", "wb").write(open(f"{tmp}/t480002.ogg", "rb").read()[:3000])
    for j in ("junk.mp3", "trunc.ogg"):
        p = subprocess.run([exe, f"{tmp}/{j}"], capture_output=True)
        check("junk " + j, p.returncode in (0, 1) and b"AddressSanitizer" not in p.stderr, f"exit {p.returncode}, {p.stderr.decode().splitlines()[0] if p.stderr else ''}")
finally:
    shutil.rmtree(tmp)
sys.exit(1 if fails else 0)
