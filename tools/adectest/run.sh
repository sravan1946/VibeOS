#!/bin/sh
# Host test for user/music/adec.c (WAV / MP3 via minimp3 / Ogg via stb_vorbis) against ffmpeg's decoders, under ASan+UBSan.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -I"$root/user/music" \
    -c "$root/user/music/adec.c" -I"$root/third_party/minimp3" -I"$root/third_party/stb_vorbis" -o "$out/adec.o"
gcc -O2 -w -I"$root/third_party/minimp3" -c "$root/user/music/mp3.c" -o "$out/mp3.o"
gcc -O2 -w -I"$root/third_party/stb_vorbis" -DSTB_VORBIS_NO_STDIO -DSTB_VORBIS_NO_PUSHDATA_API -DSTB_VORBIS_NO_FAST_SCALED_FLOAT \
    -c "$root/user/music/vorbis.c" -o "$out/vorbis.o"
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -I"$root/user/music" "$here/adectest.c" \
    "$out/adec.o" "$out/mp3.o" "$out/vorbis.o" -lm -o "$out/adectest"
python3 -I "$here/test.py" "$out/adectest" "$root/root/home/Music"
