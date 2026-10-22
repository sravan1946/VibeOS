#!/bin/sh
# Host test for user/lib/image.c (zlib, PNG/BMP/PPM/JPEG/GIF decode, PNG encode) against Pillow, under ASan+UBSan.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
    -I"$root/user/lib" "$here/imagetest.c" "$root/user/lib/image.c" "$root/user/lib/jpeg.c" "$root/user/lib/gif.c" -lm -o "$out/imagetest"
python3 "$here/test.py" "$out/imagetest"
