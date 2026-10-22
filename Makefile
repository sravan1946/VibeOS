# VibeOS v2 build
#   make            -> build/vibeos.img (bootable raw disk: MBR + stage2 + kernel + ext2 root)
#   make run        -> boot it in QEMU
#   make test       -> headless smoke test (tools/vmtest.py)
#   make test-host  -> host unit tests (libc, image codecs)
#   make doom-wad   -> fetch Freedoom (for /bin/doom) into build/wads/

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
FS_MB      := 96
FS_SECTORS := $(shell echo $$(( $(FS_MB) * 2048 )))

KSRC  := $(wildcard kernel/*/*.c)
KASM  := $(wildcard kernel/arch/*.asm)
KOBJ  := $(patsubst %.c,build/%.o,$(KSRC)) $(patsubst %.asm,build/%.o,$(KASM)) \
         build/common/gfx.k.o build/common/fonts.k.o build/common/term.k.o build/common/wallpaper.k.o

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

build/stage1.bin: boot/stage1.asm Makefile   # FS_LBA/FS_SECTORS are baked into the partition table
	@mkdir -p build
	$(NASM) -f bin -DSTAGE2_SECTORS=32 -DFS_LBA=$(FS_LBA) -DFS_SECTORS=$(FS_SECTORS) $< -o $@

build/stage2.bin: boot/stage2.asm build/kernel.bin
	$(NASM) -f bin -DKERNEL_LBA=$(KERNEL_LBA) \
	  -DKERNEL_SECTORS=$$(( ($$(stat -c %s build/kernel.bin) + 511) / 512 )) $< -o $@
	@test $$(stat -c %s $@) -le 16384 || (echo "stage2 too big"; exit 1)

# ---- userland: libc.a + libgui.a, every user/bin/NAME.c or user/bin/NAME/*.c -> /bin/NAME ----
UCFLAGS := -m32 -march=i686 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -nostdinc -isystem $(GCCINC) \
           -Iuser/include -Iuser/lib -Icommon -O2 -g $(CWARN) -MMD -MP
ULIBC_SRC := $(wildcard user/lib/*.c)
# trust anchors for TLS: the host's Mozilla bundle becomes /etc/ssl/cert.pem (override with CA_BUNDLE=...)
CA_BUNDLE ?= /etc/ssl/certs/ca-certificates.crt
build/ca.stamp: FORCE      # rebuild the rootfs when CA_BUNDLE points somewhere else
	@mkdir -p build; echo '$(CA_BUNDLE)' | cmp -s - $@ || echo '$(CA_BUNDLE)' > $@
FORCE:
ULIBC_ASM := $(filter-out user/lib/crt0.asm,$(wildcard user/lib/*.asm))
# gui.o + the shared gfx/fonts live in libgui.a so only GUI programs pay for the font data
ULIBGUI_OBJ := build/user/obj/lib/gui.o build/user/obj/lib/ui.o build/user/obj/lib/image.o build/user/obj/common/gfx.o build/user/obj/common/fonts.o build/user/obj/common/wallpaper.o
ULIBC_OBJ := $(filter-out build/user/obj/lib/gui.o build/user/obj/lib/ui.o build/user/obj/lib/image.o build/user/obj/lib/tls.o,$(patsubst user/%.c,build/user/obj/%.o,$(ULIBC_SRC))) \
             $(patsubst user/%.asm,build/user/obj/%.o,$(ULIBC_ASM))
ULIBS := build/user/libgui.a build/user/libtls.a build/user/libc.a
UBIN_SINGLE := $(basename $(notdir $(wildcard user/bin/*.c)))
UBIN_MULTI := $(notdir $(patsubst %/,%,$(wildcard user/bin/*/)))
UBINS := $(addprefix build/user/bin/,$(UBIN_SINGLE) $(UBIN_MULTI))
UCRT0 := build/user/obj/lib/crt0.o

build/user/obj/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/user/obj/common/%.o: common/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

build/user/obj/%.o: user/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -g $< -o $@

# string.c must not let gcc turn its loops back into calls to memset/memcpy
build/user/obj/lib/string.o: UCFLAGS += -fno-tree-loop-distribute-patterns

build/user/libc.a: $(ULIBC_OBJ)
	rm -f $@ && ar rcs $@ $^
build/user/libgui.a: $(ULIBGUI_OBJ)
	rm -f $@ && ar rcs $@ $^

# libtls.a: user/lib/tls.c over BearSSL 0.6 (third_party/bearssl, unmodified; configured by -D, see README.vibeos)
BEARSSL_SRC := $(shell find third_party/bearssl/src -name '*.c')
BEARSSL_OBJ := $(patsubst %.c,build/user/obj/%.o,$(BEARSSL_SRC))
BEARSSL_FLAGS := -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -DBR_64=0 -DBR_USE_URANDOM=1 -DBR_USE_UNIX_TIME=1 \
                 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_RDRAND=0 -DBR_POWER8=0
$(BEARSSL_OBJ): build/user/obj/third_party/bearssl/src/%.o: third_party/bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(BEARSSL_FLAGS) -w -c $< -o $@
build/user/obj/lib/tls.o: UCFLAGS += -Ithird_party/bearssl/inc
build/user/libtls.a: build/user/obj/lib/tls.o $(BEARSSL_OBJ)
	rm -f $@ && ar rcs $@ $^

ULDFLAGS := -T user/user.ld -z max-page-size=4096 -z noexecstack --no-warn-rwx-segments

# single-file programs
$(addprefix build/user/bin/,$(UBIN_SINGLE)): build/user/bin/%: build/user/obj/bin/%.o $(UCRT0) $(ULIBS) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(ULDFLAGS) -o $@ $(UCRT0) $< $(ULIBS) $(LIBGCC)

# multi-file programs: user/bin/NAME/*.c (+ $(UEXTRA_NAME): third-party objects linked in as well)
define UMULTI
build/user/bin/$(1): $$(patsubst user/%.c,build/user/obj/%.o,$$(wildcard user/bin/$(1)/*.c)) $$(UEXTRA_$(1)) $$(UCRT0) $$(ULIBS) user/user.ld
	@mkdir -p $$(dir $$@)
	$$(LD) $$(ULDFLAGS) -o $$@ $$(UCRT0) $$(filter-out $$(UCRT0),$$(filter %.o,$$^)) $$(ULIBS) $$(LIBGCC)
endef
# Lua 5.4 (third_party/lua, unmodified): configured through user/bin/lua/vibe_luaconf.h, force-included.
# -fexcess-precision=standard makes the x87 round doubles at every assignment like real IEEE doubles.
LUA_SRC := $(filter-out %/luac.c,$(wildcard third_party/lua/src/*.c))
UEXTRA_lua := $(patsubst %.c,build/user/obj/%.o,$(LUA_SRC))
LUA_FLAGS := -Ithird_party/lua/src -include user/bin/lua/vibe_luaconf.h
build/user/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(LUA_FLAGS) -fexcess-precision=standard -c $< -o $@
build/user/obj/bin/lua/%.o: UCFLAGS += $(LUA_FLAGS)
build/user/obj/third_party/lua/src/lua.o: UCFLAGS += -DVIBE_LUA_FRONTEND

# TinyCC 0.9.27 (third_party/tcc, unmodified): one translation unit (tcc.c #includes the rest), configured by
# user/bin/tcc/config.h, its main renamed so user/bin/tcc/vibe.c can add the static/load-address defaults.
TCC_FLAGS := -Ithird_party/tcc -Iuser/bin/tcc -Dmain=tcc_main -w
UEXTRA_tcc := build/user/obj/third_party/tcc/tcc.o
build/user/obj/third_party/tcc/%.o: third_party/tcc/%.c $(wildcard third_party/tcc/*.[ch])
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(TCC_FLAGS) -c $< -o $@
# Doom (third_party/doomgeneric, GPL-2.0, unmodified; warnings silenced per file; abi.h first so its KEY_* enums precede doomkeys.h macros) + the platform layer in user/bin/doom/
DOOM_SRC := $(wildcard third_party/doomgeneric/src/*.c)
UEXTRA_doom := $(patsubst %.c,build/user/obj/%.o,$(DOOM_SRC))
DOOM_FLAGS := -Iuser/music -Ithird_party/doomgeneric/src -Iuser/bin/doom/compat -include abi.h -DNORMALUNIX -DLINUX -DFEATURE_SOUND
$(UEXTRA_doom): build/user/obj/third_party/doomgeneric/src/%.o: third_party/doomgeneric/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(DOOM_FLAGS) -w -c $< -o $@
build/user/obj/bin/doom/%.o: UCFLAGS += $(DOOM_FLAGS)
# OPL3 music synth (user/music/ over Nuked-OPL3, LGPL-2.1, unmodified) linked into doom and midi
MUSIC_OBJ := build/user/obj/music/music.o build/user/obj/third_party/nuked-opl3/opl3.o
build/user/obj/third_party/nuked-opl3/opl3.o: third_party/nuked-opl3/opl3.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -w -c $< -o $@
build/user/obj/music/music.o: UCFLAGS += -Ithird_party/nuked-opl3
UEXTRA_doom += $(MUSIC_OBJ)
UEXTRA_midi := $(MUSIC_OBJ)
build/user/obj/bin/midi/%.o: UCFLAGS += -Iuser/music

$(foreach p,$(UBIN_MULTI),$(eval $(call UMULTI,$(p))))

user: $(UBINS)

-include $(shell find build/user -name '*.d' 2>/dev/null)

# root/ is the static skeleton; user binaries land in /bin; empty dirs aren't tracked by the VCS
# the in-OS toolchain's sdk (tcc is the compiler): headers in /usr/include, crt + libs in /usr/lib, tcc's own
# runtime + headers in /usr/lib/tcc. libtcc1.a is plain gcc output (tcc's 64-bit divide/shift helpers, alloca).
SDK_HDRS := user/lib/gui.h user/lib/ui.h user/lib/image.h common/abi.h common/gfx.h common/font.h
build/user/libtcc1.a: third_party/tcc/lib/libtcc1.c third_party/tcc/lib/alloca86.S
	$(CC) $(UCFLAGS) -fno-builtin -c third_party/tcc/lib/libtcc1.c -o build/user/libtcc1.o
	objcopy --globalize-symbol=__udivmoddi4 build/user/libtcc1.o   # libc.a (gcc-built) calls it
	$(CC) -m32 -c third_party/tcc/lib/alloca86.S -o build/user/alloca86.o
	rm -f $@ && ar rcs $@ build/user/libtcc1.o build/user/alloca86.o

build/root.ext2: $(shell find root 2>/dev/null) $(UBINS) $(ULIBS) $(UCRT0) build/user/libtcc1.a $(wildcard user/include/*.h user/include/sys/*.h third_party/tcc/include/*.h) $(SDK_HDRS) $(wildcard build/wads/freedoom1.wad $(CA_BUNDLE)) build/ca.stamp
	rm -rf build/rootfs
	@mkdir -p build/rootfs/bin build/rootfs/dev build/rootfs/proc build/rootfs/tmp build/rootfs/home
	@cp -a root/. build/rootfs/ 2>/dev/null || true
	@for f in $(UBINS); do strip -o build/rootfs/bin/$$(basename $$f) $$f; done
	@mkdir -p build/rootfs/usr/include build/rootfs/usr/lib/tcc/include
	cp -a user/include/. build/rootfs/usr/include/ && cp $(SDK_HDRS) build/rootfs/usr/include/
	cp third_party/tcc/include/*.h build/rootfs/usr/lib/tcc/include/ && cp build/user/libtcc1.a build/rootfs/usr/lib/tcc/
	cp $(UCRT0) build/rootfs/usr/lib/crt1.o && $(CC) -m32 -x c -c /dev/null -o build/rootfs/usr/lib/crti.o && cp build/rootfs/usr/lib/crti.o build/rootfs/usr/lib/crtn.o
	@for f in $(ULIBS); do strip -g -o build/rootfs/usr/lib/$$(basename $$f) $$f; done
	@mkdir -p build/rootfs/etc/ssl
	@if [ -f $(CA_BUNDLE) ]; then cp $(CA_BUNDLE) build/rootfs/etc/ssl/cert.pem; fi
	@if [ -f build/wads/freedoom1.wad ]; then mkdir -p build/rootfs/usr/share/doom; \
	  cp build/wads/freedoom1.wad build/rootfs/usr/share/doom/; cp build/wads/freedoom-COPYING.txt build/rootfs/usr/share/doom/COPYING 2>/dev/null || true; fi
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

QEMU := qemu-system-i386 -accel kvm -accel tcg -m 256 -drive file=build/vibeos.img,format=raw,if=ide \
        -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=4 -no-reboot \
        -nic user,model=e1000,hostfwd=tcp:127.0.0.1:8080-:80   # httpd in the VM -> http://localhost:8080

# PC speaker + AC'97 (/dev/dsp) through the host's PulseAudio/PipeWire
run: build/vibeos.img
	$(QEMU) -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 -device AC97,audiodev=snd0

debug: build/vibeos.img
	$(QEMU) -s -S & gdb build/kernel.elf -ex 'target remote :1234'

doom-wad:    # download + verify Freedoom into build/wads/ (then `make` puts it in /usr/share/doom)
	sh tools/getwad.sh

test-host:   # host-side unit tests (libc, image codecs)
	sh tools/imagetest/run.sh
	sh tools/libctest/run.sh

test-tls:    # HTTPS against local test servers (needs openssl)
	sh tools/tlstest/run.sh

test: build/vibeos.img
	python3 tools/vmtest.py smoke

clean:
	rm -rf build

.PHONY: all run debug test test-host test-tls clean user doom-wad FORCE
