# Makefile for VibeOS Project
# ----------------------------
# This Makefile builds a bootable OS image consisting of a bootloader (Assembly)
# and a C kernel. The kernel is loaded by the bootloader and prints a message to the screen.
#
# Variables:
#   CC, LD: Compiler and linker (set unconditionally to gcc -m32/ld -m elf_i386 for 32-bit output)
#   CFLAGS: Flags for freestanding, 32-bit, no standard libs
#   LDFLAGS: Use custom linker script

CC = gcc -m32
LD = ld -m elf_i386
CFLAGS := -ffreestanding -m32 -nostdlib -nostdinc -fno-pic -fno-pie -fno-stack-protector -fno-builtin -nostartfiles -nodefaultlibs -march=i386 -mtune=i386 -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions -mno-red-zone
LDFLAGS := -T kernel/linker.ld -nostdlib

KERNEL_C_SRCS := $(wildcard kernel/*.c)
KERNEL_OBJS := $(patsubst kernel/%.c,build/%.o,$(KERNEL_C_SRCS))

# Default target: build the OS image
all: build/os-image

# Pattern rule for C files
build/%.o: kernel/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble the bootloader (NASM, 16-bit real mode)
build/bootloader.bin: boot/bootloader.asm
	@mkdir -p build
	nasm -f bin boot/bootloader.asm -o build/bootloader.bin

# Assemble .asm files (entry, interrupts, exceptions)
build/entry.o: kernel/entry.asm
	@mkdir -p build
	nasm -f elf32 $< -o $@
build/interrupts.o: kernel/interrupts.asm
	@mkdir -p build
	nasm -f elf32 $< -o $@
build/exceptions_asm.o: kernel/exceptions.asm
	@mkdir -p build
	nasm -f elf32 $< -o $@

# Link all kernel objects
build/kernel.bin: build/entry.o $(KERNEL_OBJS) build/interrupts.o build/exceptions_asm.o
	@mkdir -p build
	$(LD) $(LDFLAGS) build/entry.o $(KERNEL_OBJS) build/interrupts.o build/exceptions_asm.o -o build/kernel.elf
	objcopy -O binary build/kernel.elf build/kernel.bin

# Create the OS image
build/os-image: build/bootloader.bin build/kernel.bin
	dd if=/dev/zero of=build/os-image bs=512 count=2880
	dd if=build/bootloader.bin of=build/os-image conv=notrunc
	dd if=build/kernel.bin of=build/os-image bs=512 seek=1 conv=notrunc

# Run the OS in QEMU (SDL display)
run: build/os-image
	qemu-system-i386 -drive format=raw,file=build/os-image -display sdl

# Clean build artifacts
clean:
	rm -rf build/*.o build/kernel.bin build/bootloader.bin build/os-image build/kernel.elf 