#!/bin/sh
# url.c (the browser's URL resolver) compiled for the host and checked against RFC 3986 section 5.4
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
gcc -O1 -w -Iuser/lib -Icommon -Iuser/bin/browser -include stdlib.h -include string.h \
    -o build/urltest tools/urltest/urltest.c user/bin/browser/url.c
build/urltest
