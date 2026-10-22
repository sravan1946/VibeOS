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

# ---- sockets and select over loopback (also runs on upstream's unix port) ----
import socket, select

def oserr(fn):
    try:
        fn()
    except OSError as e:
        return e.args[0]
    return None

PORT = 18999
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(2)
srv.settimeout(3)
eq("accept times out", oserr(lambda: (srv.settimeout(0.2), srv.accept())), errno.ETIMEDOUT)
srv.setblocking(False)
eq("accept non-blocking", oserr(srv.accept), errno.EAGAIN)
srv.settimeout(3)
cli = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
cli.connect(("127.0.0.1", PORT))
conn, peer = srv.accept()
eq("accept peer", peer[0], "127.0.0.1")
eq("send", cli.send(b"hello"), 5)
conn.settimeout(3)
eq("recv", conn.recv(100), b"hello")
conn.sendall(b"line one\nline two\n")
f = cli.makefile("rb")
eq("readline", (f.readline(), f.readline()), (b"line one\n", b"line two\n"))
big = bytes(range(256)) * 78                        # 19968 bytes: fits the send + receive buffers
cli.sendall(big)
got = b""
while len(got) < len(big):
    got += conn.recv(4096)
eq("sendall 20 KB", got, big)
cli.settimeout(0.2)
eq("recv times out", oserr(lambda: cli.recv(1)), errno.ETIMEDOUT)
cli.setblocking(False)
eq("recv non-blocking", oserr(lambda: cli.recv(1)), errno.EAGAIN)
cli.settimeout(None)
eq("gettimeout", (cli.gettimeout(), conn.gettimeout()), (None, 3.0))
rl, wl, _ = select.select([cli, conn], [conn], [], 0)
eq("select idle", (len(rl), len(wl)), (0, 1))
conn.send(b"x")
rl, _, _ = select.select([cli], [], [], 2)
eq("select readable", rl, [cli])
po = select.poll()
po.register(cli, select.POLLIN)
po.register(conn, select.POLLIN | select.POLLOUT)
ev = dict((id(o), e) for o, e in po.poll(1000))
eq("poll", (ev[id(cli)] & select.POLLIN, ev[id(conn)] & select.POLLOUT), (select.POLLIN, select.POLLOUT))
po.unregister(conn)
eq("poll after unregister", [o is cli for o, e in po.poll(0)], [True])
eq("recv after select", cli.recv(5), b"x")
conn.close()
cli.settimeout(3)
eq("recv at EOF", cli.recv(5), b"")
cli.close()
srv.close()
raises("closed socket", OSError, lambda: cli.send(b"x"))
eq("connection refused", oserr(lambda: socket.socket().connect(("127.0.0.1", 18998))), errno.ECONNREFUSED)

u1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u1.bind(("0.0.0.0", PORT))
u1.settimeout(3)
u2.sendto(b"ping", ("127.0.0.1", PORT))
data, frm = u1.recvfrom(100)
eq("udp recvfrom", (data, frm[0]), (b"ping", "127.0.0.1"))
u1.sendto(b"pong", frm)
u2.settimeout(3)
eq("udp reply", u2.recvfrom(100)[0], b"pong")
u1.close()
u2.close()

# ssl: a peer that hangs up during the handshake is an OSError, not a hang
import ssl
srv = socket.socket(); srv.bind(("127.0.0.1", PORT)); srv.listen(1)
cli = socket.socket(); cli.connect(("127.0.0.1", PORT))
srv.accept()[0].close()
raises("ssl handshake fails", OSError, lambda: ssl.wrap_socket(cli, server_hostname="localhost"))
cli.close()
srv.close()

if sys.platform == "vibeos":                        # the port's own socket extras, ssl policy and the gui module
    eq("getaddrinfo", socket.getaddrinfo("127.0.0.1", 80)[0][-1], ("127.0.0.1", 80))
    eq("gethostbyname", socket.gethostbyname("localhost"), "127.0.0.1")
    eq("inet_aton/ntoa", socket.inet_ntoa(socket.inet_aton("10.1.2.3")), "10.1.2.3")
    raises("ssl needs a host name", ValueError, lambda: ssl.wrap_socket(socket.socket()))
    raises("ssl context can't skip checks", ValueError, lambda: (lambda c: (setattr(c, "verify_mode", ssl.CERT_NONE), c.wrap_socket(None))[1])(ssl.create_default_context()))

    # ---- os.path ----
    P = os.path
    eq("join", (P.join("/a", "b", "c"), P.join("a", "/b"), P.join("a/", "b"), P.join("", "b")), ("/a/b/c", "/b", "a/b", "b"))
    eq("split", (P.split("/a/b/c.txt"), P.split("/x"), P.split("x"), P.split("a//b//")), (("/a/b", "c.txt"), ("/", "x"), ("", "x"), ("a//b", "")))
    eq("basename/dirname", (P.basename("/a/b.c"), P.dirname("/a/b.c"), P.dirname("b"), P.basename("/a/")), ("b.c", "/a", "", ""))
    eq("splitext", (P.splitext("a/b.tar.gz"), P.splitext(".bashrc"), P.splitext("a.b/c"), P.splitext("x.")), (("a/b.tar", ".gz"), (".bashrc", ""), ("a.b/c", ""), ("x", ".")))
    eq("normpath", (P.normpath("/a/./b//c/.."), P.normpath("../x/../.."), P.normpath(""), P.normpath("//a")), ("/a/b", "../..", ".", "//a"))
    os.chdir("/bin")
    eq("abspath", (P.abspath("python"), P.abspath("../etc/./x"), P.abspath("/a/..")), ("/bin/python", "/etc/x", "/"))
    os.chdir(cwd)
    eq("isabs", (P.isabs("/x"), P.isabs("x")), (True, False))
    eq("exists", (P.exists("/bin"), P.exists("/bin/python"), P.exists("/no/such")), (True, True, False))
    eq("isdir/isfile", (P.isdir("/bin"), P.isdir("/bin/python"), P.isfile("/bin/python"), P.isfile("/bin"), P.isfile("/no/such")), (True, False, True, False, False))
    eq("getsize", P.getsize("/etc/tz"), os.stat("/etc/tz")[6])
    eq("expanduser", P.expanduser("~/x"), os.getenv("HOME", "/home").rstrip("/") + "/x")
    eq("os.path via import", __import__("os.path").path, P)
    eq("os still has its builtins", (os.getcwd() == cwd, os.sep, hasattr(os, "listdir")), (True, "/", True))

    # ---- gui ----
    import gui
    eq("gui.rgb", (gui.rgb(1, 2, 3), gui.rgb(255, 0, 0)), (0x010203, 0xff0000))
    check("gui.text_size", gui.text_size("abc")[0] > 0 and gui.text_size("abc", "big")[1] > gui.text_size("abc")[1], gui.text_size("abc"))
    with gui.Window("pytest", 200, 120, resizable=False) as w:
        eq("window size", (w.width, w.height), (200, 120))
        w.fill(0x102030)
        eq("fill/pixel", (w.pixel(5, 5), w.pixel(199, 119), w.pixel(200, 5), w.pixel(-1, 0)), (0x102030, 0x102030, 0, 0))
        w.fill(10, 20, 30, 40, (255, 0, 0))
        eq("fill rect", (w.pixel(10, 20), w.pixel(39, 59), w.pixel(40, 60), w.pixel(9, 20)), (0xff0000, 0xff0000, 0x102030, 0x102030))
        w.rect(100, 10, 20, 20, 0x00ff00, fill=False)
        eq("rect outline", (w.pixel(100, 10), w.pixel(119, 29), w.pixel(110, 20)), (0x00ff00, 0x00ff00, 0x102030))
        w.rect(100, 40, 10, 10, 0x0000ff)
        eq("rect fill", w.pixel(105, 45), 0x0000ff)
        w.line(0, 100, 50, 100, 0xffffff)
        eq("line", (w.pixel(0, 100), w.pixel(25, 100), w.pixel(25, 101)), (0xffffff, 0xffffff, 0x102030))
        w.line(60, 80, 100, 80, 0xffff00, width=5)
        eq("wide line", (w.pixel(80, 80), w.pixel(80, 78), w.pixel(80, 90)), (0xffff00, 0xffff00, 0x102030))
        w.circle(150, 90, 10, 0xff00ff)
        eq("circle", (w.pixel(150, 90), w.pixel(150, 82), w.pixel(150, 105)), (0xff00ff, 0xff00ff, 0x102030))
        w.pixel(1, 1, 0x123456)
        eq("pixel set", w.pixel(1, 1), 0x123456)
        adv = w.text(4, 4, "Hi", 0xffffff)
        check("text", adv > 0 and any(w.pixel(x, y) != 0x102030 for x in range(4, 4 + adv) for y in range(4, 20)), adv)
        eq("text advance", adv, gui.text_size("Hi")[0])
        raises("bad font", ValueError, lambda: w.text(0, 0, "x", 0xffffff, font="nope"))
        px = bytes([0x11, 0x22, 0x33, 0xff]) * 4         # little-endian 0xAARRGGBB
        w.blit(150, 10, px, 2, 2)
        eq("blit", (w.pixel(150, 10), w.pixel(151, 11), w.pixel(152, 10)), (0x332211, 0x332211, 0x102030))
        w.blit(160, 10, bytes([0, 0, 255, 128]) * 4, 2, 2, alpha=True)
        check("blit alpha", 0x7c <= w.pixel(160, 10) >> 16 <= 0x94, hex(w.pixel(160, 10)))
        raises("blit too small", ValueError, lambda: w.blit(0, 0, b"\0" * 8, 2, 2))
        raises("bad colour", ValueError, lambda: w.fill((1, 2)))
        w.present()
        w.events(100)                                    # the window manager's focus event
        eq("no events", (w.poll(), w.events()), (None, []))
        w.set_title("pytest 2")
        check("fileno", w.fileno() > 2, w.fileno())
    check("closed", w.closed)
    raises("closed window", ValueError, lambda: w.fill(0))
    w = gui.Window("pytest resize", 240, 160)
    w.resize(300, 200)
    ev = None
    for _ in range(20):
        r, _, _ = select.select([w], [], [], 0.5)       # a window is selectable through fileno()
        if not r:
            break
        for e in w.events():
            if e.type == "resize":
                ev = e
        if ev:
            break
    check("resize event", ev is not None and ev.type == "resize" and (ev.w, ev.h) == (300, 200) and (w.width, w.height) == (300, 200), ev)
    check("event fields", ev is None or (ev.x >= 0 and ev.key == 0 and ev.ch == "" and ev.pressed is False and len(ev) == 11), ev)
    w.close()

# ---- host network checks: `python pytest.py net`, driven by tools/pynettest/run.sh (servers on the host, QEMU user net) ----
if sys.argv[1:2] == ["net"]:
    HOST = "10.0.2.2"
    rep = socket.socket()
    rep.connect((HOST, 18702))                       # the harness: gets "listening" and the final result

    s = socket.socket()
    s.connect((HOST, 18700))
    eq("host getaddrinfo", socket.getaddrinfo(HOST, 18700)[0][-1], (HOST, 18700))
    s.sendall(b"hello host\n")
    eq("echo from the host", s.recv(100), b"hello host\n")
    data = bytes(range(256)) * 400                   # 100 KB, sent and read back in steps
    back = b""
    for i in range(0, len(data), 4096):
        s.sendall(data[i:i + 4096])
        want = len(data[i:i + 4096])
        while want:
            b = s.recv(want)
            back += b
            want -= len(b)
    eq("100 KB echo", back == data, True)
    s.close()

    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(5)
    u.sendto(b"udp host", (HOST, 18701))
    eq("udp echo from the host", u.recvfrom(100)[0], b"udp host")
    u.close()

    def https(port, path="/"):
        c = socket.socket()
        c.connect((HOST, port))
        t = ssl.wrap_socket(c, server_hostname=HOST)
        t.write(b"GET " + path.encode() + b" HTTP/1.0\r\nHost: x\r\n\r\n")
        out = b""
        while True:
            b = t.read(4096)
            if not b:
                break
            out += b
        t.close()
        return out
    r = https(18443, "/blob")
    head, _, body = r.partition(b"\r\n\r\n")
    eq("https status", head[:12], b"HTTP/1.0 200")
    eq("https body", len(body), 300000)
    rep.send(("blobsum %d\n" % (sum(body) & 0xffffff)).encode())
    for port, want in ((18444, "not trusted"), (18445, "different host")):
        try:
            https(port)
            check("https rejects " + want, False, "connected")
        except OSError as e:
            check("https rejects " + want, want in str(e.args[0]), e.args)

    srv = socket.socket()
    srv.bind(("0.0.0.0", 8081))
    srv.listen(1)
    srv.settimeout(40)
    rep.send(b"listening\n")
    c, _ = srv.accept()
    req = c.recv(100)
    c.sendall(req.upper())
    c.close()
    srv.close()
    eq("served the host's client", req, b"ping from host")
    rep.send(("RESULT %d %d\n" % (passed, failed)).encode())
    rep.close()

big = [bytearray(1024) for _ in range(20000)]        # 20 MiB: the GC heap has to grow past its first 1 MiB
eq("heap growth", len(big), 20000)
del big
gc.collect()
check("gc", gc.mem_free() > 0)

print("\n%d passed, %d failed" % (passed, failed))
print("ALL PASS" if failed == 0 else "SOME FAILED")
sys.exit(0 if failed == 0 else 1)
