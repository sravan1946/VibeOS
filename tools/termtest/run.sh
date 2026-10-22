#!/bin/sh
# Host test for common/term.c: asserts on the cell grid, renders tools/termtest/showcase.png.
# gfx.c uses 32-bit inline asm, so build with -m32 like the kernel.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -m32 -g -O1 -Wall -Wextra -Wno-unused-parameter -I"$root/common" \
    "$here/termtest.c" "$root/common/term.c" "$root/common/theme.c" "$root/common/gfx.c" "$root/common/fonts.c" "$root/common/termfonts.c" -o "$out/termtest"
"$out/termtest" "$out"
for n in showcase boxes; do
    if command -v magick >/dev/null 2>&1; then magick "$out/$n.ppm" "$here/$n.png"
    else python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])" "$out/$n.ppm" "$here/$n.png"; fi
done
echo "wrote $here/showcase.png $here/boxes.png"
