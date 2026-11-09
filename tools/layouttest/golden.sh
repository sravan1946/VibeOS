#!/bin/sh
# Golden layout checks: lay the local test pages out with the browser's real engine (host build, tools/layouttest) and compare
# the box tree dump (every box: kind, position, size) with tools/layouttest/golden/*.txt. `golden.sh --update` rewrites them.
# A diff means a layout change: look at it (render with run.sh), then update if it is intended.
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$here/../..; b=$root/user/bin/browser
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
gcc -m32 -O1 -w -I$root/user/lib -I$root/common -I$b -DLT_DEBUG \
  $here/render.c $b/layout.c $b/dom.c $b/html.c $b/css.c $b/cssel.c $b/entities.c $b/url.c $b/form.c $b/ttf.c -I$root/third_party/stb_truetype \
  $root/common/gfx.c $root/common/fonts.c $root/common/theme.c $root/common/fontsw.c -lm -o $out/render
export VIBE_FONTDIR=$root/root/usr/share/vibeos/fonts
fail=0; n=0
for spec in Documents/test.html:900 Documents/css.html:900 csstest/tables.html:900 csstest/layout.html:700 csstest/layout.html:420 csstest/layout2.html:600 Documents/svgicons.html:700 csstest/flexitems.html:400 csstest/gradients.html:700 csstest/gridcalc.html:700; do
    f=${spec%%:*}; w=${spec##*:}; name=$(echo "$f" | tr '/' '_')_$w
    LT_DUMP=1 $out/render $root/root/home/$f $w $out/x.ppm 50 2>$out/dump.txt >/dev/null
    grep -v '^layout ' $out/dump.txt > $out/cur.txt
    if [ "$1" = "--update" ]; then mkdir -p $here/golden; cp $out/cur.txt $here/golden/$name.txt; continue; fi
    n=$((n + 1))
    if ! diff -q $here/golden/$name.txt $out/cur.txt >/dev/null 2>&1; then
        echo "FAIL $name"; diff $here/golden/$name.txt $out/cur.txt | head -8; fail=$((fail + 1))
    fi
done
[ "$1" = "--update" ] && { echo "golden files updated"; exit 0; }
echo "layouttest: $n pages, $fail failed"
[ $fail = 0 ]
