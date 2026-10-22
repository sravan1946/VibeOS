#!/usr/bin/env python3
"""wavcmp.py FILE [CAPTURE.wav] [--skip S] - compare what the VM played (default build/audio.wav) with the host
decoding of FILE (ffmpeg, 48 kHz stereo); the two are aligned by cross-correlation, which also finds the MP3 encoder delay ffmpeg trims.
Prints the lag, the gain the codec/mixer applied (the AC'97 master volume attenuates), and the SNR after fitting it.
--skip S starts the comparison S seconds into the reference (when the capture holds something else first)."""
import array, math, os, subprocess, sys, wave

def mono(x): return [(x[i] + x[i + 1]) / 2 for i in range(0, len(x) - 1, 2)]

def main():
    a = sys.argv[1:]
    skip = float(a[a.index("--skip") + 1]) if "--skip" in a else 0
    files = [x for i, x in enumerate(a) if not x.startswith("--") and (i == 0 or a[i - 1] != "--skip")]
    cap = files[1] if len(files) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "audio.wav")
    d = open(cap, "rb").read()                                  # QEMU leaves the RIFF sizes 0 when killed: don't trust them
    ch = d[22]
    c = array.array("h", d[d.index(b"data") + 8:][: (len(d) - d.index(b"data") - 8) // 2 * 2])
    c = mono(c) if ch == 2 else list(c)
    r = array.array("h", subprocess.run(["ffmpeg", "-v", "error", "-i", files[0], "-f", "s16le", "-ar", "48000", "-ac", "2", "-"], check=True, capture_output=True).stdout)
    r = mono(r)[int(skip * 48000):]
    first = lambda x: next(i for i, v in enumerate(x) if abs(v) > 800)
    off = first(c) - first(r)                                    # capture index = reference index + off, roughly
    best = None
    for lag in range(off - 1500, off + 1500):
        e = 0.0
        n = 0
        for i in range(30000, 30000 + 24000, 6):
            j = i + lag
            if 0 <= j < len(c) and i < len(r): e += c[j] * r[i]; n += 1
        if best is None or e > best[0]: best = (e, lag)
    lag = best[1]
    n = min(len(r), len(c) - lag) - 1000
    num = sum(c[i + lag] * r[i] for i in range(1000, n, 2)); den = sum(r[i] * r[i] for i in range(1000, n, 2))
    g = num / den
    err = sum((c[i + lag] - g * r[i]) ** 2 for i in range(1000, n, 2)); sig = sum((g * r[i]) ** 2 for i in range(1000, n, 2))
    print(f"{cap}: lag {lag} samples ({lag / 48:.1f} ms), gain {g:.3f}, snr {10 * math.log10(sig / err):.1f} dB over {n / 48000:.1f} s of {len(r) / 48000:.1f} s")

if __name__ == "__main__":
    main()
