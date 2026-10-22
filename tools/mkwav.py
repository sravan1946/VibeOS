#!/usr/bin/env python3
"""Generate root/home/Music/arpeggio.wav: a short original arpeggio, 22.05 kHz mono 16-bit (~150 KB).
Deliberately not 48 kHz so /bin/play's resampler gets exercised.  Run: python3 tools/mkwav.py"""
import math, os, struct, wave

RATE = 22050
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "root", "home", "Music", "arpeggio.wav")
# (note frequency in Hz, beats); 0.25 s per beat
TUNE = [(523.25, 1), (659.25, 1), (783.99, 1), (1046.50, 2), (783.99, 1), (659.25, 1), (880.00, 1),
        (698.46, 1), (587.33, 1), (783.99, 1), (523.25, 4)]
BEAT = 0.25

def note(f, beats):
    n = int(RATE * BEAT * beats * 1.15)            # a little tail rings into the next note
    out = []
    for i in range(n):
        t = i / RATE
        env = min(1, t / 0.005) * math.exp(-3.0 * t)   # 5 ms attack, exponential decay
        s = math.sin(2 * math.pi * f * t) + 0.3 * math.sin(4 * math.pi * f * t) + 0.1 * math.sin(6 * math.pi * f * t)
        out.append(s * env * 0.45)
    return out

total = int(RATE * BEAT * sum(b for _, b in TUNE)) + RATE // 2
buf = [0.0] * total
pos = 0
for f, b in TUNE:
    for i, s in enumerate(note(f, b)):
        if pos + i < total: buf[pos + i] += s
    pos += int(RATE * BEAT * b)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with wave.open(OUT, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in buf))
print(OUT, os.path.getsize(OUT), "bytes")
