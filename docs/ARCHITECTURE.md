# VibeOS architecture

A 32-bit x86 (i686) kernel with a built-in compositor, a ring-3 userland and its own libc.
Everything is written from scratch; the only build inputs are `gcc -m32`, `nasm`, `mke2fs`
and two font files that are baked into C arrays at build time.

```
 BIOS ─► stage1 (MBR) ─► stage2 ─► kernel @ 1 MiB ─► compositor thread ─► user apps
                         A20, E820,   paging, heap,     windows, taskbar,    /bin/sh, coreutils,
                         VBE 32bpp,   scheduler, VFS,   launcher, terminal   GUI apps, games
                         unreal-mode  ext2, tty, IRQs   emulator
                         kernel load
```

## Disk layout

| LBA        | What                                                    |
|------------|---------------------------------------------------------|
| 0          | stage1: MBR, loads stage2, partition table entry for `/` |
| 1–32       | stage2 (16 KiB)                                         |
| 64–…       | kernel flat binary                                      |
| 8192–…     | ext2 root filesystem (64 MiB, 1 KiB blocks), partition 1 |

`make` builds `build/vibeos.img` by `dd`-ing the pieces together; the ext2 image is made by
`mke2fs -d build/rootfs`, where `build/rootfs` is `root/` plus every program in `user/bin`.

## Boot

* **stage1** (`boot/stage1.asm`) reads stage2 with BIOS LBA extensions and jumps to it.
* **stage2** (`boot/stage2.asm`) enables A20, stores the E820 memory map at `0x6000`, picks a
  32-bpp VBE linear-framebuffer mode (1280×800 preferred, any 1024–1280 wide otherwise), loads the
  kernel through a bounce buffer at `0x10000` to 1 MiB using unreal mode, switches to protected
  mode and jumps to `0x100000` with `EBX` → `struct boot_info` (magic `'VIBE'`, framebuffer,
  memory map) at `0x5000`.
![Boot splash](screenshots/splash.png)

* **kmain** (`kernel/core/main.c`) brings up serial, the framebuffer splash, GDT/TSS, IDT, the
  physical allocator, the kernel heap, paging, the 1 kHz PIT, RTC, scheduler, syscalls, keyboard,
  `/dev` + `/proc`, mounts ext2 on `/`, then starts the compositor and becomes the idle task.

## Memory

* **Physical**: bitmap allocator over the E820 map (`kernel/mm/pmm.c`).
* **Kernel heap**: first-fit with coalescing, 32 MiB when RAM ≥ 200 MiB (`kernel/mm/heap.c`).
* **Paging** (`kernel/mm/vmm.c`): every address space shares an identity map of 0–1 GiB built
  from 4 MiB PSE pages, so the kernel can touch any physical frame directly. User space is
  `0x40000000–0xBFFFFFFF` in 4 KiB pages, one page directory per process. MMIO (the
  framebuffer) is identity-mapped above `0xC0000000`. `PTE_NOFREE` marks frames a process
  maps but doesn't own (window buffers).
* **User stacks** start at 64 KiB below `0xBFFF0000` and grow on demand to 8 MiB from the page
  fault handler.

## Processes and scheduling

* xv6-style: each task has a kernel stack; `context_switch` saves callee-saved registers and
  flips stacks. The kernel itself is non-preemptive; user mode is preempted on a 10 ms quantum,
  checked when an interrupt returns to ring 3. FPU state is saved per task with `fnsave`.
* Blocking is `sleep_on(chan, timeout)` / `wakeup(chan)`; `poll()` sleeps on a global channel
  that every wakeup pokes.
* There's no `fork`: processes are created with `spawn(path, argv, envp, opts)` where `opts`
  carries the child's stdin/stdout/stderr, cwd and flags. ELF32 executables are linked at
  `0x40001000` (`user/user.ld`), and argv/envp land on the stack in the Linux i386 layout.
* Signals are minimal: a pending signal is delivered on the way back to user mode as an exit
  with status `128 + sig`. The TTY sends `SIGINT` to its foreground process on `^C`.

## Syscalls

`int 0x80`, number in `EAX`, arguments in `EBX ECX EDX ESI EDI`, a negative return is
`-errno`. The whole ABI (numbers, structs, flags, errno, key codes, GUI events) lives in one
header shared by the kernel and libc: `common/abi.h`.

## Filesystems

* **VFS** (`kernel/fs/vfs.c`): paths are canonicalised lexically, then resolved from the longest
  matching mount. Vnodes are refcounted and carry a `vnode_ops` table; open files hold an
  offset and flags and are shared between fds via refcounts.
* **ext2** (`kernel/fs/ext2.c`): read/write, 1/2/4 KiB blocks, indirect blocks, create/unlink/
  rename/mkdir/truncate; tested on the host against `e2fsck` (`tools/ext2test`).
* **devfs** at `/dev`: `null zero random urandom tty kmsg`. **procfs** at `/proc`:
  `meminfo uptime cpuinfo version processes`.
* **Pipes**: 8 KiB ring buffers with blocking read/write and EOF/EPIPE.

## TTY and terminal

`kernel/core/tty.c` is the line discipline: canonical mode with echo, `^C ^D ^U` and erase,
raw mode via `TCSETS`, window size via `TIOCGWINSZ`, and a foreground process. A terminal
window (`kernel/gui/termwin.c`) glues a window, a TTY and the terminal emulator in
`common/term.c` (xterm-compatible: 256 colours and truecolor, scroll regions, alternate screen,
procedurally drawn box-drawing glyphs, scrollback). Keys become escape sequences in `tty_key`.

## Graphics and the desktop

* `common/gfx.c` is shared by the kernel and userland: alpha blending, anti-aliased rounded
  rectangles, gradients, soft shadows, AA lines, scaled blits and text.
* Fonts (JetBrains Mono, Noto Sans) are rasterised at build time by `tools/mkfont.py` into
  8-bit alpha tables in `common/fonts.c`.
* The compositor (`kernel/gui/wm.c`) is a kernel thread. It keeps a back buffer and a
  pre-rendered wallpaper (`wallpaper.c`, procedural), tracks one dirty rectangle, and redraws
  only that region: wallpaper, windows back to front with rounded frames and shadows, the
  taskbar, the launcher menu and the cursor. It handles focus, dragging, resizing, edge
  snapping, minimise/maximise, Alt+Tab, and keyboard shortcuts.
* **User windows are file descriptors.** `win_create` maps a pixel buffer into the process at
  `0xA0000000+` (8 MiB slots; buffers are full-screen-sized so resizes never reallocate);
  the app draws into it and calls `win_flip` with a damage rectangle. `read()` on the fd returns
  `struct gui_event`s and `poll()` works on it.
* `user/lib/gui.c` wraps that; `user/lib/ui.c` is an immediate-mode widget toolkit (buttons,
  checkboxes, sliders, text boxes, lists, panels) in the desktop's theme.

## Input

PS/2 keyboard (scan set 1, modifiers, caps lock) and a hybrid mouse driver: PS/2 IntelliMouse
packets for buttons and the wheel, plus the VMware/QEMU `vmmouse` backdoor for absolute
coordinates, so the pointer isn't grabbed in a VM.

## Userland

`user/lib` is a small libc: string/ctype, `printf` family, `scanf`, stdio buffering, `malloc`,
`stdlib`, `math`, `time`, `setjmp`, termios, dirents and the spawn API. Every
`user/bin/NAME.c` or `user/bin/NAME/*.c` becomes `/bin/NAME`.

## Testing

* `tools/vmtest.py` boots the image in QEMU and drives it through the monitor: type text, send
  keys, move/click/drag the mouse, wait for serial output, take screenshots.
* `/etc/autostart` lists what the desktop opens at boot (`shell`, `term ARGV`, `run ARGV`).
* Host-side unit tests: `tools/ext2test` (filesystem vs `e2fsck`), `tools/termtest`
  (terminal emulator), `tools/libctest` (libc).
