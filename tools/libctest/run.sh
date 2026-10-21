#!/bin/sh
# Builds the portable parts of user/lib as a 32-bit freestanding Linux program and runs it,
# plus a 64-bit differential test of the printf engine against the host glibc. Non-zero on failure.
set -e
cd "$(dirname "$0")/../.."
T=build/libctest
mkdir -p $T
GCCINC=$(gcc -print-file-name=include)
LIBGCC=$(gcc -m32 -print-libgcc-file-name)
FLAGS="-m32 -march=i686 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
  -nostdinc -isystem $GCCINC -Iuser/include -Icommon -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format"
OBJS=""
for f in string errno ctype malloc stdlib printf stdio scanf math time; do
    X=""
    [ $f = string ] && X=-fno-tree-loop-distribute-patterns
    gcc $FLAGS $X -c user/lib/$f.c -o $T/$f.o
    OBJS="$OBJS $T/$f.o"
done
nasm -f elf32 user/lib/setjmp.asm -o $T/setjmp.o
gcc $FLAGS -c tools/libctest/stubs.c -o $T/stubs.o
gcc $FLAGS -c tools/libctest/libctest.c -o $T/libctest.o
ld -m elf_i386 --no-warn-rwx-segments -e _start -o $T/libctest $T/libctest.o $T/stubs.o $OBJS $T/setjmp.o $LIBGCC
echo "== 32-bit libc test"
(cd $T && ./libctest)

echo "== printf differential test vs glibc (64-bit build of user/lib/printf.c)"
gcc -O2 -ffreestanding -fno-builtin -nostdinc -isystem $GCCINC -Iuser/include -Icommon \
    -Dvsnprintf=vibe_vsnprintf -Dsnprintf=vibe_snprintf -Dvsprintf=vibe_vsprintf -Dsprintf=vibe_sprintf \
    -Dasprintf=vibe_asprintf -Dvasprintf=vibe_vasprintf -w -c user/lib/printf.c -o $T/printf64.o
gcc -O2 -Wall tools/libctest/printf_diff.c $T/printf64.o -o $T/printf_diff -lm
$T/printf_diff
