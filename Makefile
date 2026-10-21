# VibeOS v2 build
#   make            -> build/vibeos.img (bootable raw disk: MBR + stage2 + kernel + ext2 root)
#   make run        -> boot it in QEMU
#   make test       -> headless smoke test (tools/vmtest.py)

CC      := gcc
LD      := ld -m elf_i386
NASM    := nasm
GCCINC  := $(shell $(CC) -print-file-name=include)
LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)
CWARN   := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CBASE   := -m32 -march=i686 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -nostdinc -isystem $(GCCINC) \
           -O2 -g $(CWARN) -Icommon
KCFLAGS := $(CBASE) -mgeneral-regs-only -Ikernel/include
# gfx_line_aa uses x87; keep floats legal in the shared gfx object
KGFXFLAGS := $(CBASE) -Ikernel/include

KERNEL_LBA := 64
FS_LBA     := 8192
FS_MB      := 64
FS_SECTORS := $(shell echo $$(( $(FS_MB) * 2048 )))

KSRC  := $(wildcard kernel/*/*.c)
KASM  := $(wildcard kernel/arch/*.asm)
KOBJ  := $(patsubst %.c,build/%.o,$(KSRC)) $(patsubst %.asm,build/%.o,$(KASM)) \
         build/common/gfx.k.o build/common/fonts.k.o build/common/term.k.o

all: build/vibeos.img

build/kernel/%.o: kernel/%.c $(wildcard kernel/include/*.h common/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

build/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -g $< -o $@

build/common/%.k.o: common/%.c common/*.h
	@mkdir -p $(dir $@)
	$(CC) $(KGFXFLAGS) -c $< -o $@

build/kernel.elf: $(KOBJ) kernel/arch/linker.ld
	$(LD) -T kernel/arch/linker.ld --no-warn-rwx-segments -o $@ $(KOBJ) $(LIBGCC)

build/kernel.bin: build/kernel.elf
	objcopy -O binary $< $@

build/stage1.bin: boot/stage1.asm
	@mkdir -p build
	$(NASM) -f bin -DSTAGE2_SECTORS=32 -DFS_LBA=$(FS_LBA) -DFS_SECTORS=$(FS_SECTORS) $< -o $@

build/stage2.bin: boot/stage2.asm build/kernel.bin
	$(NASM) -f bin -DKERNEL_LBA=$(KERNEL_LBA) \
	  -DKERNEL_SECTORS=$$(( ($$(stat -c %s build/kernel.bin) + 511) / 512 )) $< -o $@
	@test $$(stat -c %s $@) -le 16384 || (echo "stage2 too big"; exit 1)

build/root.ext2: $(shell find root -type f 2>/dev/null)
	@mkdir -p build/rootfs
	@cp -a root/. build/rootfs/ 2>/dev/null || true
	rm -f $@
	mke2fs -q -t ext2 -b 1024 -I 256 -L vibeos -d build/rootfs $@ $(FS_MB)M

build/vibeos.img: build/stage1.bin build/stage2.bin build/kernel.bin build/root.ext2
	@test $$(stat -c %s build/kernel.bin) -le $$(( ($(FS_LBA) - $(KERNEL_LBA)) * 512 )) || (echo "kernel too big"; exit 1)
	rm -f $@
	truncate -s $$(( ($(FS_LBA) + $(FS_SECTORS)) * 512 )) $@
	dd if=build/stage1.bin of=$@ conv=notrunc status=none
	dd if=build/stage2.bin of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=build/kernel.bin of=$@ bs=512 seek=$(KERNEL_LBA) conv=notrunc status=none
	dd if=build/root.ext2 of=$@ bs=512 seek=$(FS_LBA) conv=notrunc status=none

QEMU := qemu-system-i386 -m 256 -drive file=build/vibeos.img,format=raw,if=ide \
        -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=4 -no-reboot

run: build/vibeos.img
	$(QEMU)

debug: build/vibeos.img
	$(QEMU) -s -S & gdb build/kernel.elf -ex 'target remote :1234'

test: build/vibeos.img
	python3 tools/vmtest.py smoke

clean:
	rm -rf build

.PHONY: all run debug test clean
