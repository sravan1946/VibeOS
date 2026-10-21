# VibeOS v2 — Changelog & Roadmap

Living checklist of the v2 rewrite. `[x]` = done and verified in QEMU, `[ ]` = planned.
Newest notes at the bottom of each phase.

## Phase 0 — Tooling & foundation
- [x] Headless QEMU test harness (serial log, sendkey, screenshots) — `tools/vmtest.py`
- [x] Two-stage bootloader: LBA disk reads, A20, E820 memory map, VBE 32bpp mode search, kernel loaded at 1 MiB via unreal mode
- [x] MBR partition table pointing at an ext2 root partition
- [x] Kernel entry: BSS clear, boot-info handoff
- [x] Serial (COM1) kernel log
- [x] GDT + TSS, IDT with all 256 vectors, generic ISR/IRQ dispatch, PIC remap
- [x] PIT @ 1000 Hz, uptime
- [x] Panic screen with register dump
- [x] Physical memory manager (bitmap, from E820)
- [x] Paging (4 MiB identity kernel map + 4 KiB user pages, framebuffer mapped)
- [x] Kernel heap (kmalloc/kfree)

## Phase 1 — Processes, storage, userland
- [x] Preemptive round-robin scheduler, kernel threads, sleep/wait queues
- [x] Ring-3 user processes with per-process address spaces
- [x] `int 0x80` syscall interface
- [x] ELF32 loader, `spawn` + `waitpid`
- [x] ATA PIO disk driver
- [x] ext2 filesystem (read + write)
- [x] VFS with /dev (tty, null, zero, random) and /proc
- [x] PS/2 keyboard driver (full keymap, modifiers, arrows, key buffer)
- [x] Terminal emulator with ANSI escapes + TTY line discipline (canonical/raw)
- [x] Pipes and fd redirection
- [x] User libc (string, stdio/printf, scanf, malloc, file I/O, math, time, setjmp)
- [ ] `sh` with history, line editing, tab completion, pipes, redirection, env vars, scripts
- [ ] Coreutils: ls cat echo mkdir rm rmdir touch cp mv pwd wc head tail grep hexdump ps kill uptime date free clear

## Phase 2 — Desktop
- [x] PS/2 mouse driver
- [x] Double-buffered compositor, window manager (drag, focus, z-order, close, minimise)
- [x] Desktop right-click menu; 5 wallpaper themes (Aurora, Ocean, Sunset, Forest, Midnight), saved in /etc/wallpaper
- [x] Window open animation (fade + rise, ease-out, 180 ms)
- [x] Window snapping: drag to left/right edge to tile, top to maximise, with preview; drag a tiled window off to restore
- [x] Desktop: wallpaper, taskbar, launcher menu, clock
- [x] Anti-aliased UI font baked at build time
- [x] Terminal windows (multiple shells)
- [x] Window syscalls + userland GUI library (libgui: gui.h windows/events, ui.h immediate-mode widgets)
- [ ] GUI apps: system monitor, paint, calculator, clock, file manager, text editor
- [x] About VibeOS app (first ui.h app, verified open/close in QEMU)
- [ ] Games

## Phase 3 — Polish
- [x] Boot splash with progress (logo, gradient bar, live kprintf status, crossfade into desktop)
- [x] RTC clock / date
- [x] PC speaker: `beep` syscall, startup chime; `make run` routes it to the host via PulseAudio
- [ ] README with screenshots, architecture docs

## Log
- 2026-10-22: Cloned original VibeOS (~2.7k lines: 1-sector bootloader, 16bpp VESA console, in-RAM FS, IRQ-driven shell). Started v2 branch.
- Phase 0 done: boots via new stage1/stage2 into a 1280x800x32 framebuffer, kernel at 1 MiB, paging + heap + 1 kHz timer; anti-aliased JetBrains Mono / Noto Sans glyphs baked by `tools/mkfont.py`; shared `common/gfx.c` (alpha blending, rounded rects, shadows, AA lines); styled panic screen with register dump + backtrace.
- Phase 1 kernel: xv6-style scheduler (non-preemptive kernel, preemptive user mode, FPU state per task), per-process page directories with demand-grown stacks, `spawn`/`waitpid`/signals, 35 syscalls, VFS with mounts + `/dev` (null zero random tty kmsg) + `/proc` (meminfo uptime cpuinfo version processes), pipes, canonical/raw TTY with ^C/^D/^U, CMOS RTC. ATA PIO + read/write ext2 (agent, host-tested with e2fsck at 1k/2k/4k blocks). ANSI/xterm terminal emulator with 256/truecolor, scroll regions, alt screen, procedural box drawing (agent, host-tested).
- Phase 2 desktop: in-kernel compositor with damage tracking, procedural aurora wallpaper, rounded windows with soft shadows and traffic-light buttons, drag/resize/maximise/minimise, taskbar with launcher, window list, CPU meter and clock (`/etc/tz`), vmmouse absolute pointer (no mouse grab in QEMU), terminal windows. First ring-3 program ran in a terminal window.
