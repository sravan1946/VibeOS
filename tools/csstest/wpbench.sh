#!/bin/sh
# Realistic css_compute timing: fetches en.wikipedia.org/wiki/Operating_system and its stylesheets (needs curl and network),
# builds the DOM with a mini parser and styles every element. Not part of make test-host (network).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=${TMPDIR:-/tmp}/wpbench.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
curl -sL -A Mozilla/5.0 https://en.wikipedia.org/wiki/Operating_system -o "$out/page.html"
css=""
i=0
for u in $(grep -o 'href="/w/load.php[^"]*"' "$out/page.html" | sed 's/href="//;s/"$//;s/&amp;/\&/g' | sort -u); do
    i=$((i+1)); curl -sL -A Mozilla/5.0 "https://en.wikipedia.org$u" -o "$out/s$i.css"; css="$css $out/s$i.css"
done
gcc -g -O2 -Wall -Wno-unused-parameter -I"$here/stub" -I"$root/user/bin/browser" "$here/wpbench.c" "$root/user/bin/browser/css.c" "$root/user/bin/browser/cssel.c" -lm -o "$out/wpbench"
"$out/wpbench" "$out/page.html" $css
