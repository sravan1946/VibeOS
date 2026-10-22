#!/bin/sh
# Host test of user/lib/fsutil.c: associations, trash, copy/delete helpers. Non-zero on failure.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/filestest
gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -DFS_HOST -Iuser/lib \
    tools/filestest/filestest.c user/lib/fsutil.c -o build/filestest/filestest
echo "== files: associations + trash (host)"
build/filestest/filestest
