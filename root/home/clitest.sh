#!/bin/sh
# clitest.sh - tar, gzip, du and friends.  Run: sh /home/clitest.sh   (summary line last)
pass=0
fail=0
t() { if [ "$2" = "$3" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $1: expected [$2] got [$3]"; fi; }
T=/tmp/clitest.$$
rm -rf $T; mkdir -p $T/src/sub/deep $T/out
cd $T
echo hello > src/a.txt
seq 1 5000 > src/sub/nums.txt
echo deep > src/sub/deep/d.txt
: > src/empty
head -c 30000 /bin/ls > src/bin.dat
L=src/sub/deep/this_is_a_rather_long_directory_name_to_force_the_ustar_prefix_field_to_be_used_by_the_archive_writer_ok
mkdir -p $L; echo long > $L/file_with_a_name_that_is_also_fairly_long_but_still_below_the_limit.txt
BIN=$(md5sum < src/bin.dat)

# gzip round trips
gzip -k src/sub/nums.txt
t gz-exists 0 "$(test -f src/sub/nums.txt.gz; echo $?)"
t gz-smaller yes "$(if [ $(wc -c < src/sub/nums.txt.gz) -lt $(wc -c < src/sub/nums.txt) ]; then echo yes; else echo no; fi)"
t zcat "$(md5sum < src/sub/nums.txt)" "$(zcat src/sub/nums.txt.gz | md5sum)"
gunzip -c src/sub/nums.txt.gz > out/n2
t gunzip-c "$(md5sum < src/sub/nums.txt)" "$(md5sum < out/n2)"
rm src/sub/nums.txt; gunzip src/sub/nums.txt.gz
t gunzip-restores 5000 "$(wc -l < src/sub/nums.txt)"
gzip src/bin.dat
t gz-removes-src no "$(if [ -f src/bin.dat ]; then echo yes; else echo no; fi)"
gunzip src/bin.dat.gz
t gz-binary "$BIN" "$(md5sum < src/bin.dat)"
echo junk | gunzip > /dev/null 2>&1
t gunzip-rejects-junk 1 $?
gzip < src/empty > out/e.gz
t gz-empty 0 "$(gunzip < out/e.gz | wc -c)"

# tar
tar cf out/a.tar src
t tar-list 1 "$(tar tf out/a.tar | grep -c "^src/a.txt$")"
mkdir out/x; tar xf out/a.tar -C out/x
t tar-roundtrip-a hello "$(cat out/x/src/a.txt)"
t tar-roundtrip-nums "$(md5sum < src/sub/nums.txt)" "$(md5sum < out/x/src/sub/nums.txt)"
t tar-roundtrip-bin "$BIN" "$(md5sum < out/x/src/bin.dat)"
t tar-empty-file 0 "$(wc -c < out/x/src/empty)"
t tar-long-names long "$(cat out/x/$L/file_with_a_name_that_is_also_fairly_long_but_still_below_the_limit.txt)"
tar czf out/b.tgz src
tar tzf out/b.tgz > out/list
t tgz-list "$(tar tf out/a.tar | wc -l)" "$(wc -l < out/list)"
mkdir out/y; tar xf out/b.tgz -C out/y
t tgz-roundtrip "$(md5sum < src/sub/nums.txt)" "$(md5sum < out/y/src/sub/nums.txt)"
mkdir out/z; tar cf - src/a.txt | tar xf - -C out/z
t tar-stdio hello "$(cat out/z/src/a.txt)"
mkdir out/w; tar xf out/a.tar src/sub/deep/d.txt -C out/w
t tar-select deep "$(cat out/w/src/sub/deep/d.txt)"

# du
mkdir du1; head -c 20000 /bin/ls > du1/f
t du-summary 1 "$(du -s du1 | grep -c '^[0-9]*	du1$')"
t du-kb-sane yes "$(if [ $(du -s du1 | cut -f1) -ge 20 ]; then echo yes; else echo no; fi)"
t du-a 1 "$(du -a du1 | grep -c 'du1/f$')"

cd /; rm -rf $T
echo "$pass passed, $fail failed"
[ $fail = 0 ]
