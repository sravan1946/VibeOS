#!/usr/bin/env python3
"""root/lib/python/os/path.py against CPython's posixpath on a pile of awkward paths."""
import importlib.util, itertools, os, posixpath, sys
sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("vibepath", f"{ROOT}/root/lib/python/os/path.py")
vp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vp)

parts = ["", "/", "//", "///", "a", "a/", "/a", "a/b", "a//b", "a/./b", "a/../b", "..", "../..", "/..", ".", "./", "a.txt",
         ".hidden", "..x", "a/.b", "a/b.c.d", "x.", "/a/b/c.tar.gz", "~", "~/z", "~u", "a b/c d", "é/ü.é"]
bad = 0
def chk(name, args):
    global bad
    got, want = getattr(vp, name)(*args), getattr(posixpath, name)(*args)
    if got != want:
        bad += 1
        print(f"FAIL {name}{args!r}: {got!r} != {want!r}")

for p in parts:
    for f in ("isabs", "split", "basename", "dirname", "splitext", "normpath"):
        chk(f, (p,))
    os.environ["HOME"] = "/home/t"
    if not p.startswith("~u"):                       # CPython looks up other users, we have none
        chk("expanduser", (p,))
for a, b in itertools.product(parts, repeat=2):
    chk("join", (a, b))
for a, b, c in itertools.product(parts[:12], repeat=3):
    chk("join", (a, b, c))
os.chdir("/usr")
for p in parts:
    chk("abspath", (p,))
for p in ("/", "/usr", "/etc/passwd", "/nope", "", "/usr/bin/../bin"):
    for f in ("exists", "isdir", "isfile"):
        chk(f, (p,))
chk("getsize", ("/etc/passwd",))
assert int(vp.getmtime("/etc/passwd")) == int(posixpath.getmtime("/etc/passwd"))   # whole seconds on VibeOS
print("pathtest:", "FAILED" if bad else "ok", f"({bad} mismatches)" if bad else "")
sys.exit(1 if bad else 0)
