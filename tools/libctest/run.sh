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
for f in string errno ctype malloc stdlib strtod locale printf stdio scanf math time regex; do
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

echo "== strtod differential test vs glibc (64-bit build of user/lib/strtod.c)"
gcc -O2 -ffreestanding -fno-builtin -nostdinc -isystem $GCCINC -Iuser/include -Icommon \
    -Dstrtod=vibe_strtod -Dstrtof=vibe_strtof -Datof=vibe_atof -Derrno=vibe_errno -w -c user/lib/strtod.c -o $T/strtod64.o
gcc -O2 -Wall tools/libctest/strtod_diff.c $T/strtod64.o -o $T/strtod_diff -lm
$T/strtod_diff

echo "== libm differential test vs glibc (64-bit build of user/lib/math.c)"
R=""
for n in sin cos tan asin acos atan atan2 sqrt pow exp log log2 log10 floor ceil fabs fmod round trunc sinh cosh tanh hypot \
         frexp ldexp modf sinf cosf sqrtf fabsf floorf ceilf powf atan2f fmodf roundf; do R="$R -D$n=vibe_$n"; done
gcc -O2 -ffreestanding -fno-builtin -nostdinc -isystem $GCCINC -Iuser/include -Icommon $R -w -c user/lib/math.c -o $T/math64.o
gcc -O2 -Wall tools/libctest/math_diff.c $T/math64.o -o $T/math_diff -lm
$T/math_diff

echo "== regex differential test vs glibc (64-bit build of user/lib/regex.c)"
gcc -O2 -ffreestanding -fno-builtin -nostdinc -isystem $GCCINC -Iuser/include -Icommon \
    -Dregcomp=vibe_regcomp -Dregexec=vibe_regexec -Dregfree=vibe_regfree -Dregerror=vibe_regerror -w -c user/lib/regex.c -o $T/regex64.o
gcc -O2 -Wall tools/libctest/regex_diff.c $T/regex64.o -o $T/regex_diff
$T/regex_diff
