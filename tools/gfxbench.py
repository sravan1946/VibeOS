#!/usr/bin/env python3
"""Compositor benchmark: boots VibeOS (QEMU; --headless or VIBE_HEADLESS=1 for no window, absolute pointer), runs UI scenarios and prints the
GFXSTAT lines the kernel writes to serial. Needs a kernel built with `make KEXTRA=-DGFXSTAT`.

  tools/gfxbench.py [--mode WxH] [--shots DIR] [scenario ...]     (default: all)

Each scenario prints the GFXSTAT summaries logged while it ran:
  frames=N/ms busy=% (compose time / wall time) kcyc/frame [wall win ovl cur pres] px/frame anim=frames that asked for another
--shots DIR saves a screenshot at the end of every scenario (for pixel-identical before/after comparisons).
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmtest import VM

def stats(vm, since):
    return re.findall(r"^GFXSTAT .*$", vm.serial()[since:], re.M)

def ptr(vm, x, y): vm.moveto(x, y)

def cpu(vm):                                 # QEMU's own user+system CPU time in seconds (all threads), from /proc
    f = open(f"/proc/{vm.p.pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")

def drag(vm, x0, y0, x1, y1, steps=40, dt=0.02):
    ptr(vm, x0, y0); time.sleep(0.2)
    vm.hmp("mouse_button 1"); time.sleep(0.1)
    for i in range(1, steps + 1):
        vm.moveto(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps, home=False); time.sleep(dt)
    vm.hmp("mouse_button 0"); time.sleep(0.3)

def term(vm, cmd, wait=0.0):
    vm.type(cmd + "\n"); time.sleep(wait)

def sc_idle(vm, W, H): time.sleep(4)
def sc_drag(vm, W, H):                       # drag the terminal back and forth
    for _ in range(int(os.environ.get('REPS', 2))):
        drag(vm, 400, 32, 800, 300); drag(vm, 800, 300, 400, 32)
def sc_resize(vm, W, H):                     # grow/shrink from the bottom-right corner
    for _ in range(2):
        drag(vm, 912, 600, 1100, 720); drag(vm, 1100, 720, 912, 600)
def sc_maximize(vm, W, H):                   # maximise / restore: full-screen repaints
    for _ in range(4):
        vm.moveto(500, 32); vm.hmp("mouse_button 1"); time.sleep(0.04); vm.hmp("mouse_button 0"); time.sleep(0.04)
        vm.hmp("mouse_button 1"); time.sleep(0.04); vm.hmp("mouse_button 0"); time.sleep(0.8)
def sc_launcher(vm, W, H):
    for _ in range(6):
        vm.keyev("meta_l", True); vm.keyev("meta_l", False); time.sleep(0.6)
        vm.keyev("meta_l", True); vm.keyev("meta_l", False); time.sleep(0.6)
def sc_open_close(vm, W, H):                 # animated open (Files) and close
    for _ in range(3):
        term(vm, "files &", 1.2); vm.keyev("alt", True); vm.keyev("f4", True); vm.keyev("f4", False); vm.keyev("alt", False); time.sleep(1.0)
def sc_scroll(vm, W, H):                     # terminal output scrolling
    term(vm, "seq 1 3000", 1.0); term(vm, "cat /usr/share/doc/* 2>/dev/null | head -2000; seq 1 2000", 3)
def sc_type(vm, W, H):
    vm.type("echo the quick brown fox jumps over the lazy dog " * 3); time.sleep(0.5); vm.type("\n"); time.sleep(0.5)
def sc_wheel(vm, W, H):                      # scrollback with the wheel
    ptr(vm, 500, 300)
    for _ in range(4):
        for _ in range(10): vm.qmp_events([{"type": "btn", "data": {"down": True, "button": "wheel-up"}}, {"type": "btn", "data": {"down": False, "button": "wheel-up"}}]); time.sleep(0.05)
        for _ in range(10): vm.qmp_events([{"type": "btn", "data": {"down": True, "button": "wheel-down"}}, {"type": "btn", "data": {"down": False, "button": "wheel-down"}}]); time.sleep(0.05)
def sc_overlap(vm, W, H):                    # several windows, drag the top one over the others
    for _ in range(2): vm.keyev("ctrl", True); vm.keyev("shift", True); vm.keyev("n", True); vm.keyev("n", False); vm.keyev("shift", False); vm.keyev("ctrl", False); time.sleep(0.5)
    term(vm, "files &", 1.5); term(vm, "settings &", 1.5)
    drag(vm, 500, 32, 700, 150); drag(vm, 700, 150, 300, 80)
    drag(vm, 300, 80, 600, 250); drag(vm, 600, 250, 200, 100)

SCEN = [(k[3:], v) for k, v in list(globals().items()) if k.startswith("sc_")]

def main(argv):
    mode, shots, names = None, None, []
    headless = os.environ.get("VIBE_HEADLESS") == "1"
    it = iter(argv)
    for a in it:
        if a == "--mode": mode = next(it)
        elif a == "--headless": headless = True
        elif a == "--shots": shots = next(it); os.makedirs(shots, exist_ok=True)
        else: names.append(a)
    vm = VM(headless=headless, absolute=True)
    try:
        if not vm.wait("[desktop] ready", 60): print("no desktop"); return 1
        time.sleep(1)
        if mode:
            vm.type("echo %s > /dev/display\n" % mode); time.sleep(3)
        W, H = vm.screen()
        print(f"mode {W}x{H}")
        for name, fn in SCEN:
            if names and name not in names: continue
            at = len(vm.serial())
            t0, c0 = time.time(), cpu(vm)
            fn(vm, W, H)
            c1, t1 = cpu(vm), time.time()
            time.sleep(3.6)
            st = stats(vm, at)
            print(f"== {name}   [qemu host cpu {100 * (c1 - c0) / (t1 - t0):.0f}% over {t1 - t0:.1f}s]"); print("\n".join(st) if st else "  (no stats)")
            if shots: vm.shot(os.path.join(shots, name + ".png"))
    finally:
        vm.close()
    return 0

if __name__ == "__main__": sys.exit(main(sys.argv[1:]))
