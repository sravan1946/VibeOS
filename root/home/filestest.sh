#!/bin/sh
# filestest.sh - regression test for file associations (`open -w`) and the trash (`trash`), run inside VibeOS.
# Run: sh /home/filestest.sh   (prints PASS/FAIL per check, then a summary)
pass=0
fail=0
t() {   # t name expected actual
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); echo "PASS $1"
    else
        fail=$((fail + 1)); echo "FAIL $1: expected [$2] got [$3]"
    fi
}
T=/tmp/filestest.$$
mkdir -p $T/d/sub
cd $T
echo hello > d/a.txt; echo x > d/sub/b.txt; echo y > d/pic.png; echo z > d/song.wav; echo m > d/t.mid
echo '<p>' > d/p.html; echo 'int x;' > d/m.c; echo q > d/blob.xyz

# ---- associations (open.conf: the file argument picks the program)
t assoc-txt /bin/notepad "$(open -w d/a.txt)"
t assoc-c /bin/code "$(open -w d/m.c)"
t assoc-png /bin/viewer "$(open -w d/pic.png)"
t assoc-wav /bin/music "$(open -w d/song.wav)"
t assoc-mid /bin/midi "$(open -w d/t.mid)"
t assoc-html /bin/browser "$(open -w d/p.html)"
t assoc-unknown /bin/notepad "$(open -w d/blob.xyz)"
t assoc-dir /bin/files "$(open -w d)"
t assoc-exe "/bin/ls" "$(open -w /bin/ls)"
open -w d/missing 2>/dev/null; t assoc-missing 1 $?

# ---- trash
TD=/home/.trash
trash -e
t trash-empty-list "" "$(trash -l)"
trash d/a.txt d/sub
t trash-moved-file no "$([ -e d/a.txt ] && echo yes || echo no)"
t trash-has-file yes "$([ -e $TD/files/a.txt ] && echo yes || echo no)"
t trash-has-dir-contents yes "$([ -e $TD/files/sub/b.txt ] && echo yes || echo no)"
t trash-info "$T/d/a.txt" "$(head -1 $TD/info/a.txt)"
echo again > d/a.txt
trash d/a.txt
t trash-collision yes "$([ -e "$TD/files/a (2).txt" ] && echo yes || echo no)"
t trash-list 3 "$(trash -l | wc -l | tr -d ' ')"
trash -r a.txt > /dev/null
t restore-file hello "$(cat d/a.txt)"
rmdir d/sub 2>/dev/null
trash -r sub > /dev/null
t restore-dir x "$(cat d/sub/b.txt)"
trash -r "a (2).txt" > /dev/null
t restore-conflict again "$(cat "d/a (2).txt")"
trash d/pic.png
trash -e
t empty-files "" "$(ls $TD/files)"
t empty-info "" "$(ls $TD/info)"
trash /nonexistent 2>/dev/null; t trash-missing 1 $?

# ---- df (statfs): the Avail column follows writes and deletes
avail() { df -k "${1:-$T}" | awk 'NR == 2 { print $4 }'; }
a0=$(avail)
seq 1 40000 > d/big.txt                       # ~ 218 KB
a1=$(avail)
t df-avail-drops yes "$([ $((a0 - a1)) -ge 200 ] && [ $((a0 - a1)) -le 260 ] && echo yes || echo no)"
rm d/big.txt
t df-avail-returns "$a0" "$(avail)"
t df-header "Filesystem" "$(df | awk 'NR == 1 { print $1 }')"
t df-root-row ext2 "$(df / | awk 'NR == 2 { print $1 }')"
t df-mount-col / "$(df / | awk 'NR == 2 { print $NF }')"
t df-dev-row devfs "$(df /dev | awk 'NR == 2 { print $1 }')"
t df-default-only-real 2 "$(df | wc -l)"
t df-h-unit yes "$(df -h / | awk 'NR == 2 && $2 ~ /[MG]$/ { print "yes" }')"
df /nonexistent >/dev/null 2>&1; t df-missing 1 $?
df -z >/dev/null 2>&1; t df-usage 1 $?
# ---- what Files' Undo relies on: trash / restore round trips, emptying a nested tree (fs_trash_empty_x)
mkdir -p d/n/m; echo deep > d/n/m/f
trash d/n
t undo-trash-gone no "$([ -e d/n ] && echo yes || echo no)"
trash -r n > /dev/null
t undo-trash-back deep "$(cat d/n/m/f)"
trash d/n
t undo-restore-retrash yes "$([ -e $TD/files/n/m/f ] && echo yes || echo no)"
trash -e
t empty-nested-files "" "$(ls $TD/files)"
t empty-nested-info "" "$(ls $TD/info)"

rm -rf $T
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
