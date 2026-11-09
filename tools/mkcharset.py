#!/usr/bin/env python3
"""Bakes the legacy charset tables the browser decodes into root/usr/share/vibeos/charset.bin.gz.

usage: tools/mkcharset.py [OUT]     (Python's codecs are the source; the output is committed)

File (gzip): "VCHS", u32 n, then n x 28-byte entries: name[16] (WHATWG-ish label, lower case), u8 kind (0 single byte: 128 u16 for
0x80..0xFF; 1 double byte), u8 kana (0, 1 = Shift_JIS 0xA1..0xDF halfwidth katakana, 2 = EUC-JP 0x8E + katakana), u8 lead_min,
lead_max, trail_min, trail_max, u8 pad[2], u32 offset (bytes into the data after the entries), then the data: u16 little endian
code points, 0 = unmapped (lead rows of (lead_max-lead_min+1) x (trail_max-trail_min+1) for double byte).
user/bin/browser/fetch.c (to_utf8) reads it the first time a page declares one of these charsets.
"""
import gzip, os, struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "root/usr/share/vibeos/charset.bin.gz"
SINGLE = [("windows-1250", "cp1250"), ("windows-1253", "cp1253"), ("windows-1254", "cp1254"), ("windows-1255", "cp1255"),
          ("windows-1256", "cp1256"), ("windows-1257", "cp1257"), ("windows-1258", "cp1258"), ("iso-8859-3", "iso8859_3"),
          ("iso-8859-4", "iso8859_4"), ("iso-8859-5", "iso8859_5"), ("iso-8859-6", "iso8859_6"), ("iso-8859-7", "iso8859_7"),
          ("iso-8859-8", "iso8859_8"), ("iso-8859-9", "iso8859_9"), ("iso-8859-13", "iso8859_13"), ("koi8-u", "koi8_u"),
          ("ibm866", "cp866"), ("windows-874", "cp874"), ("macintosh", "mac_roman")]
DOUBLE = [("gbk", "gbk", (0x81, 0xFE, 0x40, 0xFE), 0), ("big5", "big5", (0x81, 0xFE, 0x40, 0xFE), 0),
          ("shift_jis", "cp932", (0x81, 0xFC, 0x40, 0xFC), 1), ("euc-jp", "euc_jp", (0xA1, 0xFE, 0xA1, 0xFE), 2),
          ("euc-kr", "cp949", (0x81, 0xFE, 0x41, 0xFE), 0)]


def cp_of(codec, bs):
    try:
        s = bs.decode(codec)
    except UnicodeDecodeError:
        return 0
    return ord(s) if len(s) == 1 and ord(s) < 0x10000 and s != "�" else 0


ents, data = [], b""
for name, codec in SINGLE:
    t = [cp_of(codec, bytes([b])) or b for b in range(0x80, 0x100)]
    ents.append((name, 0, 0, (0, 0, 0, 0), len(data))); data += struct.pack("<128H", *t)
for name, codec, (l0, l1, t0, t1), kana in DOUBLE:
    t = [cp_of(codec, bytes([l, tr])) for l in range(l0, l1 + 1) for tr in range(t0, t1 + 1)]
    ents.append((name, 1, kana, (l0, l1, t0, t1), len(data))); data += struct.pack(f"<{len(t)}H", *t)
head = b"VCHS" + struct.pack("<I", len(ents))
for name, kind, kana, (l0, l1, t0, t1), off in ents:
    head += name.encode().ljust(16, b"\0") + struct.pack("<BBBBBB2xI", kind, kana, l0, l1, t0, t1, off)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as fo:
    fo.write(gzip.compress(head + data, 9, mtime=0))
print(len(ents), "charsets,", len(head + data), "bytes raw,", os.path.getsize(OUT), "gz")
