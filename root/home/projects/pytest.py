# pytest.py - checks /bin/python (MicroPython) end to end: run `python projects/pytest.py`
import sys, os, time, math, json, re, random, gc, struct, collections, heapq, binascii, errno

passed = failed = 0

def check(name, ok, got=None):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL", name, "" if got is None else repr(got))

def eq(name, got, want):
    check(name, got == want, got)

def raises(name, exc, fn):
    try:
        fn()
    except exc:
        check(name, True)
    except Exception as e:
        check(name, False, e)
    else:
        check(name, False, "no exception")

# ---- language ----
eq("int arith", 7 * 6 - 2 ** 3 // 3, 40)
eq("bigint", 2 ** 100, 1267650600228229401496703205376)
eq("bigint fact", math.factorial(25), 15511210043330985984000000)
eq("divmod neg", divmod(-7, 2), (-4, 1))
eq("float", 0.1 + 0.2, 0.30000000000000004)
eq("float repr", repr(1 / 3), "0.3333333333333333")
eq("float fmt", "%.3f|%e|%g" % (math.pi, 12345.678, 1e-5), "3.142|1.234568e+04|1e-05")
eq("format spec", "{:>8.2f}|{:08b}|{:x}|{:,}".format(2.5, 5, 255, 1234567), "    2.50|00000101|ff|1,234,567")
eq("fstring", f"{6 * 7} {'x' * 3!r} {3.14159:.2f}", "42 'xxx' 3.14")
eq("int parse", int("ff", 16) + int("0b101", 0) + int(" 12 "), 272)
eq("float parse", float("2.5e-3") + float("1E3"), 1000.0025)
eq("str methods", " Hello, World ".strip().lower().replace("world", "vibe").split(", "), ["hello", "vibe"])
eq("str join/center", "-".join(["a", "b"]).center(7), "  a-b  ")
eq("unicode", len("héllo→"), 6)
eq("bytes", b"abc"[1] + len(bytes(3)) + bytearray(b"xy")[0], 98 + 3 + 120)
eq("encode", "é".encode(), b"\xc3\xa9")
eq("slices", list(range(10))[2:8:3], [2, 5])
eq("listcomp", [x * x for x in range(6) if x % 2], [1, 9, 25])
eq("dictcomp", {k: v for k, v in zip("abc", range(3))}, {"a": 0, "b": 1, "c": 2})
eq("setops", sorted({1, 2, 3} & {2, 3, 4} | {9}), [2, 3, 9])
eq("sorted key", sorted(["bb", "a", "ccc"], key=len, reverse=True), ["ccc", "bb", "a"])
eq("unpack", (lambda a, *b, c=1, **k: (a, b, c, k))(1, 2, 3, c=4, d=5), (1, (2, 3), 4, {"d": 5}))
eq("star call", (lambda *a: a)(*range(3), *"ab"), (0, 1, 2, "a", "b"))

def gen(n):
    for i in range(n):
        yield i * 2
    return "done"
eq("generator", list(gen(4)), [0, 2, 4, 6])
eq("genexpr sum", sum(x for x in range(101)), 5050)

def counter():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    return inc
c = counter(); c(); c()
eq("closure", c(), 3)

class Shape:
    sides = 0
    def __init__(self, name):
        self.name = name
    def __repr__(self):
        return "<%s %d>" % (self.name, self.sides)
    def __eq__(self, other):
        return self.sides == other.sides
class Square(Shape):
    sides = 4
    def __init__(self):
        super().__init__("square")
    @property
    def angle(self):
        return 360 // self.sides
eq("class repr", repr(Square()), "<square 4>")
eq("inheritance", (isinstance(Square(), Shape), Square().angle, Square() == Square()), (True, 90, True))

class Ctx:
    log = []
    def __enter__(self):
        self.log.append("in")
        return self
    def __exit__(self, *a):
        self.log.append("out")
        return False
with Ctx():
    pass
eq("with", Ctx.log, ["in", "out"])

def fin():
    try:
        return "try"
    finally:
        Ctx.log.append("finally")
eq("try/finally", (fin(), Ctx.log[-1]), ("try", "finally"))
raises("ZeroDivisionError", ZeroDivisionError, lambda: 1 / 0)
raises("KeyError", KeyError, lambda: {}["x"])
raises("IndexError", IndexError, lambda: [][1])
raises("custom exception", ValueError, lambda: int("nope"))
try:
    raise OSError(errno.ENOENT, "gone")
except OSError as e:
    eq("OSError errno", e.errno, errno.ENOENT)

def deep(n):
    return n if n == 0 else 1 + deep(n - 1)
n = 5000 if sys.platform == "vibeos" else 100        # the host unix port has a small stack limit
eq("recursion", deep(n), n)
raises("RecursionError", RuntimeError, lambda: deep(10 ** 7))

# ---- modules ----
check("sys.platform", sys.platform in ("vibeos", "linux"), sys.platform)
check("sys.argv", sys.argv[0].endswith("pytest.py"), sys.argv)
check("sys.version", sys.version.startswith("3.") and sys.implementation.name == "micropython", sys.version)

eq("math.sqrt", math.sqrt(2), 1.4142135623730951)
check("math trig", abs(math.sin(math.pi / 6) - 0.5) < 1e-15 and abs(math.atan2(1, 1) - math.pi / 4) < 1e-15)
eq("math.gamma", (math.gamma(5), math.gamma(0.5) ** 2 - math.pi < 1e-14), (24.0, True))
check("math.lgamma", abs(math.lgamma(100) - 359.13420536957540) < 1e-11, math.lgamma(100))
check("math.erf", abs(math.erf(1) - 0.8427007929497149) < 1e-15 and abs(math.erfc(3) - 2.209049699858544e-05) < 1e-19)
eq("math.expm1", math.expm1(1e-10), 1.00000000005e-10)
check("math hyperbolic", abs(math.asinh(1) - 0.881373587019543) < 1e-15 and math.acosh(1) == 0 and abs(math.atanh(0.5) - 0.5493061443340549) < 1e-15)
eq("math.copysign", math.copysign(3, -0.0), -3.0)
eq("round half even", (round(2.5), round(3.5), round(-0.5), round(1.2345, 2)), (2, 4, 0, 1.23))
eq("math misc", (math.floor(-1.5), math.ceil(1.2), math.isclose(1, 1 + 1e-12), math.fmod(7, -3)), (-2, 2, True, 1.0))

doc = {"name": "vibe", "n": [1, 2.5, None, True], "nested": {"k": "vé"}}
s = json.dumps(doc)
eq("json roundtrip", json.loads(s), doc)
eq("json parse", json.loads('{"a": [1, {"b": null}], "c": "\\u00e9"}'), {"a": [1, {"b": None}], "c": "é"})
raises("json error", ValueError, lambda: json.loads("{bad"))

m = re.match(r"(\w+)@(\w+)\.com", "vibe@example.com")
eq("re groups", (m.group(1), m.group(2)), ("vibe", "example"))
eq("re sub", re.sub(r"\d+", "#", "a1b22c333"), "a#b#c#")
eq("re split", re.compile(r"[,;] *").split("a, b;c"), ["a", "b", "c"])
eq("re search", re.search(r"o+", "fooo bar").group(0), "ooo")
eq("re compile", [bool(re.compile("^[a-z]+$").match(w)) for w in ("abc", "ab1")], [True, False])

random.seed(42)
a = [random.randint(1, 6) for _ in range(5)]
random.seed(42)
eq("random seed", [random.randint(1, 6) for _ in range(5)], a)
r = random.random()
check("random range", 0 <= r < 1 and random.choice("xyz") in "xyz" and 1 <= random.uniform(1, 2) <= 2)
eq("random getrandbits", 0 <= random.getrandbits(8) < 256 and random.randrange(10, 20, 5) in (10, 15), True)

eq("struct", struct.unpack("<HI", struct.pack("<HI", 513, 0xdeadbeef)), (513, 0xdeadbeef))
eq("struct calcsize", struct.calcsize("<bhiq"), 15)
dq = collections.deque((), 5); dq.append(1); dq.appendleft(0)
eq("deque", list(dq), [0, 1])
Point = collections.namedtuple("Point", ("x", "y"))
eq("namedtuple", Point(1, 2).y, 2)
od = collections.OrderedDict(); od["z"] = 1; od["a"] = 2
eq("OrderedDict", list(od.keys()), ["z", "a"])
h = [5, 1, 4]; heapq.heapify(h); heapq.heappush(h, 0)
eq("heapq", [heapq.heappop(h) for _ in range(4)], [0, 1, 4, 5])
eq("binascii", (binascii.hexlify(b"\x01\xff"), binascii.b2a_base64(b"vibe")), (b"01ff", b"dmliZQ==\n"))
sio = __import__("io").StringIO("a\nb")
eq("io.StringIO", (sio.readline(), sio.read()), ("a\n", "b"))

# ---- files and os ----
d = "/tmp/pytest.dir"
try:
    os.mkdir(d)
except OSError:
    pass
p = d + "/f.txt"
with open(p, "w") as f:
    f.write("line one\nline two\n")
    f.write("%d\n" % 3)
with open(p) as f:
    eq("read lines", [x.rstrip() for x in f], ["line one", "line two", "3"])
with open(p, "a") as f:
    f.write("more")
with open(p, "rb") as f:
    f.seek(5)
    eq("seek/read bytes", f.read(3), b"one")
    eq("tell", f.tell(), 8)
eq("stat size", os.stat(p)[6], 24)
check("listdir", "f.txt" in os.listdir(d), os.listdir(d))
os.rename(p, d + "/g.txt")
eq("rename", os.listdir(d), ["g.txt"])
eq("ilistdir type", [(e[0], e[1] == 0x8000) for e in os.ilistdir(d)], [("g.txt", True)])
os.remove(d + "/g.txt")
os.rmdir(d)
raises("open missing", OSError, lambda: open("/no/such/file"))
cwd = os.getcwd()
os.chdir("/bin")
check("chdir/getcwd", os.getcwd() == "/bin" and "python" in os.listdir(), os.getcwd())
os.chdir(cwd)
eq("getenv", (os.getenv("NOPE_XYZ"), os.getenv("NOPE_XYZ", "dflt")), (None, "dflt"))
os.putenv("PYTEST_VAR", "42")
eq("putenv", os.getenv("PYTEST_VAR"), "42")
eq("system", os.system("true"), 0)
check("urandom", len(os.urandom(16)) == 16)

with open("/tmp/pytest_mod.py", "w") as f:
    f.write("X = 7\ndef twice(v):\n    return 2 * v\n")
sys.path.insert(0, "/tmp")
import pytest_mod
eq("import from file", pytest_mod.twice(pytest_mod.X), 14)
os.remove("/tmp/pytest_mod.py")

# ---- time and memory ----
t0 = time.ticks_ms()
time.sleep(0.1)
dt = time.ticks_diff(time.ticks_ms(), t0)
check("sleep/ticks", 90 <= dt < 1000, dt)
check("time.time", time.time() > 1.7e9, time.time())
lt = time.localtime()
check("localtime", len(lt) >= 8 and 2024 <= lt[0] < 2100 and 1 <= lt[1] <= 12, lt)
eq("gmtime epoch", time.gmtime(0)[:6], (1970, 1, 1, 0, 0, 0))
eq("mktime", time.mktime(time.localtime(1700000000)), 1700000000)

if sys.platform == "vibeos":                        # this port's own additions (the rest also runs on upstream's unix port)
    check("sys.path", "/lib/python" in sys.path, sys.path)
    eq("sys.maxsize", sys.maxsize, 2 ** 31 - 1)
    eq("uname", os.uname().sysname, "vibeos")
    eq("strftime", time.strftime("%Y-%m-%d %H:%M", (2026, 10, 22, 9, 5, 0, 3, 295)), "2026-10-22 09:05")
    import vibe
    si = vibe.sysinfo()
    check("vibe.sysinfo", si["mem_total_kb"] > 0 and si["screen_w"] > 0, si)
    check("vibe.ps", any(p[5] == "python" and p[2] == "running" for p in vibe.ps()), vibe.ps())
    check("vibe.uptime", vibe.uptime() > 0)

big = [bytearray(1024) for _ in range(20000)]        # 20 MiB: the GC heap has to grow past its first 1 MiB
eq("heap growth", len(big), 20000)
del big
gc.collect()
check("gc", gc.mem_free() > 0)

print("\n%d passed, %d failed" % (passed, failed))
print("ALL PASS" if failed == 0 else "SOME FAILED")
sys.exit(0 if failed == 0 else 1)
