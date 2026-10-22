#!/bin/sh
# Host test for the common/gfx.c design primitives: hairlines, per-corner radii, rings, focus ring, 9-slice shadows,
# A8 masks. Asserts on pixels and writes build/gfxtest.png (a contact sheet to eyeball). gfx.c uses 32-bit inline asm.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -m32 -g -O1 -Wall -Wextra -Wno-unused-parameter -I"$root/common" \
    "$here/gfxtest.c" "$root/common/gfx.c" "$root/common/theme.c" "$root/common/fonts.c" -o "$out/gfxtest"
"$out/gfxtest" "$out/gfxtest.ppm"
mkdir -p "$root/build"
if command -v magick >/dev/null 2>&1; then magick "$out/gfxtest.ppm" "$root/build/gfxtest.png"
else python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])" "$out/gfxtest.ppm" "$root/build/gfxtest.png"; fi
echo "wrote build/gfxtest.png"
