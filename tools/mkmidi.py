#!/usr/bin/env python3
"""Write root/home/Music/vibe-theme.mid: a short original piece (Am F C G, piano + bass + lead +
strings + drums), format 1, 480 PPQ. Deterministic; no samples, nothing copyrighted.
  python3 tools/mkmidi.py [OUT.mid]"""
import os, struct, sys

PPQ = 480
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def vlq(n):
    b = [n & 127]
    while n >> 7:
        n >>= 7
        b.append(n & 127 | 128)
    return bytes(reversed(b))

def track(events):
    """events: (tick, bytes) -> MTrk chunk with delta times"""
    out, last = b"", 0
    for t, e in sorted(events, key=lambda x: x[0]):   # stable: keeps program/volume before notes
        out += vlq(t - last) + e
        last = t
    out += vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(out)) + out

def notes(ch, items):
    """items: (start_beat, length_beats, note, velocity)"""
    ev = []
    for b, l, n, v in items:
        ev.append((int(b * PPQ), bytes([0x90 | ch, n, v])))
        ev.append((int((b + l) * PPQ) - 1, bytes([0x80 | ch, n, 0])))
    return ev

def setup(ch, prog, vol, pan=64):
    return [(0, bytes([0xC0 | ch, prog])), (0, bytes([0xB0 | ch, 7, vol])), (0, bytes([0xB0 | ch, 10, pan]))]

# Am F C G, 4 beats each; 8 bars per pass, 2 passes (the second adds the lead and strings)
CH = [(57, [57, 60, 64]), (53, [53, 57, 60]), (60, [60, 64, 67]), (55, [55, 59, 62])]   # root, triad
LEAD = [  # per bar: (beat, length, note)
    [(0, 1, 76), (1, 0.5, 72), (1.5, 0.5, 74), (2, 2, 76)],
    [(0, 1, 77), (1, 1, 74), (2, 1, 72), (3, 1, 69)],
    [(0, 1, 72), (1, 0.5, 74), (1.5, 0.5, 76), (2, 1.5, 79), (3.5, 0.5, 76)],
    [(0, 2, 74), (2, 1, 71), (3, 1, 67)],
]

piano, bass, lead, strings, drums = [], [], [], [], []
for bar in range(16):
    base, (root, chord) = bar * 4, CH[bar % 4]
    for i in range(8):                                  # eighth-note arpeggio
        piano.append((base + i * 0.5, 0.5, chord[(0, 1, 2, 1, 0, 1, 2, 1)[i]] + (12 if i % 4 == 3 else 0), 70 + 10 * (i % 2 == 0)))
    for b, n in ((0, root - 24), (1.5, root - 24), (2, root - 12), (3, root - 24)):
        bass.append((base + b, 1 if b != 1.5 else 0.5, n, 95))
    for i in range(4):
        drums += [(base + i, 0.1, 42, 60), (base + i + 0.5, 0.1, 42, 45)]
    drums += [(base, 0.1, 36, 110), (base + 2, 0.1, 36, 105), (base + 2.5, 0.1, 36, 80), (base + 1, 0.1, 38, 100), (base + 3, 0.1, 38, 100)]
    if bar >= 8:
        for b, l, n in LEAD[bar % 4]:
            lead.append((base + b, l * 0.95, n, 100))
        strings += [(base, 4, n - 12 if n > 60 else n, 60) for n in chord]
    if bar == 15:
        drums.append((base + 3.5, 0.1, 49, 100))

meta = [(0, b"\xff\x51\x03" + (60_000_000 // 112).to_bytes(3, "big")), (0, b"\xff\x03\x0bVibe theme!")]
tracks = [track(meta),
          track(setup(0, 0, 100, 50) + notes(0, piano)),
          track(setup(1, 33, 110, 64) + notes(1, bass)),
          track(setup(2, 73, 100, 80) + notes(2, lead)),
          track(setup(3, 48, 90, 40) + notes(3, strings)),
          track(setup(9, 0, 110) + notes(9, drums))]
data = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), PPQ) + b"".join(tracks)
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "root/home/Music/vibe-theme.mid")
open(out, "wb").write(data)
print(f"{out}: {len(data)} bytes, {16 * 4 * 60 / 112:.1f} s")
