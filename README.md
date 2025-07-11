# VibeOS

VibeOS is a hobby operating system project, written for fun and learning. It is not intended for any practical or production use—this is a purely vibe-coded project!

## Features

- **Custom Bootloader**: Loads the kernel from disk, sets up protected mode, and jumps to the kernel entry point using a proper far jump.
- **Robust Protected Mode Entry**: The kernel sets up its own GDT and segment registers, ensuring reliable operation regardless of the bootloader.
- **VGA Text Console**: Direct output to VGA text mode, with support for clearing the screen, scrolling, and cursor updates.
- **Interrupt-Driven Shell**: The shell is now fully interrupt-driven—keyboard input is handled by the keyboard interrupt handler, providing responsive and accurate command input.
- **Shell Commands**: Includes commands like `help`, `clear`, `count`, `ls`, `cat`, `echo`, and a simple file editor.
- **Simple In-Memory Filesystem**: Supports basic file operations in RAM.
- **Nano-like Editor**: Edit files in a minimal, non-modal text editor.
- **Larger Kernel Support**: The bootloader now loads up to 32 sectors (16 KB) of kernel code, supporting more features and larger binaries.

## Disclaimer

**This project is for educational and entertainment purposes only.**

- VibeOS is a "vibe-coded" project: it is not intended for any practical, production, or real-world use.
- Expect bugs, missing features, and wild design choices.
- If you want to learn OS development, feel free to use this as a reference or playground!

## Building and Running

See the Makefile for build instructions. You will need `nasm`, `gcc` (with 32-bit support), and `qemu` or another emulator.

```sh
make clean && make all
make run
```

---

Enjoy the vibes! ✨ 