#!/bin/bash
# Host test for kernel/fs/ext2.c: mutate an mke2fs image through the vnode ops,
# then e2fsck -fn it and compare written files via debugfs.  Non-zero exit on any failure.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT

gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -include "$here/shim.h" -I"$root/kernel/include" -I"$root/common" \
    -o "$w/t" "$here/test.c" "$root/kernel/fs/ext2.c"

# source tree: nested dirs, a >300 KiB file (indirect blocks), an empty file
s=$w/src
mkdir -p "$s/sub/deep" "$s/emptydir"
echo "hello from the host" > "$s/small.txt"
: > "$s/empty.txt"
head -c 327680 /dev/urandom > "$s/big300k.bin"
head -c 51200 /dev/urandom > "$s/sub/deep/b.bin"
seq 1 5000 > "$s/sub/a.txt"
head -c 1500000 /dev/urandom > "$s/sub/deep/mid.bin"

fail=0
for bs in 1024 2048 4096; do
    echo "=== block size $bs ==="
    img=$w/img$bs; out=$w/out$bs; mkdir -p "$out"
    rm -f "$img"
    mke2fs -q -t ext2 -b $bs -I 256 -d "$s" "$img" 100M
    "$w/t" "$img" "$s" "$out" $bs || { echo "FAIL: test program (bs=$bs)"; fail=1; continue; }
    if e2fsck -fn "$img" > "$out/fsck.txt" 2>&1; then echo "e2fsck: clean"
    else echo "FAIL: e2fsck (bs=$bs)"; cat "$out/fsck.txt"; fail=1; fi
    while read -r kind idx path; do
        if [ "$kind" = F ]; then
            debugfs -R "cat /$path" "$img" 2>/dev/null | cmp -s - "$out/$idx" || { echo "FAIL: debugfs content /$path"; fail=1; }
        else
            debugfs -R "stat /$path" "$img" 2>&1 | grep -q "not found" || { echo "FAIL: /$path should be gone"; fail=1; }
        fi
    done < "$out/manifest.txt"
    echo "debugfs: $(wc -l < "$out/manifest.txt") paths checked"
done
if [ $fail = 0 ]; then echo "ALL PASSED"; else echo "SOME FAILED"; exit 1; fi
