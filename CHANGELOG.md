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
- [x] `sh`: raw-mode line editor, persisted history, Tab completion, pipes, redirections, `; && || &`, globbing, quoting, $vars, scripts, `/etc/profile` — agent
- [x] Networking: PCI, e1000, ARP/IPv4/ICMP/UDP/DHCP/DNS, TCP client, sockets + poll; ifconfig ping nslookup wget nc — agent
- [x] Coreutils: ls cat echo mkdir rm rmdir touch cp mv pwd wc head tail grep hexdump ps kill uptime date free clear, plus sleep env true false yes seq sort uniq tee find which basename dirname stat dmesg uname whoami cal fortune beep shutdown reboot vibefetch; TUIs: less top edit — agent
- [x] `sh` scripting shell: recursive-descent parser + AST evaluator; `if/elif/else`, `while/until`, `for`, `case` (globs), `{ }`, `( )`, `!`, `break/continue [n]`, functions with `return`/`local`; `$(..)`/backticks, `$((..))`, `${v:-x} ${v:=x} ${v:+x} ${#v} ${v#p} ${v%p}`, `"$@"`; here-docs (`<<EOF`, `<<'EOF'`, `<<-`); builtins `test/[ read shift set(-e -x) printf echo eval exec type alias unalias wait local return`; plain `A=1` is shell-local until `export`; `!!` `!prefix` `!N`, Ctrl-R search, multi-line compound commands at the prompt; kernel `#!` support in `proc_spawn`; `/home/demo.sh` tour and `/home/shtest.sh` regression test (162 checks, run `sh /home/shtest.sh`) — agent

## Phase 2 — Desktop
- [x] PS/2 mouse driver
- [x] Double-buffered compositor, window manager (drag, focus, z-order, close, minimise)
- [x] Desktop right-click menu; 5 wallpaper themes (Aurora, Ocean, Sunset, Forest, Midnight), saved in /etc/wallpaper
- [x] Smart window placement (least overlap on a 24 px grid, cascade when the screen is full)
- [x] Window open animation (fade + rise, ease-out, 180 ms)
- [x] Window snapping: drag to left/right edge to tile, top to maximise, with preview; drag a tiled window off to restore; Super+←/→/↑ tile, Super+↓ restore, Super tap opens the launcher
- [x] Desktop: wallpaper, taskbar, launcher menu, clock
- [x] Anti-aliased UI font baked at build time
- [x] Terminal windows (multiple shells)
- [x] Window syscalls + userland GUI library (libgui: gui.h windows/events, ui.h immediate-mode widgets)
- [x] GUI apps: system monitor, paint, calculator, clock, file manager, text editor (`user/bin/{sysmon,paint,calc,clock,files,notepad}.c`) — agent
- [x] Vibe Browser (`user/bin/browser/`): HTTP/1.1 GET in a helper process (redirects, chunked, timeout, Stop), HTML subset renderer (headings, lists, tables, pre, links, entities, UTF-8), history, anchors, `file://` + directory listings, built-in start page, friendly HTTPS-unsupported page; verified in QEMU against info.cern.ch, example.com, neverssl.com, httpbin redirects and a local test server
- [x] About VibeOS app (first ui.h app, verified open/close in QEMU)
- [x] Games: snake, tetris (SRS, hold, ghost, DAS), minesweeper (3 levels, chording) — agent

## Browser
- [x] Images: `<img>` PNG/BMP/PPM from http and file URLs, width/height attributes, shrunk to the column, placeholders while loading, two helper-process downloads at a time, per-page cache of decoded + scaled images, direct viewing of image URLs; inflate is table-driven (`tools/imagetest` 222/222)
- [x] CSS: `<style>` + `style=""`, selectors (type, .class, #id, descendant, `>`, comma lists), cascade with specificity/`!important`; color, background(-color), font-weight/size/family, text-align, text-decoration, margin/padding, border(-top/-bottom/-left/-right), width/max-width with `margin: auto`, display none/block/inline, list-style none; colours as names, #rgb, #rrggbb, rgb()/rgba()
- [x] Forms: GET and POST, text/search/password/hidden/checkbox/radio/submit/button/reset inputs, textarea, select dropdown, `<button>`; fields edit inline (caret, paste), Enter submits, Tab moves
- [x] Find in page (Ctrl+F, Enter / Shift+Enter, F3), mouse text selection with Ctrl+C to the system clipboard, Ctrl+A
- [ ] External stylesheets (`<link>`), tables as a grid, JPEG/GIF, HTTPS

## Networking
- [x] PCI enumeration, e1000 (82540EM) driver with RX/TX rings + IRQ, net thread (`kernel/net/`)
- [x] Ethernet, ARP cache, IPv4, ICMP echo (answer + send), UDP, DHCP at boot, DNS resolver, TCP client (retransmit, FIN/RST)
- [x] Sockets as fds (`socket connect bind sendto recvfrom`, `resolve`, `netinfo`; syscalls 60-66), poll() support
- [x] `ifconfig ping nslookup wget nc` (verified in QEMU user net: ping gw, DNS, HTTP GET incl. a 16 MB download)
- [x] TCP server side: `listen`/`accept` (syscalls 67-68, SYN_RCVD children, backlog, poll on listeners), `nc -l`, `/bin/httpd` (static files, listings, index.html), `make run` forwards host :8080 → guest :80
- [x] Loopback: 127/8 and our own address are queued back into the net thread; `localhost` resolves; the in-OS browser loads the in-OS httpd (http://localhost:8000/). `ping 127.0.0.1` and `wget` over loopback verified
- [x] **HTTPS**: BearSSL 0.6 (`third_party/bearssl`, unmodified) + `user/lib/tls.c` → `libtls.a` (`tls_open/read/write/close`, `<tls.h>`); trust anchors parsed from `/etc/ssl/cert.pem` (the host's Mozilla bundle at build time, `CA_BUNDLE=` overrides); `wget` and the browser (pages, images, forms) speak https://. TLS 1.0-1.2, no 1.3. `make test-tls`: throwaway CA + host servers, accept / not-trusted / wrong-name all verified
- [ ] IP fragments, DHCP renewal

## Lua
- [x] Lua 5.4.9 (`third_party/lua`, unmodified) as `/bin/lua`: REPL with line editing + history, `lua script.lua args`, `-e`, `-`; `vibe` module (sysinfo ps beep sleep uptime listdir ...) and `gui` module (windows, drawing, events); `projects/luatest.lua` (183 checks, all pass), fib/mandelbrot/sysinfo/gui samples. libc grew hex-float `strtod`, `%a`, `freopen tmpfile tmpnam signal setlocale strcoll frexp ldexp modf`, ISO-week `strftime`, exact libm (<= 2 ulp vs glibc), and a binned O(1) `malloc` (the old one was quadratic: 100k-deep Lua recursion took 6.5 s, now 0.1 s); glibc differential tests for printf/strtod/libm in `tools/libctest`

## Toolchain
- [x] TinyCC 0.9.27 (`third_party/tcc`, unmodified) as `/bin/tcc`: single translation unit configured by `user/bin/tcc/config.h`, `user/bin/tcc/vibe.c` prepends `-static -Wl,-Ttext=0x40001000` (the user load address) so `tcc a.c -o a` just works; the kernel ELF loader accepts tcc's output unchanged
- [x] In-OS SDK in the rootfs: libc headers + `gui.h ui.h image.h` in `/usr/include`, `crt1.o libc.a libgui.a` in `/usr/lib`, `libtcc1.a` (tcc's 64-bit helpers + alloca, built with gcc) and tcc's own headers in `/usr/lib/tcc`
- [x] `tcc hello.c -o hello`, `tcc -run`, multi-file (`tcc main.c sieve.c`), `-c` + link, GUI via `-lgui`: verified in QEMU (`projects/hello.c`, `projects/sieve/`, `projects/bounce.c`, `projects/README.txt`)
- [x] libc additions: `stdint.h inttypes.h sys/time.h sys/mman.h`, `gettimeofday mprotect execvp` (spawn+wait emulation) in `user/lib/compat.c`, `strtold` (double precision); host-tested in `tools/libctest`
- [ ] tcc `-run` backtraces / `-b` bounds checking (needs signals with handlers), `long double` literals beyond 53 bits, `__builtin_*` headers, a `make`

## Desktop
- [x] 4 virtual workspaces: Super+1..4 switch, Super+Shift+1..4 move the focused window, Ctrl+Super+←/→ cycle; clickable numbers in the taskbar (a dot marks workspaces holding windows); the window list shows only the current workspace; 120 ms cross-fade between workspaces
- [x] Alt+Tab switcher: overlay with live downscaled window thumbnails and titles, most recently used first; Tab / Shift+Tab (or arrows) cycle while Alt is held, release to focus, Esc cancels, click picks
- [x] Notifications: kernel queue + toasts at the top right (accent bar, title, wrapped body, slide-in, 4 s auto-dismiss, click to dismiss); `/dev/notify` (`title\nbody[\nkind]`), `/bin/notify [-k good|warn|bad] TITLE [BODY]`, `gui_notify()` in libgui; notification center (click the clock or Super+N) with history, per-item dismiss, Clear all, scroll; unread dot by the clock; `/bin/screenshot` now posts a notification instead of its own toast window
- [x] Window polish: double-click title bar maximises/restores, right-click title bar menu (minimise, maximise/restore, always on top, move to workspace N, close), Super+drag moves a window from anywhere
- [x] Tray: volume icon (only with `/dev/mixer`; scroll to change) and network icon (lease state); click either for quick settings (volume slider + mute, IP)
- [x] vmtest: `keydown:NAME` / `keyup:NAME` hold keys through QMP (Alt+Tab, Super+drag)

## Phase 3 — Polish
- [x] Boot splash with progress (logo, gradient bar, live kprintf status, crossfade into desktop)
- [x] RTC clock / date
- [x] PC speaker: `beep` syscall, startup chime; `make run` routes it to the host via PulseAudio
- [x] README with screenshots, architecture docs (`docs/ARCHITECTURE.md`)
- [x] Settings app: live wallpaper thumbnails, time zone picker, system info; desktop reloads prefs via `WIN_SET_PREFS`
- [x] Double-buffered `ui.h` windows: draw privately, copy only changed rows (no half-drawn frames reaching the compositor)
- [x] `time` command; faster spawns: binaries stripped into the rootfs (16 MB → 5.7 MB of /bin), ext2 block cache 32 → 1024 blocks (`$(..)` 53 → 18 ms, `ls` 37 → 15 ms)
- [x] QEMU uses KVM when available (`make run` and vmtest; falls back to TCG): demo.sh 3.5 s → 0.3 s
- [x] System clipboard `/dev/clipboard`: drag-select in terminals copies, Ctrl+Shift+C/V and middle-click; Notepad and text boxes share it; `cat`/`tee` work on it
- [x] ATA bus-master DMA (PIIX IDE, 64 KiB bounce buffer; PIO fallback), halting on the IRQ while busy: under KVM every PIO-written word trapped to the host (~2 ms/sector); copying 541 KB 11.5 s → 0.6 s, PrtSc screenshots now save in ~1 s
- [x] Signal handlers: `signal`/`sigaction`/`sigprocmask`, frames on the user stack with saved x87 state, `sigreturn`, EINTR, SIGCHLD/SIGWINCH; `trap` in sh (EXIT, INT in scripts); Ctrl-C now interrupts Lua scripts instead of killing lua (`sigtest` 13/13, shtest 169/169)

- [x] Images: `user/lib/image.c` (libgui) — from-scratch zlib inflate, PNG decoder (all colour types, depths 1-16, all filters, Adam7, tRNS), BMP 24/32 and binary PPM/PGM, PNG encoder (LZ77 + fixed Huffman, valid CRC/Adler), area-average/bilinear resampler; host-tested against Pillow under ASan/UBSan (`tools/imagetest`, `make test-host`)
- [x] Viewer app (`/bin/viewer`): fit-to-window, smooth zoom (wheel, +/-), drag to pan, 1:1, arrow keys browse the folder, checkerboard behind transparency; Files opens .png/.bmp/.ppm in it; launcher entry "Photos" opens ~/Pictures
- [x] Screenshots: PrtSc spawns `/bin/screenshot`, which reads the composed screen from the new `/dev/screen` and writes `/home/Pictures/screenshot-N.png` (toast when done); `KEY_PRTSC` in the ABI
- [x] Paint saves PNG (or BMP by extension) and opens PNG/BMP/PPM; sample pictures generated by `tools/mkpictures.py`

## Audio
- [x] AC'97 (ICH, 8086:2415) driver: NAM/NABM, 32-entry buffer descriptor list, IRQ-driven refill, master volume; shared PCI IRQ lines (`irq_register` chains handlers)
- [x] `/dev/dsp` (48 kHz stereo s16le; blocking `write`, `poll`, draining `close`, `AUDIO_GETQ/SETVOL/GETVOL` ioctls) and `/dev/mixer`; both absent without a card, PC speaker path untouched
- [x] `play FILE.wav` (8/16-bit, mono/stereo, linear resample to 48k), `tone [hz] [ms]`, `/home/Music/arpeggio.wav` (`tools/mkwav.py`)
- [x] Piano app: two-octave keyboard (mouse + computer keys), sine/square/saw/triangle, ADSR, 16-voice polyphony, scope; 10 ms chunks, ~30 ms queued
- [x] vmtest: AC'97 + `wav` audiodev (`build/audio.wav`, configurable), `tools/wavcheck.py` to find tones/RMS in the capture
- [ ] Sample-rate conversion in the kernel (VRA), recording, `/dev/dsp` mixing of several writers

## Doom
- [x] doomgeneric (GPL-2.0, pinned commit, unmodified) vendored in `third_party/doomgeneric/`; platform layer in `user/bin/doom/` (libgui window, `uptime_ms` clock, key queue with real key-up events, no stuck keys on focus loss)
- [x] Freedoom 0.13.0 (BSD) fetched on demand: `make doom-wad` / `tools/getwad.sh` (pinned zip, sha256-checked, cached in `build/wads/`, never committed); the rootfs build copies it to `/usr/share/doom/`; `FS_MB` 64 -> 96
- [x] `/bin/doom`: finds the IWAD or says so (terminal message, or a window when started from the launcher); `-iwad FILE` works; config + savegames in `/home/.doom`; Quit menu and the close button exit cleanly; launcher entry
- [x] SFX through `/dev/dsp`: 16-channel mixer, Doom panning, 11025 -> 48 kHz linear resampling, ~40 ms queued via `AUDIO_GETQ`, never blocks the game loop; verified in the captured WAV
- [x] libc: only `inttypes.h` was missing; everything else doomgeneric needs already linked
- [ ] Music (needs MUS -> OPL/MIDI synthesis; the music module is a stub), mouse look, resizable scaling (frame is centred, fixed 2x), `/dev/dsp` shared with other writers
## Text tools
- [x] POSIX regex in libc (`user/include/regex.h`, `user/lib/regex.c`): `regcomp/regexec/regerror/regfree`, BRE and ERE (+ GNU `\| \+ \? \w \s \b \< \>`), `REG_ICASE/NEWLINE/NOSUB/NOTBOL/NOTEOL/STARTEND`, groups with submatch offsets, alternation, `* + ? {m,n}`, bracket expressions with classes/ranges/negation, anchors, back-references; a Pike VM (leftmost-longest, linear time) with a step-bounded backtracker only for back-references
- [x] `tools/libctest/regex_diff.c`: differential test against glibc (hand corpus x 4 flag sets x NOTBOL/NOTEOL + 40k random patterns, compile errors and extents compared; 270k checks, 0 failures)
- [x] `grep` rewritten on it: `-E -F -G -i -v -n -c -l -L -r -o -w -x -q -s -H -h -e... -A -B -C -NUM`
- [x] `sed`: `s///` (g p N I, `\U\L\u\l`), `y`, `d D p P q Q n N = a i c r h H g G x b t T : { }`, addresses `N $ /re/ first~step a,b a,+N !`, `-n -e -f -E -i[SUF]`; checked against GNU sed on 60+ scripts
- [x] `tr` (sets, ranges, classes, `-c -d -s`), `cut` (`-b -c -f -d -s`), `xargs` (`-n -I -0 -t -r`, spawnvp), `cmp` (`-s -l`), `tac`, `rev`, `nl`, `paste` (`-s -d`)
- [x] `diff`: Myers O(ND); normal output and `-u/-U N` byte-identical to GNU diff on unique-line inputs, `-q -i -w -b`, "\\ No newline at end of file"
- [x] `awk`: patterns/ranges/BEGIN/END, fields and NF/NR/FNR/FS/OFS/ORS/RS (single char, paragraph, regex), printf/print with `> >> |`, user functions (arrays by reference), assoc arrays and `in`/`delete`/SUBSEP, getline (plain, var, `< file`, `cmd |`), length substr index split sub gsub match sprintf tolower toupper int sqrt sin cos atan2 exp log rand srand system close; compared with gawk on ~140 programs
- [x] `md5sum`, `sha256sum` (`-c` to verify; `tools/hashtest.py` checks both against hashlib)
- [x] `/home/texttest.sh`: 247 checks for all of the above (`sh /home/texttest.sh`)
- [ ] `join`, `sort -k/-t`, `tail -n +N`, multibyte (UTF-8) awareness in regex and awk, awk `printf %c` for code points > 255

## Log
- 2026-10-22: Cloned original VibeOS (~2.7k lines: 1-sector bootloader, 16bpp VESA console, in-RAM FS, IRQ-driven shell). Started v2 branch.
- Phase 0 done: boots via new stage1/stage2 into a 1280x800x32 framebuffer, kernel at 1 MiB, paging + heap + 1 kHz timer; anti-aliased JetBrains Mono / Noto Sans glyphs baked by `tools/mkfont.py`; shared `common/gfx.c` (alpha blending, rounded rects, shadows, AA lines); styled panic screen with register dump + backtrace.
- Phase 1 kernel: xv6-style scheduler (non-preemptive kernel, preemptive user mode, FPU state per task), per-process page directories with demand-grown stacks, `spawn`/`waitpid`/signals, 35 syscalls, VFS with mounts + `/dev` (null zero random tty kmsg) + `/proc` (meminfo uptime cpuinfo version processes), pipes, canonical/raw TTY with ^C/^D/^U, CMOS RTC. ATA PIO + read/write ext2 (agent, host-tested with e2fsck at 1k/2k/4k blocks). ANSI/xterm terminal emulator with 256/truecolor, scroll regions, alt screen, procedural box drawing (agent, host-tested).
- Phase 2 desktop: in-kernel compositor with damage tracking, procedural aurora wallpaper, rounded windows with soft shadows and traffic-light buttons, drag/resize/maximise/minimise, taskbar with launcher, window list, CPU meter and clock (`/etc/tz`), vmmouse absolute pointer (no mouse grab in QEMU), terminal windows. First ring-3 program ran in a terminal window.
- Phase 2 apps: six libgui apps (files, notepad, calc, clock, sysmon, paint) on the immediate-mode toolkit; ui.c gained ui_text_fit, ui_tabs, ui_tool, ui_vscroll, ui_id, ui_prompt, ui_confirm; vmtest.py gained `dclick`.
- Phase 3 (2026-10-22): userland fanned out to agents (shell, coreutils + TUIs, GUI apps, games, networking) and merged; boot splash, startup chime, window animations, snapping, smart placement, desktop menu and wallpaper themes; UTF-8 text with ~150 extra glyphs. Fixed: ATA spin-count timeouts failing large writes with EIO, shadows filling their whole interior (multi-second first frames), full-window flips every frame (ui now flips only what changed: Tetris+compositor 54% → 14% CPU), AA lines scanning their whole bounding box (clock 25% → 10%), localtime ignoring /etc/tz, stale rootfs after deleting files under root/.
- Settings app; wallpaper renderer moved to `common/` so apps can draw thumbnails. Fixed: ui diff-flip could let the compositor capture a half-drawn frame that was never re-flipped (a thumbnail vanished after a click), now double-buffered.
- Merged the scripting shell (if/for/while/case, functions, `$(..)`, `$((..))`, here-docs, builtins, Ctrl-R, `!!`, kernel `#!`; 162/162 in shtest.sh). Profiling it showed every spawn re-reading the whole unstripped ELF over PIO through a 32-block cache.
- Audio (2026-10-22): AC'97 driver + `/dev/dsp`, `play`, `tone`, Piano app. Verified in QEMU against the captured WAV (440/1000 Hz tones, arpeggio notes within 1 Hz, 8-bit/8 kHz stereo resampled to 1000 Hz). Fixed: `irq_register` replaced the previous handler, so the AC'97 card on IRQ 11 silently killed the e1000.
- Images: PNG/BMP/PPM codecs in libgui (own inflate + PNG encoder, 222 host checks vs Pillow under sanitizers), `/bin/viewer`, PrtSc screenshots via `/dev/screen` + `/bin/screenshot`, PNG in Paint and Files. Fixed along the way: a float bilinear resampler took >1 s per 800x480 frame under QEMU (frames lagged input), now packed-integer lerp + row-copy at 1:1.
- Merged the audio, Lua and image agents. A slow PrtSc led to the disk: writes ran at ~47 KB/s under KVM because `rep outsw` exits per word; reads were fine. DMA fixed it; e2fsck clean after a write-heavy session.
- Toolchain (2026-10-22): TinyCC 0.9.27 ported as `/bin/tcc` with an in-OS SDK (`/usr/include`, `/usr/lib`); compiles and runs C, multi-file and libgui programs inside VibeOS. Needed `__udivmoddi4` exported from libtcc1 because the gcc-built libc.a calls it, and `-Ttext` at the user base since tcc defaults to 0x08048000.
- TCP server sockets + `/bin/httpd`: the host browses VibeOS at http://localhost:8080 under `make run`; vmtest gained `--hostfwd`, `--pcap` and `host:CMD`. The capture showed two stack bugs: the first SYN-ACK waited 500 ms on ARP (the net thread can't sleep, so it fell to the retransmit timer; senders' MACs are now learned from incoming IP frames), and a burst of tiny segments overran the 32-slot e1000 RX ring and dropped the next SYN (ring is now 128, httpd batches its writes).
- Doom (2026-10-22): doomgeneric + Freedoom run in a 640x400 desktop window with sound. `-timedemo demo1` renders 202 fps uncapped (7117 tics in 35 s, KVM), so the 35 fps cap has plenty of headroom: doom itself uses <10% CPU, but the in-kernel compositor spends ~85% of the CPU re-composing a 640x400 window 35 times a second (a pre-existing cost of full-window damage; worth optimising). Gotchas: doomkeys.h `#define`s names that `abi.h` declares as enum constants (so abi.h is force-included first and the platform layer captures our codes before including doomkeys.h), and doomgeneric's `I_Quit` returns instead of exiting (an `I_AtExit` hook does the `exit`).
- Desktop (2026-10-22): workspaces, Alt+Tab thumbnails, notifications (`/dev/notify`, toasts, center), window menu, Super+drag and a volume/network tray, all in `wm.c` and damage-tracked (idle stays at 1% CPU: indicators only damage their own rectangle, toast expiry and tray polling run on the 250 ms tick). Held modifiers needed QMP `input-send-event`, since HMP `sendkey` always releases.
- Merged tcc, Doom and desktop. Profiling the compositor under Doom (85% CPU) showed 97% of it in the final copy to the framebuffer: it was mapped uncached (PCD), and under KVM on AMD the guest's UC is honoured, so every pixel store cost ~260 cycles. Mapping it write-combining through PAT entry 1 took Doom's desktop load from 85% to ~8% and speeds up every screen update. Also: stage1 now rebuilds when the disk layout changes (a stale partition table after `FS_MB` 64→96 failed the root mount).
- Browser: images, CSS, forms, find and selection (see Browser). Rendering pitfalls found on the way: first_item() assumed items are under 40 px tall, so a tall textarea/image scrolled partly off-screen vanished or couldn't be clicked (now tracks the tallest item); a positional struct initialiser silently put a colour into `link` after a field was added. inflate went from bit-at-a-time to a 9-bit table (1.7x on a synthetic 4.5 MB stream).
- HTTPS (2026-10-22): BearSSL compiled unmodified against our libc (all 277 files, config by -D: /dev/urandom seeding, unix time for certificate dates, no x86 intrinsics). Tested only against local servers with a throwaway CA, not live sites. 300 KB over TLS in 0.45 s including the handshake; a stripped `wget` grew to 152 KB. TLS reads skip poll() because BearSSL can hold decrypted bytes the socket no longer shows. The rootfs now rebuilds when `CA_BUNDLE` changes (a stamp), after a test CA leaked into a normal build.
- Text tools (2026-10-22): a from-scratch POSIX regex engine in libc (Pike VM; glibc differential test with 270k checks) and a text-processing toolbox on top of it: grep, sed, tr, cut, xargs, cmp, diff, tac, rev, nl, paste, awk, md5sum, sha256sum; verified against GNU sed/diff/grep/awk on the host and by `texttest.sh` (247 checks) in QEMU. Found on the way: `tail` has no `-n +N`, and libctest no longer linked after `signal` moved into syscall.c (stubbed).
