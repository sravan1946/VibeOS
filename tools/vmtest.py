#!/usr/bin/env python3
"""Drive VibeOS in a headless QEMU.

  vmtest.py [--timeout S] [--keep] STEP...

steps:
  wait:TEXT        wait until TEXT shows up in the serial log
  type:TEXT        type TEXT (\\n = Enter, \\t = Tab)
  key:NAME         QEMU sendkey name, e.g. ctrl-c, up, esc
  sleep:SECS
  shot:PATH.png    screenshot
  mouse:DX,DY      relative mouse move     click / rclick
  smoke            = wait for the desktop and take build/smoke.png
The VM window opens on the desktop unless --headless (or VIBE_HEADLESS=1).
Serial output goes to build/serial.log; exits non-zero on timeout or panic.
"""
import os, socket, subprocess, sys, time
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
    def __init__(self, headless=False, extra=()):
        os.makedirs(B, exist_ok=True)
        self.sock = os.path.join(B, "mon.sock")
        self.log = os.path.join(B, "serial.log")
        for p in (self.sock, self.log):
            if os.path.exists(p): os.unlink(p)
        self.p = subprocess.Popen([
            "qemu-system-i386", "-m", "256", "-drive", f"file={B}/vibeos.img,format=raw,if=ide",
            "-display", "none" if headless else "gtk,zoom-to-fit=off", "-name", "VibeOS (vmtest)",
            "-serial", f"file:{self.log}", "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=4",
            "-monitor", f"unix:{self.sock},server,nowait", *extra])
        for _ in range(100):
            if os.path.exists(self.sock): break
            time.sleep(0.05)
        self.m = socket.socket(socket.AF_UNIX)
        self.m.connect(self.sock)
        self.m.settimeout(0.2)
        self.drain()

    def drain(self):
        try:
            while self.m.recv(65536): pass
        except (socket.timeout, BlockingIOError): pass

    def hmp(self, cmd):
        self.m.sendall((cmd + "\n").encode())
        time.sleep(0.03)
        self.drain()

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
    it = iter(argv)
    for a in it:
        if a == "--timeout": timeout = float(next(it))
        elif a == "--keep": keep = True
        elif a == "--headless": headless = True
        elif a == "smoke": steps += ["wait:[desktop] ready", f"shot:{B}/smoke.png"]
        else: steps.append(a)
    vm = VM(headless)
    ok = True
    try:
        for s in steps:
            kind, _, arg = s.partition(":")
            arg = arg.encode().decode("unicode_escape")
            if kind == "wait":
                if not vm.wait(arg, timeout):
                    print(f"FAIL: timed out waiting for {arg!r}"); ok = False; break
            elif kind == "type": vm.type(arg)
            elif kind == "key": vm.hmp("sendkey " + arg)
            elif kind == "sleep": time.sleep(float(arg))
            elif kind == "shot": vm.shot(arg); print("screenshot:", arg)
            elif kind == "mouse": dx, dy = arg.split(","); vm.hmp(f"mouse_move {dx} {dy}")
            elif kind == "click": vm.hmp("mouse_button 1"); time.sleep(0.05); vm.hmp("mouse_button 0")
            elif kind == "rclick": vm.hmp("mouse_button 2"); time.sleep(0.05); vm.hmp("mouse_button 0")
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
