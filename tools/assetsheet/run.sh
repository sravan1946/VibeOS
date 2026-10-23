#!/bin/sh
# Host check + contact sheet of the baked fonts, icons and wallpapers: build/assetsheet.png (and build/dunes.png).
# gfx.c uses 32-bit inline asm, so build with -m32 like the kernel.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
make -s -C "$root" build/gen/icons.c build/gen/iconall.c
gcc -m32 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$root/common" "$here/sheet.c" "$root/common/gfx.c" "$root/common/fonts.c" \
    "$root/common/termfonts.c" "$root/common/wallpaper.c" "$root/build/gen/icons.c" "$root/build/gen/appicons.c" \
    "$root/build/gen/fileicons.c" "$root/build/gen/iconall.c" -o "$out/sheet"
"$out/sheet" "$out/sheet.ppm" "$out/dunes.ppm"
for n in sheet:assetsheet dunes:dunes; do
    python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])" "$out/${n%%:*}.ppm" "$root/build/${n##*:}.png"
done
echo "wrote build/assetsheet.png build/dunes.png"
