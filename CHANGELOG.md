# VibeOS v2 — Changelog & Roadmap

Living checklist of the v2 rewrite. `[x]` = done and verified in QEMU, `[ ]` = planned.
Newest notes at the bottom of each phase.

## Phase 0 — Tooling & foundation
- [ ] Headless QEMU test harness (serial log, sendkey, screenshots) — `tools/vmtest.py`
- [ ] Two-stage bootloader: LBA disk reads, A20, E820 memory map, VBE 32bpp mode search, kernel loaded at 1 MiB via unreal mode
- [ ] MBR partition table pointing at an ext2 root partition
- [ ] Kernel entry: BSS clear, boot-info handoff
- [ ] Serial (COM1) kernel log
- [ ] GDT + TSS, IDT with all 256 vectors, generic ISR/IRQ dispatch, PIC remap
- [ ] PIT @ 1000 Hz, uptime
- [ ] Panic screen with register dump
- [ ] Physical memory manager (bitmap, from E820)
- [ ] Paging (4 MiB identity kernel map + 4 KiB user pages, framebuffer mapped)
- [ ] Kernel heap (kmalloc/kfree)

## Phase 1 — Processes, storage, userland
- [ ] Preemptive round-robin scheduler, kernel threads, sleep/wait queues
- [ ] Ring-3 user processes with per-process address spaces
- [ ] `int 0x80` syscall interface
- [ ] ELF32 loader, `spawn` + `waitpid`
- [ ] ATA PIO disk driver
- [ ] ext2 filesystem (read + write)
- [ ] VFS with /dev (tty, null, zero, random) and /proc
- [ ] PS/2 keyboard driver (full keymap, modifiers, arrows, key buffer)
- [ ] Terminal emulator with ANSI escapes + TTY line discipline (canonical/raw)
- [ ] Pipes and fd redirection
- [ ] User libc (string, stdio/printf, malloc, file I/O)
- [ ] `sh` with history, line editing, tab completion, pipes, redirection, env vars, scripts
- [ ] Coreutils: ls cat echo mkdir rm rmdir touch cp mv pwd wc head tail grep hexdump ps kill uptime date free clear

## Phase 2 — Desktop
- [ ] PS/2 mouse driver
- [ ] Double-buffered compositor, window manager (drag, focus, z-order, close, minimise)
- [ ] Desktop: wallpaper, taskbar, launcher menu, clock
- [ ] Anti-aliased UI font baked at build time
- [ ] Terminal windows (multiple shells)
- [ ] Window syscalls + userland GUI library
- [ ] GUI apps: system monitor, paint, calculator, clock, file manager, text editor
- [ ] Games

## Phase 3 — Polish
- [ ] Boot splash with progress
- [ ] RTC clock / date
- [ ] PC speaker
- [ ] README with screenshots, architecture docs

## Log
- 2026-10-22: Cloned original VibeOS (~2.7k lines: 1-sector bootloader, 16bpp VESA console, in-RAM FS, IRQ-driven shell). Started v2 branch.
