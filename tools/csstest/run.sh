#!/bin/sh
# Host test for the browser's CSS engine (user/bin/browser/css.c, cssel.c): colours, selectors, cascade, var()/calc(), @media/@supports/@layer.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -g -O2 ${SAN:+-fsanitize=address,undefined} -Wall -Wextra -Wno-unused-parameter -I"$here/stub" -I"$root/user/bin/browser" \
    "$here/csstest.c" "$root/user/bin/browser/css.c" "$root/user/bin/browser/cssel.c" -lm -o "$out/csstest"
"$out/csstest" "$@"
