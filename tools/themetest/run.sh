#!/bin/sh
# Host test for common/theme.c: WCAG contrast of every theme's text/background pairs (and every accent preset on each).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -g -O1 -Wall -Wextra -Wno-unused-parameter -I"$root/common" "$here/themetest.c" "$root/common/theme.c" -lm -o "$out/themetest"
"$out/themetest"
