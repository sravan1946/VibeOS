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
* Signals are checked on the way back to ring 3 (`sched_irq_exit`). Each task has a pending and
  a blocked mask and a disposition per signal. A handler runs on the user stack: the kernel
  pushes a frame (the libc restorer as return address, the signal number, the interrupted
  registers, the blocked mask and the `fnsave`d x87 state), and `SYS_sigreturn` from the
  restorer puts it all back. The signal is blocked while its handler runs; blocking syscalls
  fail with `EINTR`. The default action is exit with `128 + sig`, except for SIGCHLD and SIGWINCH,
  which are ignored. The TTY sends `SIGINT` to its foreground process on `^C` and SIGWINCH on
  resize, and a child's exit sends SIGCHLD to its parent. Ignored signals stay ignored across
  `spawn`, as they would across exec. `/bin/sigtest` checks it all.

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
* **devfs** at `/dev`: `null zero random urandom tty kmsg clipboard screen notify` (+ `dsp mixer` with an AC'97 card; `screen` reads as `u32 w, u32 h` then the composed
  screen as ARGB rows, taken from the compositor's back buffer). **procfs** at `/proc`:
  `meminfo uptime cpuinfo version processes`.
* **Pipes**: 8 KiB ring buffers with blocking read/write and EOF/EPIPE.

## Audio

`kernel/dev/ac97.c` drives an Intel ICH AC'97 controller (QEMU `-device AC97`, found by PCI id like the
e1000). PCM-out DMA walks a 32-entry buffer descriptor list (the hardware index wraps at 32) over a 32 KiB
physically contiguous ring (the identity map makes the physical address usable as a pointer). `/dev/dsp` is
**mixed in software**: devfs's `clone` hook makes every lookup of `/dev/dsp` a fresh vnode, i.e. a stream with a
64 KiB queue of its own (allocated on its first write), a gain and a place in a list. `write()` only fills the
stream's queue (blocking while it is full, `sleep_on`/`wakeup` like pipes). The IRQ (interrupt-on-completion per
descriptor) retires finished descriptors and tops the ring up to 4 in flight: each new descriptor takes up to 256
frames (5.3 ms), summing what every stream has queued (per-stream gain in 8.8 fixed point, clamped to 16 bits), so
the DMA lead is 16-21 ms for everybody and a stream's latency is its own queue plus that. With nothing queued
nothing is mixed, the engine halts at the last-valid index and restarts when it moves; opened-but-idle streams are
skipped (a list walk). `/dev/dsp` takes **48 kHz stereo s16le** only (no VRA, no resampling in the kernel: userland
resamples). `close` waits (max 5 s) until the stream's queue is empty and the descriptors carrying its tail have
played, other streams keep going. Ioctls (`common/abi.h`): `AUDIO_GETQ` (bytes between `write` and the speaker:
this stream's queue + the DMA ring), `AUDIO_SETVOL`/`AUDIO_GETVOL` (master codec volume, 0-100),
`AUDIO_SETSVOL`/`AUDIO_GETSVOL` (this stream, default 100), `AUDIO_FLUSH` (drop this stream's queue: pause, seek).
`/dev/mixer` has only the master ioctls. `/dev/dsp` and `/dev/mixer` only appear if a card was found; the PC speaker
(`speaker_tone`) is independent. PCI devices may share an interrupt line, so `irq_register` keeps a small handler
chain per IRQ and each handler checks its own device.

`user/music/adec.c` (+ `adec.h`) is the userland decoder for WAV, MP3 ([minimp3](https://github.com/lieff/minimp3),
`third_party/minimp3`) and Ogg Vorbis ([stb_vorbis](https://github.com/nothings/stb), `third_party/stb_vorbis`),
both unmodified, each in a translation unit of its own (`mp3.c`, `vorbis.c`; the Makefile sets stb_vorbis up by
-D). It yields 48 kHz stereo frames: native-rate frames go through a linear resampler whose phase is kept exactly in
1/48000 units. MP3 and Ogg are read whole; the MP3 frame table is built at open (length, seeking with two frames of
lead-in for the bit reservoir, the Xing/Info frame skipped), ID3v2.3/2.4 give title/artist/album and the APIC
picture. `/bin/play` and `/bin/music` link it; `tools/adectest` checks it against ffmpeg on the host.

`/bin/music` (`user/bin/music/`) keeps one stream open, decodes ~1024 frames at a time to hold ~150 ms queued, and
derives the position from frames written minus `AUDIO_GETQ`; pause and seek `AUDIO_FLUSH` and seek the decoder.

Userland: `tone` and the Piano app generate audio in 10 ms chunks and keep ~30 ms queued, polling
`AUDIO_GETQ` instead of blocking in `write`. `tools/vmtest.py` records the card to `build/audio.wav` (QEMU `wav`
audiodev); `tools/wavcheck.py` reports tones and RMS in it.

## Doom

`/bin/doom` is [doomgeneric](https://github.com/ozkl/doomgeneric) (`third_party/doomgeneric`, unmodified, see its
`README.vibeos`) plus `user/bin/doom/`: `doomgeneric_vibeos.c` (a 640x400 libgui window, copied from Doom's 2x frame; key
events including releases, all held keys released when focus is lost; `DG_SleepMs` waits in `poll()` on the window fd),
`i_vibesound.c` (doomgeneric's sound module: 16 channels mixed in 8 ms chunks into `/dev/dsp`, linear 11025 -> 48 kHz,
~40 ms kept queued with `AUDIO_GETQ`, topped up every tic and while Doom sleeps; music is mixed into the same chunks, see below) and `main.c` (IWAD lookup,
config and saves in `/home/.doom`). Keys: arrows move, Ctrl fire, Space use, Shift run, Alt strafe, `,` `.` strafe, 1-7
weapons, Tab map, Esc menu. The Makefile builds the upstream sources with their own flags (`-w`, `-include abi.h`).

### Music

`user/music/music.c` (+ `music.h`) is the synth and sequencer shared by Doom and `/bin/midi`. A song is a flat, time-sorted
list of MIDI events in 48 kHz frames: `mus_parse_mus` converts a Doom MUS lump (140 Hz ticks, percussion channel 15 -> 9,
MUS controllers -> CCs) and `mus_parse_smf` a Standard MIDI File (tracks merged by tick, tempo map applied; PPQ timing only).
`mus_render(acc, n)` runs the sequencer and the chip together, so tempo follows however fast the caller pulls audio and
nothing needs a timer or a thread. The chip is [Nuked-OPL3](third_party/nuked-opl3/README.vibeos) (LGPL, unmodified,
integer-only) in OPL3 mode: 18 two-operator voices with left/right per voice (MIDI pan CC10). Instruments come from the
`GENMIDI` lump of the WAD (175 patches; double-voice patches take two chip voices; percussion = patches 128..174 for
notes 35..81); velocity, CC7 and CC11 scale the carrier level, pitch bend is +-2 semitones, CC64 sustain. GENMIDI's base note
offsets are ignored (applying them drops every melodic note an octave). Voice stealing prefers released voices, then the oldest.
Doom's `I_SetMusicVolume` (menu slider, 0..127) is `mus_volume`. Native cost is ~3% of one core for 34 s of dense 5-part music.

The game data is not in the repository. `make doom-wad` (`tools/getwad.sh`) downloads the pinned Freedoom 0.13.0 zip, checks
its sha256 and caches `freedoom1.wad` in `build/wads/`; the next `make` copies it to `/usr/share/doom/` in the root filesystem.
Without it `doom` says so (in a window when started from the launcher). `doom -iwad FILE` uses another IWAD.

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
* **Workspaces**: every window has `ws` (0..3, or -1 for frameless toasts, shown everywhere); `shown(w)` =
  visible, not minimised, on the current workspace, and every compose/hit-test/placement/focus loop uses it.
  Switching snapshots the screen, composes the target and cross-fades (`crossfade`). `topmost` windows are kept
  above the rest by `raise()`.
* **Overlays** (drawn after the windows, top to bottom: Alt+Tab, toasts, notification center, quick settings,
  window menu, context menu, launcher) each have their own rectangle and damage only that rectangle when
  they change. Alt+Tab thumbnails are a 2x2-tap downscale of the window buffers, redrawn when a client flips.
* **Notifications**: `/dev/notify` (`title\nbody[\nkind]`) calls `wm_notify`, which pushes onto a 24-entry
  history; entries flagged `toast` are stacked top-right for 4 s (checked on the 250 ms tick). The center lists
  the history. The tray polls `ac97_volume()` and `ni` on the same tick and damages the tray only on change.
* **User windows are file descriptors.** `win_create` maps a pixel buffer into the process at
  `0xA0000000+` (8 MiB slots; buffers are full-screen-sized so resizes never reallocate);
  the app draws into it and calls `win_flip` with a damage rectangle. `read()` on the fd returns
  `struct gui_event`s and `poll()` works on it.
* `user/lib/gui.c` wraps that; `user/lib/ui.c` is an immediate-mode widget toolkit (buttons,
  checkboxes, sliders, text boxes, lists, panels) in the desktop's theme.

## Images and screenshots

`user/lib/image.c` (in libgui, also compiled on the host by `tools/imagetest`) holds a from-scratch zlib
inflate, PNG/BMP/PPM decoders that produce `0xAARRGGBB` (non-premultiplied, like `gfx.h`), a PNG encoder
(LZ77 with one hash slot + fixed Huffman) and `image_sample`, the resampler the viewer uses (area average
down, bilinear up, row copy at 1:1). PrtSc is handled in the compositor, which only spawns `/bin/screenshot`;
that process reads `/dev/screen`, encodes with `image_save_png` and shows a toast window. The kernel never
touches PNG.

## Input

PS/2 keyboard (scan set 1, modifiers, caps lock) and a hybrid mouse driver: PS/2 IntelliMouse
packets for buttons and the wheel, plus the VMware/QEMU `vmmouse` backdoor for absolute
coordinates, so the pointer isn't grabbed in a VM.

## Userland

`user/lib` is a small libc: string/ctype, `printf` family, `scanf`, stdio buffering, `malloc`,
`stdlib`, `math`, `time`, `setjmp`, termios, dirents and the spawn API. Every
`user/bin/NAME.c` or `user/bin/NAME/*.c` becomes `/bin/NAME`. `malloc` is a boundary-tag
allocator with segregated free lists. `/bin/lua` builds the unmodified Lua 5.4 from
`third_party/lua` plus `user/bin/lua/` (REPL line editor, the `vibe` and `gui` modules); see
`third_party/lua/README.vibeos`.
`/bin/tcc` is the unmodified TinyCC 0.9.27 (`third_party/tcc`, config in `user/bin/tcc/config.h`). The
rootfs doubles as its SDK: `/usr/include` (libc + gui headers), `/usr/lib/{crt1.o,libc.a,libgui.a}`,
`/usr/lib/tcc/{libtcc1.a,include}`. tcc output is a static ELF linked at the user base 0x40001000 (set by
`user/bin/tcc/vibe.c`), which `load_elf` takes as is. See `third_party/tcc/README.vibeos`.

`user/lib/regex.c` is the POSIX regex engine behind `grep`, `sed` and `awk` (`regcomp`/`regexec`, BRE and ERE plus the
GNU `\| \+ \? \w \b \< \>` extensions, `REG_STARTEND` so callers can scan a buffer for successive matches without
copying). A pattern is parsed to an AST (counted repeats are unrolled; `x*` is compiled as `(x+)?` so an empty iteration
still sets captures) and then to a small program. Matching without back-references is a Pike VM: all threads advance in
lock step, so it is linear in the input, and it keeps running after the first match to get POSIX leftmost-longest extents
(captures follow thread priority, i.e. greedy-first). Patterns with back-references run on a recursive backtracker that
explores every path for the longest match under a 2M-step budget (exceeding it reports no match). It has its own ASCII
ctype so `tools/libctest/regex_diff.c` can build it against glibc on the host. Bytes only: no locale or UTF-8 awareness.

`awk` (`user/bin/awk.c`) is a tree-walking interpreter: lexer (regex-vs-divide decided by the previous token), recursive
descent parser to an AST with variables resolved to slots, and an evaluator whose temporary strings live on a stack-like
pool that is released at statement boundaries. `cmd | getline` and `print | cmd` use `pipe` + `spawnve` with the child's
fds 0/1 taken from the pipe (no fork), reading the whole command output at once.

`/bin/sh` (`user/bin/sh/`) is a POSIX-style scripting shell: `parse.c` (lexer + recursive-descent parser
into an arena-allocated AST; "incomplete" results drive the `> ` continuation prompt), `expand.c`
(variables, `$(..)`, `$((..))`, `${..}` forms, globbing), `exec.c` (evaluator, pipelines, functions),
`builtin.c`, `edit.c` (line editor, history, Ctrl-R). Without `fork`, anything that needs another process
(`( )`, compound commands or functions inside a pipeline, `cmd &`, `$(..)`) runs as a child `sh -c TEXT`
that is sent the exported environment, plain variables, function definitions, positionals and `-e/-x`.
The kernel runs `#!interp [arg]` scripts itself (`proc_spawn_ex`, one level, 128-byte line).

`/bin/browser` (`user/bin/browser/`) is the Vibe Browser. `fetch.c` runs HTTP in a helper process (`browser --fetch URL [POST-BODY]`)
so DNS/connect never block the UI; `url.c` resolves URLs; `html.c` is a tolerant tokenizer plus a one-pass block/inline layout
into a flat display list (items sorted by y, so drawing and hit tests binary-search; layout reruns only on a new page, a resize,
or when an image's size becomes known). `css.c` parses `<style>`/`style=""` (type, class, id, descendant, child, comma selectors;
specificity, `!important`) and the layout asks it for the cascaded properties of every element as its tag streams past, keeping an
element stack for ancestry; properties land on the layout frame (colour, weight, alignment, margins, padding, 1-6px borders,
background boxes patched to their final height when the element closes, centred `width`/`max-width` columns, `display`). `img.c` is the
per-page image cache: `<img>` queues a URL, two helper processes download, `image_decode` (user/lib/image.c) decodes in-process, and one
scaled copy composited over the page colour is kept per image. `form.c` holds control state keyed by control ordinal (so it survives
relayout), draws inline controls, edits text, and builds the GET query / POST body. `main.c` is the window: toolbar, history,
selection, find bar, select dropdowns. inflate in `image.c` is table-driven (9-bit lookup, slow path for long codes).

## Testing

* `tools/vmtest.py` boots the image in QEMU and drives it through the monitor: type text, send
  keys, move/click/drag the mouse, wait for serial output, take screenshots.
* Sound is captured to `build/audio.wav` (`--audiodev` / `VIBE_AUDIODEV` to override) and checked with `tools/wavcheck.py`.
* `/etc/autostart` lists what the desktop opens at boot (`shell`, `term ARGV`, `run ARGV`).
* Host-side unit tests: `tools/ext2test` (filesystem vs `e2fsck`), `tools/termtest`
  (terminal emulator), `tools/libctest` (libc; also differential tests of printf, strtod and libm against the host's glibc),
  `tools/imagetest` (image codecs vs Pillow, ASan/UBSan); `make test-host` runs the host tests. libctest also holds `regex_diff.c` (regex vs glibc), `tools/hashtest.py` checks `md5sum`/`sha256sum` against hashlib, and `/home/texttest.sh` (run inside VibeOS) is the regression test for the text tools.
