#!/usr/bin/env python3
"""Drive VibeOS in a headless QEMU.

  vmtest.py [--timeout S] [--keep] [--hostfwd tcp::18080-:80] [--pcap] [--abs] [--autostart LINE]... STEP...

steps:
  wait:TEXT        wait until TEXT shows up in the serial log
  type:TEXT        type TEXT (\\n = Enter, \\t = Tab)
  key:NAME         QEMU sendkey name, e.g. ctrl-c, up, esc (prtsc = print)
  keydown:NAME     press and hold a key (QMP qcode: alt, meta_l, shift, ctrl, tab, 1 ...) until keyup:NAME
  mclick           middle click (rclick right, dclick double)
  sleep:SECS
  host:CMD         run CMD on the host (fails the run on non-zero exit), e.g. curl through --hostfwd
  shot:PATH.png    screenshot
  wheel:DZ         scroll wheel (negative = up)
  moveto:X,Y       move the pointer (absolute pixels, any display mode)   click / rclick / dclick / down / up
  drag:X0,Y0,X1,Y1 left-drag
  smoke            = wait for the desktop and take build/smoke.png
  (shot: waits until the screen has stopped changing, so no sleep is needed between an action and its shot)
  typetest         type a 200-char line (upper/lower, digits, shifted symbols) into the shell and check it arrives intact; prints the time
  boot             = wait for the desktop and print the boot time (host wall since QEMU started; the kernel's own ms from its serial line)
  suites           = run root/home/runtests.sh (every in-VM suite) and check each summary line on serial; nothing is typed:
                     the kernel starts it from the fw_cfg autostart below, so it needs no wait:[desktop] ready either
--autostart LINE   an /etc/autostart line the kernel runs at boot instead of the file (QEMU fw_cfg opt/vibeos/autostart),
                   e.g. --autostart 'term /bin/ls /bin' : starts commands with no typing and no disk edit (no quoting: split on spaces)
The VM window opens on the desktop unless --headless (or VIBE_HEADLESS=1).
Sound: an AC'97 card feeds -audiodev AUDIODEV, default wav,id=snd0,path=build/audio.wav (finalised when
the VM quits; analyse it with tools/wavcheck.py). Override with --audiodev SPEC or VIBE_AUDIODEV, e.g.
`pa,id=snd0` to hear it (the id must stay snd0).
Serial output goes to build/serial.log (--pcap: packets to build/net.pcap); exits non-zero on timeout or panic.
"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")
KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
        "]": "bracket_right", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
        "\\": "backslash", ",": "comma", ".": "dot", "/": "slash", "*": "asterisk"}
BURST, GAP = 8, 0   # type(): key events per QMP command, pause between commands
SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "(": "9",
           ")": "0", "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right",
           ":": "semicolon", '"': "apostrophe", "~": "grave_accent", "|": "backslash",
           "<": "comma", ">": "dot", "?": "slash"}

def keyname(ch):
    if ch.isalpha() and ch.isupper(): return "shift-" + ch.lower()
    if ch.isalnum(): return ch
    if ch in KEYS: return KEYS[ch]
    if ch in SHIFTED: return "shift-" + (KEYS.get(SHIFTED[ch], SHIFTED[ch]))
    raise ValueError(f"can't type {ch!r}")

def typetest(vm, n=200):
    import random
    r = random.Random(7)
    s = re.sub(" +", " ", "".join(r.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -=,./_+:@%^") for _ in range(n)))
    t = time.time(); vm.type(f"echo TT{s}E > /dev/kmsg\n"); dt = time.time() - t
    ok = vm.wait(f"TT{s}E", 5)
    print(f"typetest: {len(s)} chars typed in {dt:.3f} s, {'intact' if ok else 'KEYS DROPPED'}")
    return ok

def key_events(ch):
    k = keyname(ch)
    sh = k.startswith("shift-")
    ev = lambda n, d: {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": n}}}
    k = k[6:] if sh else k
    return ([ev("shift", True)] if sh else []) + [ev(k, True), ev(k, False)] + ([ev("shift", False)] if sh else [])

class VM:
    def __init__(self, headless=False, extra=(), audiodev=None, hostfwd=None, pcap=False, absolute=False, autostart=()):
        os.makedirs(B, exist_ok=True)
        self.qsock = os.path.join(B, "qmp.sock")
        self.log = os.path.join(B, "serial.log")
        for p in (self.qsock, self.log):
            if os.path.exists(p): os.unlink(p)
        self._s, self._lf, self.t0 = "", None, time.time()
        fw = []
        if autostart:                           # the kernel runs these lines instead of /etc/autostart (kernel/dev/fwcfg.c)
            f = os.path.join(B, "autostart.txt")
            open(f, "w").write("\n".join(autostart) + "\n")
            fw = ["-fw_cfg", f"name=opt/vibeos/autostart,file={f}"]
        self.p = subprocess.Popen([
            "qemu-system-i386", "-m", "256", "-drive", f"file={B}/vibeos.img,format=raw,if=ide",
            "-display", "none" if headless else "gtk,zoom-to-fit=on", "-name", "VibeOS (vmtest)",
            "-serial", f"file:{self.log}", "-no-reboot", "-netdev", "user,id=n0" + (f",hostfwd={hostfwd}" if hostfwd else ""), "-device", "e1000,netdev=n0,romfile=",
            *(["-object", f"filter-dump,id=dump,netdev=n0,file={B}/net.pcap"] if pcap else []),
            "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-machine", "pc" if absolute else "pc,vmport=off",  # --abs: the vmmouse pointer `make run` gets; else relative PS/2
            *(["-accel", "tcg"] if os.environ.get("VIBE_ACCEL") == "tcg" else ["-accel", "kvm", "-accel", "tcg"]),  # KVM when available (VIBE_ACCEL=tcg: software emulation, a slow-CPU worst case)
            "-qmp", f"unix:{self.qsock},server,nowait",
            "-audiodev", audiodev or f"wav,id=snd0,path={B}/audio.wav,out.frequency=48000", "-device", "AC97,audiodev=snd0", *fw, *extra])
        self.q = socket.socket(socket.AF_UNIX)
        for _ in range(2000):                   # QEMU listens within ~10 ms
            try: self.q.connect(self.qsock); break
            except OSError: time.sleep(0.005)
        else: raise RuntimeError("no QMP socket")
        self.qf = self.q.makefile("rb")
        self.qf.readline()                      # greeting
        self.qmp("qmp_capabilities")
        self.absolute = absolute

    def qmp(self, cmd, **args):
        """one QMP command; returns when QEMU has answered (events are skipped)"""
        self.q.sendall((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
        while True:
            l = self.qf.readline()
            if not l: raise OSError("QEMU went away")
            r = json.loads(l)
            if "return" in r: return r["return"]
            if "error" in r: raise RuntimeError(f"{cmd}: {r['error']['desc']}")

    def hmp(self, cmd):
        return self.qmp("human-monitor-command", **{"command-line": cmd})

    def rel(self, dx, dy):
        # QEMU only flushes leftover PS/2 motion on the next event: keep steps <= 127
        while dx or dy:
            sx, sy = max(-127, min(127, dx)), max(-127, min(127, dy))
            self.hmp(f"mouse_move {sx} {sy}")
            dx -= sx; dy -= sy

    def moveto(self, x, y, home=True):
        if self.absolute:                         # QMP absolute axes span 0..32767 over the whole screen
            w, h = self.screen()
            self.qmp_events([{"type": "abs", "data": {"axis": "x", "value": x * 32767 // (w - 1)}},
                             {"type": "abs", "data": {"axis": "y", "value": y * 32767 // (h - 1)}}])
            time.sleep(0.04)
            return
        if home:                                  # slam into the top-left corner first
            self.rel(-4200, -2700)
            self.pos = (0, 0)
        px, py = getattr(self, "pos", (0, 0))
        self.rel(x - px, y - py)
        self.pos = (x, y)
        for _ in range(6):                        # zero moves make QEMU flush parked motion
            time.sleep(0.03)
            self.hmp("mouse_move 0 0")

    def screen(self):
        """current mode: the kernel logs `display: WxH` on every switch, else the 1280x800 boot mode"""
        m = re.findall(r"^display: (\d+)x(\d+)\r?$", self.serial(), re.M)
        return (int(m[-1][0]), int(m[-1][1])) if m else (1280, 800)

    def qmp_events(self, evs):
        self.qmp("input-send-event", events=evs)

    def keyev(self, name, down):
        """press/release one key and leave it that way (QMP input-send-event; HMP sendkey can't hold)"""
        self.qmp_events([{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}])
        time.sleep(0.08)

    def serial(self):
        """the whole serial log so far (read incrementally)"""
        if self._lf is None:
            try: self._lf = open(self.log, errors="replace")
            except FileNotFoundError: return ""
        self._s += self._lf.read()
        return self._s

    def wait(self, text, timeout):
        t = time.time() + timeout
        while time.time() < t:
            s = self.serial()
            if text in s: return True
            if "KERNEL PANIC" in s: return False
            if self.p.poll() is not None: return False
            time.sleep(0.005)
        return False

    def type(self, text, burst=None, gap=None):
        """type TEXT as QMP key down/up events, at most `burst` events per command (then `gap` s). QEMU's PS/2 keyboard
        queue holds 16 bytes and silently drops the rest of a bigger burst, so BURST stays under that; the command round trip
        paces the guest (measured: 200 chars in ~0.3 s, no drops; the old per-key sendkey took ~35 s)"""
        burst, gap = burst or BURST, GAP if gap is None else gap
        cur = []
        for c in text:
            ev = key_events(c)
            if cur and len(cur) + len(ev) > burst:
                self.qmp_events(cur); cur = []
                if gap: time.sleep(gap)
            cur += ev
        if cur: self.qmp_events(cur)

    def shot(self, path, settle=1.5):
        """PNG screendump through QMP (~35 ms), repeated every 50 ms until two in a row are identical so the guest has finished
        drawing what the last step did (at most `settle` s: a blinking caret never settles). Replaces fixed sleeps before shots"""
        tmp, prev, end = os.path.abspath(path) + ".tmp", None, time.time() + settle
        while True:
            self.qmp("screendump", filename=tmp, format="png")
            cur = open(tmp, "rb").read()
            if cur == prev or time.time() > end: break
            prev = cur; time.sleep(0.05)
        os.replace(tmp, path)

    def close(self):
        try: self.qmp("quit")
        except (OSError, RuntimeError, ValueError): pass
        try: self.p.wait(3)
        except subprocess.TimeoutExpired: self.p.kill()

def main(argv):
    timeout, keep, steps = 30, False, []
    headless = os.environ.get("VIBE_HEADLESS") == "1"
    audiodev = os.environ.get("VIBE_AUDIODEV")
    hostfwd, pcap, absolute, autostart = None, False, False, []
    it = iter(argv)
    for a in it:
        if a == "--timeout": timeout = float(next(it))
        elif a == "--keep": keep = True
        elif a == "--headless": headless = True
        elif a == "--audiodev": audiodev = next(it)
        elif a == "--hostfwd": hostfwd = next(it)
        elif a == "--pcap": pcap = True
        elif a == "--abs": absolute = True
        elif a == "--autostart": autostart.append(next(it))
        elif a == "smoke": steps += ["wait:[desktop] ready", "sleep:0.5", f"shot:{B}/smoke.png"]   # 0.5: /bin/desktop draws its icons after "ready"
        elif a == "suites": autostart.append("term /bin/sh /home/runtests.sh"); steps += ["wait:ALL-SUITES-DONE", "suites"]
        else: steps.append(a)
    vm = VM(headless, audiodev=audiodev, hostfwd=hostfwd, pcap=pcap, absolute=absolute, autostart=autostart)
    ok = True
    try:
        for s in steps:
            kind, _, arg = s.partition(":")
            arg = arg.encode().decode("unicode_escape")
            if kind == "wait":
                if not vm.wait(arg, timeout):
                    print(f"FAIL: timed out waiting for {arg!r}"); ok = False; break
            elif kind == "type": vm.type(arg)
            elif kind == "typetest": ok = typetest(vm) and ok
            elif kind == "boot":
                if not vm.wait("[desktop] ready", timeout): print("FAIL: no desktop"); ok = False; break
                print(f"boot: {time.time() - vm.t0:.3f} s host wall from QEMU start to [desktop] ready; "
                      + re.search(r"ready \((\d+) ms\)", vm.serial()).group(1) + " ms in the kernel (timer start to ready)")
            elif kind == "host":
                r = subprocess.run(arg, shell=True, capture_output=True, text=True, timeout=timeout)
                print(f"$ {arg}\n{r.stdout}{r.stderr}", end="")
                if r.returncode: print(f"FAIL: exit {r.returncode}"); ok = False; break
            elif kind == "key":                 # chord, held 30 ms
                vm.qmp("send-key", keys=[{"type": "qcode", "data": {"prtsc": "print"}.get(k, k)} for k in arg.split("-")], **{"hold-time": 30})
                time.sleep(0.04)
            elif kind in ("keydown", "keyup"): vm.keyev(arg, kind == "keydown")
            elif kind == "sleep": time.sleep(float(arg))
            elif kind == "suites":              # check the SUITE lines runtests.sh wrote to serial
                want = len(re.findall(r"^r ", open(os.path.join(ROOT, "root/home/runtests.sh")).read(), re.M))
                got = re.findall(r"^(?:@\d+ )?SUITE (.*?)\r?$", vm.serial(), re.M)
                if len(got) != want: print(f"FAIL: {len(got)} SUITE lines on serial, runtests.sh has {want}"); ok = False
                for l in got:
                    bad = re.search(r"([1-9]\d*) failed|FAIL", l) or not re.search(r"\d+ passed|PASS", l)
                    print(("FAIL: " if bad else "ok    ") + l); ok = ok and not bad
            elif kind == "shot": vm.shot(arg); print("screenshot:", arg)
            elif kind in ("mouse", "moveto"):   # absolute pixel target via relative PS/2 moves
                x, y = (int(v) for v in arg.split(","))
                vm.moveto(x, y)
            elif kind == "drag":                # drag:X0,Y0,X1,Y1
                x0, y0, x1, y1 = (int(v) for v in arg.split(","))
                vm.moveto(x0, y0); time.sleep(0.1)
                vm.hmp("mouse_button 1"); time.sleep(0.1)
                for i in range(1, 11):
                    vm.moveto(x0 + (x1 - x0) * i // 10, y0 + (y1 - y0) * i // 10, home=False); time.sleep(0.03)
                vm.hmp("mouse_button 1"); time.sleep(0.05)
                vm.hmp("mouse_button 0")
            elif kind == "dclick":              # two clicks fast enough to count as a double click
                for _ in range(2):
                    vm.hmp("mouse_button 1"); time.sleep(0.04)
                    vm.hmp("mouse_button 0"); time.sleep(0.04)
            elif kind == "wheel" and vm.absolute:   # HMP mouse_move would also warp the absolute pointer to 0,0
                for _ in range(abs(int(arg))):
                    for down in (True, False):
                        vm.qmp_events([{"type": "btn", "data": {"down": down, "button": "wheel-up" if int(arg) < 0 else "wheel-down"}}])
                    time.sleep(0.03)
            elif kind == "wheel":
                vm.hmp(f"mouse_move 0 0 {int(arg)}")
                for _ in range(4): time.sleep(0.03); vm.hmp("mouse_move 0 0")   # flush the parked packet
            elif kind == "click": vm.hmp("mouse_button 1"); time.sleep(0.05); vm.hmp("mouse_button 0")
            elif kind == "down": vm.hmp("mouse_button 1"); time.sleep(0.05)
            elif kind == "up": vm.hmp("mouse_button 0"); time.sleep(0.05)
            elif kind == "rclick": vm.hmp("mouse_button 2"); time.sleep(0.05); vm.hmp("mouse_button 0")
            elif kind == "mclick": vm.hmp("mouse_button 4"); time.sleep(0.05); vm.hmp("mouse_button 0")
            else: print("unknown step", s); ok = False; break
        if keep: input("VM running, Enter to stop...")
    finally:
        vm.close()
    log = vm.serial()
    print("---- serial (tail) ----")
    print("\n".join(log.splitlines()[-40:]))
    if "KERNEL PANIC" in log: ok = False
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main(sys.argv[1:])
