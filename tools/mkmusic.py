#!/usr/bin/env python3
"""Generate the sample tracks in root/home/Music: two short original pieces synthesised here (no samples, nothing
copyrighted), encoded with the host's ffmpeg: low-tide.mp3 (64 kbps, ID3v2 tags + a PNG cover in APIC) and
pixel-rain.ogg (Vorbis, with tags). Together ~200 KB.  Run: python3 tools/mkmusic.py   (needs ffmpeg + Pillow)"""
import math, os, random, struct, subprocess, tempfile, wave
from PIL import Image, ImageDraw

RATE = 44100
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "root", "home", "Music")

def hz(m): return 440.0 * 2 ** ((m - 69) / 12)

class Mix:
    def __init__(self, secs): self.n = int(RATE * secs); self.l = [0.0] * self.n; self.r = [0.0] * self.n
    def add(self, t, dur, fn, amp, pan=0.5):
        """fn(t_seconds) -> sample, over dur seconds starting at t; pan 0 = left .. 1 = right"""
        a = int(t * RATE)
        for i in range(min(int(dur * RATE), self.n - a)):
            s = fn(i / RATE) * amp
            self.l[a + i] += s * (1 - pan); self.r[a + i] += s * pan
    def echo(self, delay, gain):                      # one cheap feedback echo, ping-ponged between the channels
        d = int(delay * RATE)
        for i in range(d, self.n):
            self.l[i] += self.r[i - d] * gain; self.r[i] += self.l[i - d] * gain
    def save(self, path, peak=0.8):
        m = max(max(map(abs, self.l)), max(map(abs, self.r)), 1e-9)
        k = peak * 32767 / m
        with wave.open(path, "wb") as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(b"".join(struct.pack("<hh", int(a * k), int(b * k)) for a, b in zip(self.l, self.r)))

def pad(m, dur):
    f = hz(m)
    def fn(t):
        env = min(1, t / 0.6) * min(1, (dur - t) / 0.8)
        return (math.sin(6.2832 * f * t) + math.sin(6.2832 * f * 1.004 * t) * 0.8 + 0.25 * math.sin(12.566 * f * t)) * env
    return fn

def pluck(m, decay=5.0, harm=0.35):
    f = hz(m)
    return lambda t: (math.sin(6.2832 * f * t) + harm * math.sin(12.566 * f * t) + harm * 0.4 * math.sin(18.85 * f * t)) * math.exp(-decay * t) * min(1, t / 0.004)

def square(m, decay=7.0):
    f = hz(m)
    return lambda t: (1.0 if (f * t) % 1 < 0.5 else -1.0) * math.exp(-decay * t)

def low_tide(path):
    """19 s, 76 bpm: Am - F - C - G pads under a pentatonic melody"""
    beat = 60 / 76
    m = Mix(20.5)
    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
    for bar in range(6):
        t = bar * 4 * beat
        for i, n in enumerate(chords[bar % 4]):
            m.add(t, 4 * beat + 0.4, pad(n, 4 * beat + 0.4), 0.16, 0.25 + 0.25 * i)
        m.add(t, 4 * beat, pluck(chords[bar % 4][0] - 12, 2.0, 0.2), 0.35, 0.5)
    melody = [(0, 69, 1.5), (1.5, 72, 0.5), (2, 76, 1), (3, 74, 1), (4, 72, 2), (6, 67, 2), (8, 69, 1.5), (9.5, 65, 0.5), (10, 69, 1),
              (11, 72, 1), (12, 71, 2), (14, 67, 2), (16, 64, 1.5), (17.5, 67, 0.5), (18, 72, 2), (20, 69, 4)]
    for b, n, d in melody:
        m.add(b * beat, d * beat + 0.8, pluck(n, 3.0), 0.5, 0.65)
    m.echo(beat * 0.75, 0.35)
    return m

def pixel_rain(path):
    """14 s, 140 bpm: square arpeggios over a bass line and noise hats"""
    beat = 60 / 140
    m = Mix(14.4)
    rnd = random.Random(7)
    prog = [(57, [0, 3, 7]), (53, [0, 4, 7]), (60, [0, 4, 7]), (55, [0, 4, 7])]
    for bar in range(8):
        root, ch = prog[bar % 4]
        for s in range(16):
            t = (bar * 16 + s) * beat / 4
            m.add(t, 0.2, square(root + 12 + ch[s % 3] + (12 if s % 8 >= 5 else 0), 12.0), 0.2, 0.2 if s % 2 else 0.8)
            if s % 4 == 0: m.add(t, 0.5, pluck(root - 24, 6.0, 0.5), 0.6, 0.5)
            if s % 2 == 1:
                hat = [rnd.uniform(-1, 1) for _ in range(2000)]
                m.add(t, 0.04, lambda x, h=hat: h[min(int(x * RATE), 1999)] * math.exp(-90 * x), 0.12, 0.35 + 0.3 * (s % 4 == 3))
    m.echo(beat * 0.75, 0.25)
    return m

def cover(path):
    im = Image.new("RGB", (128, 128))
    d = ImageDraw.Draw(im)
    for y in range(128): d.line([(0, y), (127, y)], fill=(int(30 + y * 0.5), int(20 + y * 0.4), int(90 + y * 1.1)))
    for i, r in enumerate((54, 40, 26, 12)): d.ellipse((64 - r, 70 - r, 64 + r, 70 + r), outline=(255, 150 + i * 25, 200), width=3)
    d.rectangle((0, 0, 127, 127), outline=(255, 255, 255))
    im.quantize(32).save(path, optimize=True)

def ff(*a): subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *a], check=True)

os.makedirs(OUT, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    low_tide(0).save(tmp + "/a.wav"); pixel_rain(0).save(tmp + "/b.wav"); cover(tmp + "/c.png")
    ff("-i", tmp + "/a.wav", "-i", tmp + "/c.png", "-map", "0:a", "-map", "1:v", "-c:a", "libmp3lame", "-b:a", "64k", "-c:v", "copy",
       "-id3v2_version", "3", "-metadata", "title=Low Tide", "-metadata", "artist=VibeOS", "-metadata", "album=Sample Tracks",
       "-metadata:s:v", "title=Cover", "-metadata:s:v", "comment=Cover (front)", OUT + "/low-tide.mp3")
    ff("-i", tmp + "/b.wav", "-c:a", "libvorbis", "-q:a", "0", "-metadata", "title=Pixel Rain", "-metadata", "artist=VibeOS",
       "-metadata", "album=Sample Tracks", OUT + "/pixel-rain.ogg")
for f in ("low-tide.mp3", "pixel-rain.ogg"): print(f, os.path.getsize(os.path.join(OUT, f)), "bytes")
