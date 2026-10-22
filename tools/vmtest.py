#!/usr/bin/env python3
"""Drive VibeOS in a headless QEMU.

  vmtest.py [--timeout S] [--keep] [--hostfwd tcp::18080-:80] [--pcap] [--abs] STEP...

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
  moveto:X,Y       move the pointer (absolute pixels)   click / rclick / dclick / down / up
  drag:X0,Y0,X1,Y1 left-drag
  smoke            = wait for the desktop and take build/smoke.png
The VM window opens on the desktop unless --headless (or VIBE_HEADLESS=1).
Sound: an AC'97 card feeds -audiodev AUDIODEV, default wav,id=snd0,path=build/audio.wav (finalised when
the VM quits; analyse it with tools/wavcheck.py). Override with --audiodev SPEC or VIBE_AUDIODEV, e.g.
`pa,id=snd0` to hear it (the id must stay snd0).
Serial output goes to build/serial.log (--pcap: packets to build/net.pcap); exits non-zero on timeout or panic.
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")
KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
        "]": "bracket_right", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
        "\\": "backslash", ",": "comma", ".": "dot", "/": "slash", "*": "asterisk"}
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

class VM:
    def __init__(self, headless=False, extra=(), audiodev=None, hostfwd=None, pcap=False, absolute=False):
        os.makedirs(B, exist_ok=True)
        self.sock = os.path.join(B, "mon.sock")
        self.log = os.path.join(B, "serial.log")
        self.qsock, self.q = os.path.join(B, "qmp.sock"), None
        for p in (self.sock, self.qsock, self.log):
            if os.path.exists(p): os.unlink(p)
        self.p = subprocess.Popen([
            "qemu-system-i386", "-m", "256", "-drive", f"file={B}/vibeos.img,format=raw,if=ide",
            "-display", "none" if headless else "gtk,zoom-to-fit=on", "-name", "VibeOS (vmtest)",
            "-serial", f"file:{self.log}", "-no-reboot", "-netdev", "user,id=n0" + (f",hostfwd={hostfwd}" if hostfwd else ""), "-device", "e1000,netdev=n0",
            *(["-object", f"filter-dump,id=dump,netdev=n0,file={B}/net.pcap"] if pcap else []),
            "-device", "isa-debug-exit,iobase=0xf4,iosize=4", "-machine", "pc" if absolute else "pc,vmport=off",  # --abs: the vmmouse pointer `make run` gets; else relative PS/2
            "-accel", "kvm", "-accel", "tcg",  # KVM when available
            "-monitor", f"unix:{self.sock},server,nowait", "-qmp", f"unix:{self.qsock},server,nowait",
            "-audiodev", audiodev or f"wav,id=snd0,path={B}/audio.wav,out.frequency=48000", "-device", "AC97,audiodev=snd0", *extra])
        for _ in range(100):
            if os.path.exists(self.sock): break
            time.sleep(0.05)
        self.m = socket.socket(socket.AF_UNIX)
        self.m.connect(self.sock)
        self.m.settimeout(0.2)
        self.drain()
        self.absolute = absolute

    def drain(self):
        try:
            while self.m.recv(65536): pass
        except (socket.timeout, BlockingIOError): pass

    def hmp(self, cmd):
        self.m.sendall((cmd + "\n").encode())
        time.sleep(0.03)
        self.drain()

    def rel(self, dx, dy):
        # QEMU only flushes leftover PS/2 motion on the next event: keep steps <= 127
        while dx or dy:
            sx, sy = max(-127, min(127, dx)), max(-127, min(127, dy))
            self.hmp(f"mouse_move {sx} {sy}")
            dx -= sx; dy -= sy

    def moveto(self, x, y, home=True):
        if self.absolute:                         # QMP absolute axes span 0..32767 over the 1280x800 screen
            self.qmp_events([{"type": "abs", "data": {"axis": "x", "value": x * 32767 // 1279}},
                             {"type": "abs", "data": {"axis": "y", "value": y * 32767 // 799}}])
            time.sleep(0.04)
            return
        if home:                                  # slam into the top-left corner first
            self.rel(-1400, -900)
            self.pos = (0, 0)
        px, py = getattr(self, "pos", (0, 0))
        self.rel(x - px, y - py)
        self.pos = (x, y)
        for _ in range(6):                        # zero moves make QEMU flush parked motion
            time.sleep(0.03)
            self.hmp("mouse_move 0 0")

    def qmp_events(self, evs):
        if not self.q:
            for _ in range(100):
                if os.path.exists(self.qsock): break
                time.sleep(0.05)
            self.q = socket.socket(socket.AF_UNIX); self.q.connect(self.qsock); self.q.settimeout(0.3)
            self.q.recv(65536); self.q.sendall(b'{"execute":"qmp_capabilities"}\n'); self.q.recv(65536)
        self.q.sendall((json.dumps({"execute": "input-send-event", "arguments": {"events": evs}}) + "\n").encode())
        try: self.q.recv(65536)
        except socket.timeout: pass

    def keyev(self, name, down):
        """press/release one key and leave it that way (QMP input-send-event; HMP sendkey can't hold)"""
        self.qmp_events([{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}])
        time.sleep(0.08)

    def serial(self):
        try: return open(self.log, errors="replace").read()
        except FileNotFoundError: return ""

    def wait(self, text, timeout):
        t = time.time() + timeout
        while time.time() < t:
            s = self.serial()
            if text in s: return True
            if "KERNEL PANIC" in s: return False
            if self.p.poll() is not None: return False
            time.sleep(0.1)
        return False

    def type(self, text):
        for ch in text:
            self.hmp("sendkey " + keyname(ch))
            time.sleep(0.02)

    def shot(self, path):
        ppm = path + ".ppm"
        self.hmp(f"screendump {ppm}")
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0: break
            time.sleep(0.05)
        time.sleep(0.2)
        Image.open(ppm).save(path)
        os.unlink(ppm)

    def close(self):
        try: self.hmp("quit")
        except OSError: pass
        try: self.p.wait(3)
        except subprocess.TimeoutExpired: self.p.kill()

def main(argv):
    timeout, keep, steps = 30, False, []
    headless = os.environ.get("VIBE_HEADLESS") == "1"
    audiodev = os.environ.get("VIBE_AUDIODEV")
    hostfwd, pcap, absolute = None, False, False
    it = iter(argv)
    for a in it:
        if a == "--timeout": timeout = float(next(it))
        elif a == "--keep": keep = True
        elif a == "--headless": headless = True
        elif a == "--audiodev": audiodev = next(it)
        elif a == "--hostfwd": hostfwd = next(it)
        elif a == "--pcap": pcap = True
        elif a == "--abs": absolute = True
        elif a == "smoke": steps += ["wait:[desktop] ready", f"shot:{B}/smoke.png"]
        else: steps.append(a)
    vm = VM(headless, audiodev=audiodev, hostfwd=hostfwd, pcap=pcap, absolute=absolute)
    ok = True
    try:
        for s in steps:
            kind, _, arg = s.partition(":")
            arg = arg.encode().decode("unicode_escape")
            if kind == "wait":
                if not vm.wait(arg, timeout):
                    print(f"FAIL: timed out waiting for {arg!r}"); ok = False; break
            elif kind == "type": vm.type(arg)
            elif kind == "host":
                r = subprocess.run(arg, shell=True, capture_output=True, text=True, timeout=timeout)
                print(f"$ {arg}\n{r.stdout}{r.stderr}", end="")
                if r.returncode: print(f"FAIL: exit {r.returncode}"); ok = False; break
            elif kind == "key": vm.hmp("sendkey " + {"prtsc": "print"}.get(arg, arg))
            elif kind in ("keydown", "keyup"): vm.keyev(arg, kind == "keydown")
            elif kind == "sleep": time.sleep(float(arg))
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
                    vm.m.sendall(b"mouse_button 1\n"); time.sleep(0.04)
                    vm.m.sendall(b"mouse_button 0\n"); time.sleep(0.04)
                vm.drain()
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
