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
LDFLAGS := -T kernel/arch/linker.ld -nostdlib

KERNEL_C_SRCS := \
 $(wildcard kernel/core/*.c) \
 $(wildcard kernel/drivers/*.c) \
 $(wildcard kernel/fs/*.c) \
 $(wildcard kernel/editor/*.c) \
 $(wildcard kernel/commands/*.c)
KERNEL_OBJS := $(patsubst kernel/%,build/%,$(KERNEL_C_SRCS:.c=.o))
KERNEL_ARCH_C_SRCS := $(wildcard kernel/arch/*.c)
KERNEL_ARCH_OBJS := $(patsubst kernel/%,build/%,$(KERNEL_ARCH_C_SRCS:.c=.o))

# Default target: build the OS image
all: build/os-image

# Pattern rule for C files in all subdirs
build/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Pattern rule for C files in kernel/arch
build/arch/%.o: kernel/arch/%.c
	@mkdir -p build/arch
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble the bootloader (NASM, 16-bit real mode)
build/bootloader.bin: boot/bootloader.asm
	@mkdir -p build
	nasm -f bin boot/bootloader.asm -o build/bootloader.bin

# Assemble .asm files (entry, interrupts, exceptions)
build/arch/entry.o: kernel/arch/entry.asm
	@mkdir -p build/arch
	nasm -f elf32 $< -o $@
build/arch/interrupts.o: kernel/arch/interrupts.asm
	@mkdir -p build/arch
	nasm -f elf32 $< -o $@
build/arch/exceptions_asm.o: kernel/arch/exceptions.asm
	@mkdir -p build/arch
	nasm -f elf32 $< -o $@

# Link all kernel objects
build/kernel.bin: build/arch/entry.o $(KERNEL_OBJS) build/arch/interrupts.o build/arch/exceptions_asm.o $(KERNEL_ARCH_OBJS)
	@mkdir -p build
	$(LD) $(LDFLAGS) build/arch/entry.o $(KERNEL_OBJS) build/arch/interrupts.o build/arch/exceptions_asm.o $(KERNEL_ARCH_OBJS) -o build/kernel.elf
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
	rm -rf build/*.o build/arch/*.o build/kernel.bin build/bootloader.bin build/os-image build/kernel.elf 