#!/bin/sh
# Host perft + rules test of user/bin/chess/engine.c. Non-zero on failure.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/chesstest
gcc -O2 -Wall -Wextra -o build/chesstest/perft tools/chesstest/perft.c user/bin/chess/engine.c
echo "== chess move generator (perft)"
build/chesstest/perft
