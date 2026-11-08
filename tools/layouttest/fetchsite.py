#!/usr/bin/env python3
"""fetchsite.py URL OUTDIR - save a page and its <link rel=stylesheet> sheets for tools/layouttest/run.sh.
OUTDIR/page.html, OUTDIR/sheets/<url with non-alnum -> _>; render with LT_SHEETS=OUTDIR/sheets run.sh OUTDIR/page.html W out.png "" URL"""
import os, re, sys, urllib.request, urllib.parse
UA = "Mozilla/5.0 (X11; Linux i686; rv:128.0) Gecko/20100101 Firefox/128.0 VibeBrowser/1.0"
def get(u):
    r = urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": UA, "Accept-Encoding": "identity"}), timeout=20)
    return r.geturl(), r.read()
url, out = sys.argv[1], sys.argv[2]
os.makedirs(out + "/sheets", exist_ok=True)
final, body = get(url)
open(out + "/page.html", "wb").write(body)
html = body.decode("utf-8", "replace")
seen = set()
def sheet(u, depth=0):
    if u in seen or depth > 2: return
    seen.add(u)
    try: _, b = get(u)
    except Exception as e:
        print("skip", u, e); return
    name = re.sub(r"[^0-9A-Za-z]", "_", u)
    if len(name) > 150: name = name[:120] + "_" + str(len(name))
    open(out + "/sheets/" + name, "wb").write(b)
    for m in re.finditer(r'@import\s+(?:url\()?["\']?([^"\')\s;]+)', b.decode("utf-8", "replace")):
        sheet(urllib.parse.urljoin(u, m.group(1)), depth + 1)
for m in re.finditer(r"<link\b[^>]*>", html, re.I):
    t = m.group(0)
    if re.search(r'rel=["\']?[^"\'>]*stylesheet', t, re.I) and not re.search(r'alternate', t, re.I):
        h = re.search(r'href=["\']?([^"\'\s>]+)', t, re.I)
        if h: sheet(urllib.parse.urljoin(final, h.group(1).replace("&amp;", "&")))
print(final, len(body), "bytes,", len(seen), "sheets")
