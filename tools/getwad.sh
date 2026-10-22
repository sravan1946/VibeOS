#!/bin/sh
# getwad.sh - fetch Freedoom (BSD) and cache freedoom1.wad in build/wads/ (make doom-wad)
set -e
VER=0.13.0
URL=https://github.com/freedoom/freedoom/releases/download/v$VER/freedoom-$VER.zip
ZIP_SHA=3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
OUT=${1:-build/wads}
[ -f "$OUT/freedoom1.wad" ] && { echo "$OUT/freedoom1.wad already there"; exit 0; }
mkdir -p "$OUT"
tmp=$(mktemp -d "$OUT/.dl.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
echo "downloading $URL"
curl -fL --progress-bar -o "$tmp/fd.zip" "$URL"
echo "$ZIP_SHA  $tmp/fd.zip" | sha256sum -c - || { echo "getwad: checksum mismatch, refusing to use it" >&2; exit 1; }
unzip -q -j -o "$tmp/fd.zip" "freedoom-$VER/freedoom1.wad" "freedoom-$VER/COPYING.txt" -d "$tmp"
mv "$tmp/COPYING.txt" "$OUT/freedoom-COPYING.txt"
mv "$tmp/freedoom1.wad" "$OUT/freedoom1.wad"
echo "ok: $OUT/freedoom1.wad"
