# VibeOS

A vibe-coded operating system for 32-bit x86, written from scratch: its own bootloader,
kernel, compositing window manager, ext2 driver, TCP/IP stack, libc, shell, terminal
emulator, and a set of desktop apps and games.

![VibeOS desktop](docs/screenshots/desktop.png)

| | |
|---|---|
| ![Files and Notepad](docs/screenshots/apps.png) | ![Paint](docs/screenshots/paint.png) |
| ![Games](docs/screenshots/games.png) | ![Editor in the terminal](docs/screenshots/edit.png) |

## Features

**Kernel**
- Two-stage BIOS bootloader: A20, E820 memory map, VBE 32-bpp linear framebuffer, unreal-mode
  kernel load to 1 MiB. A boot splash with a progress bar crossfades into the desktop.
- Paging with a separate address space per process, physical frame bitmap, kernel heap,
  demand-grown user stacks.
- Preemptive scheduling of user programs, ring 3, ELF loading, `spawn`/`waitpid`/signals,
  per-task FPU state, 45 syscalls (`common/abi.h`).
- VFS with mounts: read/write **ext2** root filesystem on ATA, `/dev` (null zero random tty
  kmsg), `/proc` (meminfo uptime cpuinfo version processes), pipes, `poll()`.
- TTY line discipline (canonical and raw modes, job control signals, window size).
- **Networking**: PCI, Intel e1000 driver, Ethernet/ARP/IPv4/ICMP/UDP, DHCP, DNS, and a TCP
  client, exposed as BSD-style socket fds.
- PS/2 keyboard and mouse with wheel, plus the VMware/QEMU absolute pointer; CMOS clock;
  PC speaker.

**Desktop**
- In-kernel compositor with damage tracking, soft shadows, rounded anti-aliased windows,
  window open animations, a procedural aurora wallpaper (5 themes).
- Drag, resize, maximise, minimise, edge snapping and Super+arrow tiling, smart window
  placement, Alt+Tab, a taskbar with launcher, CPU meter and clock, a desktop context menu.
- An xterm-compatible terminal emulator (256 colours and truecolor, alternate screen, scroll
  regions, scrollback, UTF-8).
- Fonts (JetBrains Mono, Noto Sans) rasterised into the build with Latin-1, arrows, symbols
  and box/block glyphs.
- Apps are ordinary processes: a window is a file descriptor whose buffer is mapped into the
  process, and `read()` returns input events. `libgui` + `ui.h` is a small immediate-mode
  widget toolkit.

**Userland**
- A libc: stdio, printf/scanf, malloc, string, math, time, termios, dirents, sockets.
- `/bin/sh`: line editing, persistent history, Tab completion, pipes, redirections,
  `&& || ; &`, globbing, quoting, variables and scripts.
- Coreutils: `ls cat echo mkdir rm rmdir touch cp mv pwd wc head tail grep hexdump ps kill
  uptime date free clear sleep env seq sort uniq tee find which stat dmesg uname cal fortune
  vibefetch` and more.
- `/bin/lua`: Lua 5.4 with a REPL, plus `vibe` (system info, processes, beep) and `gui` (windows) modules.
- Terminal apps: `edit` (a nano-like editor with C syntax highlighting), `less`, `top`.
- Network tools: `ifconfig ping nslookup wget nc`.
- GUI apps: Files, Notepad, Paint, Calculator, Clock (with stopwatch and timer), System
  Monitor, About.
- Games: Snake, Tetris, Minesweeper.

## Building and running

You need `gcc` with 32-bit support, `nasm`, `e2fsprogs` (`mke2fs`), `python3` with Pillow,
and `qemu-system-i386`.

```sh
make            # build build/vibeos.img
make run        # boot it in QEMU (user-mode networking, PC speaker to your sound server)
make clean
```

Try `vibefetch`, `top`, `edit welcome.txt`, `ping 10.0.2.2` or `wget http://example.com/` in the
terminal; press Super for the launcher; right-click the desktop to change the wallpaper.

### Testing

`tools/vmtest.py` boots the image in QEMU and drives it from a script: type text, press keys,
move and click the mouse, wait for serial output, take screenshots. For example:

```sh
python3 tools/vmtest.py smoke 'type:ls -l /bin\n' sleep:1 shot:build/ls.png
```

`/etc/autostart` (in `root/etc/`, not committed) picks what the desktop opens at boot:
`shell`, `term ARGV…` or `run ARGV…`. There are host-side tests for the ext2 driver
(`tools/ext2test`, checked against `e2fsck`), the terminal emulator (`tools/termtest`) and
libc (`tools/libctest`).

## Layout

```
boot/        stage1 (MBR) and stage2 bootloader
kernel/      arch (GDT/IDT/ISRs), core (scheduler, processes, syscalls, tty), mm, fs, dev, net, gui
common/      code shared by kernel and userland: ABI, graphics, fonts, terminal emulator
user/lib     libc, libgui, the ui.h toolkit
user/bin     every program in /bin (NAME.c or NAME/*.c)
root/        files copied into the root filesystem
tools/       font baker, QEMU test driver, host-side tests
docs/        architecture notes and screenshots
```

How it fits together is described in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), and what
was built when is in [CHANGELOG.md](CHANGELOG.md).

## Disclaimer

VibeOS is a hobby project for fun and learning, not for real-world use. Expect bugs and
wild design choices.
