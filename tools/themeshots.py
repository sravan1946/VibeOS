#!/usr/bin/env python3
"""Screenshots of the desktop in each theme, in a visible QEMU window (needs build/vibeos.img).

  themeshots.py OUTDIR [THEME...]     THEME = a name from `theme` (default: all); `-` = leave the boot theme alone

Per theme two boots: A = terminal (colortest), then Calculator, Notepad and a toast, then the launcher; B = Settings >
Appearance. Files: OUTDIR/THEME-{terminal,desktop,launcher,settings}.png. Fails if a boot logs a kernel panic."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmtest

THEMES = ["graphite", "graphite-light", "contrast"]

def boot():
    vm = vmtest.VM(absolute=True)
    assert vm.wait("[desktop] ready", 60), "no desktop"
    time.sleep(1.5)
    return vm

def finish(vm):
    log = vm.serial()
    vm.close()
    assert "KERNEL PANIC" not in log, "kernel panic, see build/serial.log"

def run(out, theme):
    tag = theme if theme != "-" else "base"
    vm = boot()
    try:
        if theme != "-": vm.type(f"theme {theme}\n"); time.sleep(0.8)
        vm.type("colortest\n"); time.sleep(0.8)
        vm.shot(f"{out}/{tag}-terminal.png")
        vm.type("calc & notepad & notify -k good Build 'All 42 tests passed'\n"); time.sleep(2.2)
        vm.shot(f"{out}/{tag}-desktop.png")
        time.sleep(4)
        vm.keyev("meta_l", True); vm.keyev("meta_l", False); time.sleep(0.6)
        vm.shot(f"{out}/{tag}-launcher.png")
    finally:
        finish(vm)
    vm = boot()
    try:
        if theme != "-": vm.type(f"theme {theme}\n"); time.sleep(0.8)
        vm.type("settings &\n"); time.sleep(2.5)
        vm.shot(f"{out}/{tag}-settings.png")
    finally:
        finish(vm)

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for t in sys.argv[2:] or THEMES: run(out, t)
