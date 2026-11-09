#!/bin/sh
# usage: run.sh FILE.html WIDTH OUT.png [HEIGHT] [BASEURL]   host build of the browser's layout engine; renders the display list.
# External CSS: $LT_SHEETS/<url with non-alnum -> _> (tools/layouttest/fetchsite.py fills it).
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$here/../..; b=$root/user/bin/browser
out=${LT_BUILD:-/tmp/layouttest}; mkdir -p $out
gcc -m32 -g -O1 -w -I$root/user/lib -I$root/common -I$b -DLT_DEBUG \
  $here/render.c $b/layout.c $b/dom.c $b/html.c $b/css.c $b/cssel.c $b/entities.c $b/url.c $b/form.c $b/ttf.c -I$root/third_party/stb_truetype \
  $root/common/gfx.c $root/common/fonts.c $root/common/theme.c $root/common/fontsw.c -lm -o $out/render 2>$out/build.log || { head -40 $out/build.log; exit 1; }
export VIBE_FONTDIR=${VIBE_FONTDIR:-$root/root/usr/share/vibeos/fonts}
f=$1; w=$2; o=$3; shift 3
$out/render $f $w $out/o.ppm "$@"
if command -v magick >/dev/null 2>&1; then magick $out/o.ppm $o; else python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])" $out/o.ppm $o; fi
