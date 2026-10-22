#!/bin/sh
# Builds user/bin/make.c for the host and diffs its behaviour against GNU make on the corpus in maketest.py.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/maketest
gcc -DMAKE_HOST -O2 -Wall -Wextra -o build/maketest/make user/bin/make.c
echo "== make vs GNU make"
python3 tools/maketest/maketest.py build/maketest/make
