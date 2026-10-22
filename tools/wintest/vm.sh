#!/bin/sh
# Visible-VM regression for the compositor's pointer bookkeeping (--abs: real absolute pointer events).
# The pointer rests inside a client, that window closes from its title button (the pointer is then in the frame),
# and the next motion into another client used to send EV_MOUSE_LEAVE to the freed window: a kernel page fault
# at EIP d0294c24 (2026-10-23, found double-clicking a track in Music). Then rapid track switching in Music.
# Coordinates assume the default 1280x800 mode. Run after `make`:  sh tools/wintest/vm.sh
cd "$(dirname "$0")/../.."
O=build/wintest
mkdir -p $O
vt() { python3 tools/vmtest.py --abs --timeout 60 "wait:[desktop] ready" sleep:2 "$@" > $O/last.log 2>&1 || { echo "FAILED: $*"; tail -20 $O/last.log; exit 1; }; }

T='moveto:530,313 dclick sleep:0.4 moveto:530,343 dclick sleep:0.4 moveto:530,373 dclick sleep:0.4'
vt "type:code /home/projects &\n" sleep:4 moveto:600,300 sleep:0.5 moveto:110,39 click sleep:2 "type:music &\n" sleep:3 \
   moveto:530,313 dclick sleep:2 shot:$O/01-after-close.png $T $T $T key:n key:n key:p key:n sleep:2 shot:$O/02-switched.png
grep -q "KERNEL PANIC" build/serial.log && { echo "FAILED: kernel panic"; exit 1; }
echo "ok, screenshots in $O"
