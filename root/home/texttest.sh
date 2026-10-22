#!/bin/sh
# texttest.sh - regression test for the text tools (grep sed tr cut xargs cmp diff tac rev nl paste, awk).
# Run: sh /home/texttest.sh   (prints PASS/FAIL per check, then a summary)
pass=0
fail=0
t() {   # t name expected actual
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); echo "PASS $1"
    else
        fail=$((fail + 1)); echo "FAIL $1: expected [$2] got [$3]"
    fi
}
TMP=/tmp/texttest.$$
mkdir -p $TMP
cd $TMP
printf 'foo bar\nbaz\nFoo\nfoobar\nx\ny\nfoo\nthe cat\nconcat\n\nabc123\n' > g1
printf 'foo\nqux\n' > g2
mkdir -p gd/sub
cp g1 gd/a; cp g2 gd/sub/b

# ---- grep
t grep-basic "foo bar
foobar
foo" "$(grep foo g1)"
t grep-n "1:foo bar
4:foobar
7:foo" "$(grep -n foo g1)"
t grep-c 4 "$(grep -c -i foo g1)"
t grep-v 8 "$(grep -vc foo g1)"
t grep-l "g1" "$(grep -l foo g1 g2 | head -1)"
t grep-L "g1" "$(grep -L qux g1 g2)"
t grep-H "g1:foo" "$(grep -H '^foo$' g1)"
t grep-h "foo
foo" "$(grep -h '^foo$' g1 g2)"
t grep-multi-name "g2:foo" "$(grep '^foo$' g1 g2 | tail -1)"
t grep-w "the cat" "$(grep -w cat g1)"
t grep-x "g1:foo
g2:foo" "$(grep -x foo g1 g2)"
t grep-o "oo
oo
oo" "$(grep -o 'o\+' g1 | head -3)"
t grep-on "1:oo" "$(grep -on 'o\+' g1 | head -1)"
t grep-E "foo bar
baz
foobar
foo" "$(grep -E 'foo|baz' g1)"
t grep-E-o "abc123" "$(grep -E -o '[a-z]+[0-9]+' g1)"
t grep-F-dot "" "$(grep -F 'o.b' g1)"
t grep-F "x
y" "$(grep -F -e x -e y g1)"
t grep-e-multi "foo bar
baz" "$(grep -e 'foo b' -e baz g1)"
grep -q foo g1; t grep-q-hit 0 $?
grep -q zzz g1; t grep-q-miss 1 $?
grep foo nonexist 2> /dev/null; t grep-nofile 2 $?
t grep-A "baz
Foo" "$(grep -A1 baz g1)"
t grep-B "foo bar
baz" "$(grep -B1 baz g1)"
t grep-C "4-foobar
5:x
6-y" "$(grep -n -C1 x g1)"
t grep-context-sep "foo bar
baz
--
x
y" "$(grep -A1 -e 'foo b' -e '^x' g1 | head -5)"
t grep-r "gd/a:foo
gd/sub/b:foo" "$(grep -r '^foo$' gd | sort)"
t grep-rl "gd/a
gd/sub/b" "$(grep -rl '^foo$' gd | sort)"
t grep-anchors "foo bar" "$(grep '^foo b.r$' g1)"
t grep-bre-interval "Foo
foo" "$(grep 'o\{2\}$' g1)"
t grep-ere-backref "foo bar" "$(grep -E '(o)\1 ' g1)"
t grep-icase-w "foo" "$(grep -iw FOO g2)"
t grep-empty-lines 1 "$(grep -c '^$' g1)"
t grep-pipe "the cat" "$(cat g1 | grep -w 'cat')"
t grep-class "abc123" "$(grep '[[:digit:]]' g1)"
t grep-options-after "baz" "$(grep baz g1 -h)"

# ---- sed
printf 'one\ntwo\nthree\nfour\nfive\n' > s1
t sed-s "0ne
two" "$(sed 's/o/0/;3,$d;2s/0/o/' s1 | head -2)"
t sed-g "f00 b0r" "$(echo 'foo bor' | sed 's/o/0/g;s/ b0/ b0/')"
t sed-nth "foXoo" "$(echo foooo | sed 's/o/X/2')"
t sed-nth-g "fo000" "$(echo foooo | sed 's/o/0/2g')"
t sed-n-p "two
four" "$(sed -n 'n;p' s1 | head -2)"
t sed-d "one
three
five" "$(sed '2d;4d' s1)"
t sed-range "two
three
four" "$(sed -n '2,4p' s1)"
t sed-last "five" "$(sed -n '$p' s1)"
t sed-re-addr "three" "$(sed -n '/thr/p' s1)"
t sed-re-range "two
three" "$(sed -n '/two/,/three/p' s1)"
t sed-neg "two
three
four" "$(sed '2,4!d' s1)"
t sed-neg-n "one
five" "$(sed -n '2,4!p' s1)"
t sed-step "one
three
five" "$(sed -n '1~2p' s1)"
t sed-plus "two
three" "$(sed -n '2,+1p' s1)"
t sed-q "one
two" "$(sed 2q s1)"
t sed-q-status 5 "$(sed 1q5 s1 > /dev/null; echo $?)"
t sed-e-multi "1 2 three" "$(sed -e 's/one/1/' -e 's/two/2/' s1 | head -3 | paste -s -d ' ' | sed 's/ four//')"
t sed-groups "bar foo" "$(echo 'foo bar' | sed 's/\(foo\) \(bar\)/\2 \1/')"
t sed-ere "bar foo" "$(echo 'foo bar' | sed -E 's/(foo) (bar)/\2 \1/')"
t sed-amp "[foo]" "$(echo foo | sed 's/.*/[&]/')"
t sed-empty-g "-a-b-c-" "$(echo abc | sed 's/x*/-/g')"
t sed-icase "X bar" "$(echo 'FOO bar' | sed 's/foo/X/I')"
t sed-y "xyz" "$(echo abc | sed 'y/abc/xyz/')"
t sed-p-flag "foo
foo" "$(echo foo | sed 's/o/o/p')"
t sed-delim "a|b" "$(echo a/b | sed 's|/|\||')"
t sed-newline "a
b" "$(echo aXb | sed 's/X/\n/')"
t sed-a "one
added
two" "$(sed -n '1{p;a\
added
};2p' s1)"
t sed-a-gnu "one
hi" "$(sed '1a hi' s1 | head -2)"
t sed-i-cmd "hi
one" "$(sed '1i hi' s1 | head -2)"
t sed-c "one
CHG
three" "$(sed '2c CHG' s1 | head -3)"
t sed-c-range "one
X
five" "$(sed '2,4c\
X' s1)"
t sed-eq "5" "$(sed -n '$=' s1)"
t sed-N "one two" "$(sed 'N;s/\n/ /;q' s1)"
t sed-join "one,two,three,four,five" "$(sed ':a;N;$!ba;s/\n/,/g' s1)"
t sed-hold-rev "five
four
three
two
one" "$(sed -n '1!G;h;$p' s1)"
t sed-x-g "two
one" "$(printf 'one\ntwo\n' | sed -n 'x;n;p;x;p' | head -2 | sed -n '1p;2p')"
t sed-block "ONE" "$(sed -n '1{s/one/ONE/;p}' s1)"
t sed-branch "AZ" "$(echo A | sed 's/A/A/;t end;s/A/B/;: end;s/$/Z/')"
t sed-case "FOO bar" "$(echo 'foo bar' | sed -E 's/(foo)/\U\1/')"
t sed-tab-class "x" "$(printf ' \t x\n' | sed 's/^[ \t]*//')"
cp s1 s3
sed -i 's/one/ONE/' s3
t sed-inplace "ONE" "$(head -1 s3)"
sed -i.bak '1d' s3
t sed-inplace-bak "ONE" "$(head -1 s3.bak)"
t sed-inplace-del "two" "$(head -1 s3)"
printf 'a\nb' > s4
sed 's/a/A/' s4 > s5
t sed-no-eol "A
b" "$(cat s5)"
t sed-no-eol-bytes 3 "$(wc -c < s5 | sed 's/ //g;s/^3$/3/')"
t sed-stdin "ok" "$(echo no | sed 's/no/ok/')"
sed -n 'p' s1 nonexist > /dev/null 2>&1; t sed-missing-file 2 $?
sed 'k' s1 > /dev/null 2>&1; t sed-bad-cmd 1 $?
printf 's/o/0/\ns/e/3/\n' > sc
t sed-f "0n3" "$(echo one | sed -f sc)"

# ---- tr
t tr-upper "HELLO" "$(echo hello | tr a-z A-Z)"
t tr-class "HELLO" "$(echo hello | tr '[:lower:]' '[:upper:]')"
t tr-d "hll" "$(echo hello | tr -d 'aeiou')"
t tr-s "a b" "$(echo 'a   b' | tr -s ' ')"
t tr-cd "hello" "$(echo 'h1e2l3l4o5' | tr -cd '[:alpha:]')"
t tr-c "a_b__" "$(echo 'a b ' | tr -c 'a-z' '_' | tr -d '\n')"
t tr-cs "one
two" "$(echo 'one, two' | tr -cs 'a-z' '\n')"
t tr-pad "xyy" "$(echo abc | tr abc xy)"
t tr-newline "a b" "$(printf 'a\nb\n' | tr '\n' ' ' | sed 's/ $//')"
t tr-octal "H" "$(echo h | tr '\150' H)"
t tr-ds "b" "$(echo 'aabb' | tr -ds 'a' 'b')"
t tr-range-digits "XXX" "$(echo 123 | tr 0-9 X)"
tr -z 2> /dev/null; t tr-bad-opt 2 $?

# ---- cut
printf 'a:b:c:d\nxx:yy:zz\nnocolon\n' > c1
t cut-f "a
xx
nocolon" "$(cut -d: -f1 c1)"
t cut-f-list "b:c
yy:zz
nocolon" "$(cut -d: -f2,3 c1)"
t cut-f-open "c:d
zz
nocolon" "$(cut -d: -f3- c1)"
t cut-f-s "b
yy" "$(cut -d: -s -f2 c1)"
t cut-c "abc" "$(echo abcdef | cut -c1-3)"
t cut-c-multi "acf" "$(echo abcdef | cut -c1,3,6)"
t cut-c-open "def" "$(echo abcdef | cut -c4-)"
t cut-c-head "ab" "$(echo abcdef | cut -c-2)"
t cut-tab "b" "$(printf 'a\tb\tc\n' | cut -f2)"
t cut-b "bcd" "$(echo abcde | cut -b2-4)"
cut -f 2>/dev/null; t cut-missing-list 2 $?

# ---- xargs
t xargs-default "a b c" "$(echo a b c | xargs)"
t xargs-cmd "X a b" "$(echo a b | xargs echo X)"
t xargs-n "1 2
3 4
5" "$(echo 1 2 3 4 5 | xargs -n2 echo)"
t xargs-I "[a]
[b]" "$(printf 'a\nb\n' | xargs -I{} echo '[{}]')"
t xargs-I-multi "1-1" "$(echo 1 | xargs -I % echo %-%)"
t xargs-quotes "a b
c" "$(echo "'a b' c" | xargs -n1 echo)"
t xargs-0 "a b
c" "$(printf 'a b\0c\0' | xargs -0 -n1 echo)"
t xargs-r "" "$(echo | xargs -r echo hi)"
t xargs-empty-runs "hi" "$(echo | xargs echo hi)"
t xargs-status 123 "$(echo x | xargs false; echo $?)"
t xargs-pipe-wc "3" "$(echo a b c | xargs -n1 echo | wc -l | sed 's/ //g')"
t xargs-t "echo x" "$(echo x | xargs -t echo 2>&1 > /dev/null)"

# ---- cmp
printf 'abc\ndef\n' > m1; printf 'abc\ndeg\n' > m2; printf 'abc\n' > m3
cmp m1 m1; t cmp-same 0 $?
cmp m1 m2 > /dev/null; t cmp-diff-status 1 $?
t cmp-msg "m1 m2 differ: byte 7, line 2" "$(cmp m1 m2)"
cmp -s m1 m2; t cmp-s 1 $?
t cmp-l "7 146 147" "$(cmp -l m1 m2)"
t cmp-eof "cmp: EOF on m3 after byte 4, line 1" "$(cmp m1 m3 2>&1)"
cmp m1 nonexist 2> /dev/null; t cmp-missing 2 $?

# ---- diff
printf 'a\nb\nc\nd\ne\nf\ng\nh\n' > d1
printf 'a\nb\nX\nd\ne\nf\ng\nh\ni\n' > d2
diff d1 d1; t diff-same 0 $?
diff d1 d2 > /dev/null; t diff-status 1 $?
t diff-normal "3c3
< c
---
> X
8a9
> i" "$(diff d1 d2)"
t diff-delete "2d1
< b" "$(printf 'a\nb\n' > e1; printf 'a\n' > e2; diff e1 e2)"
t diff-add-range "1a2,3
> y
> z" "$(printf 'a\ny\nz\n' > e3; diff e2 e3)"
t diff-u "@@ -1,8 +1,9 @@
 a
 b
-c
+X
 d
 e
 f
 g
 h
+i" "$(diff -u d1 d2 | tail -n +3)"
t diff-U0 "@@ -3 +3 @@
-c
+X
@@ -8,0 +9 @@
+i" "$(diff -U0 d1 d2 | tail -n +3)"
t diff-u-header "--- d1
+++ d2" "$(diff -u d1 d2 | head -2)"
t diff-q "Files d1 and d2 differ" "$(diff -q d1 d2)"
t diff-i "" "$(printf 'A\nb\n' > i1; printf 'a\nB\n' > i2; diff -i i1 i2)"
t diff-w "" "$(printf 'a  b\n' > w1; printf 'ab\n' > w2; diff -w w1 w2)"
t diff-empty-vs-file "0a1,2
> y
> z" "$(: > z0; printf 'y\nz\n' > z1; diff z0 z1)"
t diff-noeol "1c1
< a
\\ No newline at end of file
---
> a" "$(printf 'a' > n1; printf 'a\n' > n2; diff n1 n2)"
diff d1 nonexist 2> /dev/null; t diff-missing 2 $?
# patch-free round trip: apply a normal diff with sed ("3c3" hunks only) is overkill; check edit distance instead
seq 1 200 > big1; seq 1 200 | sed '50d;120s/$/x/;190a\
new' > big2
t diff-big-count 4 "$(diff big1 big2 | grep -c '^[<>]')"

# ---- tac rev nl paste
t tac "c
b
a" "$(printf 'a\nb\nc\n' | tac)"
t tac-noeol "cb
a" "$(printf 'a\nb\nc' | tac)"
t rev "cba
fed" "$(printf 'abc\ndef\n' | rev)"
t nl "     1	a
     2	b" "$(printf 'a\nb\n' | nl)"
t nl-blank "     1	a
       
     2	b" "$(printf 'a\n\nb\n' | nl)"
t nl-ba "     1	a
     2	
     3	b" "$(printf 'a\n\nb\n' | nl -ba)"
t nl-opts "010:a
015:b" "$(printf 'a\nb\n' | nl -nrz -w3 -s: -v10 -i5)"
t nl-ln "1  	a" "$(echo a | nl -nln -w3)"
printf '1\n2\n3\n' > p1; printf 'a\nb\n' > p2
t paste "1	a
2	b
3	" "$(paste p1 p2)"
t paste-d "1,a
2,b
3," "$(paste -d, p1 p2)"
t paste-s "1	2	3" "$(paste -s p1)"
t paste-sd "1,2,3" "$(paste -s -d, p1)"
t paste-stdin "1	a
2	b
3	" "$(cat p2 | paste p1 -)"
t paste-multi-delims "1+a-1" "$(printf '1\n' > q1; printf 'a\n' > q2; paste -d'+-' q1 q2 q1)"

rm -rf $TMP
echo
echo "== $pass passed, $fail failed =="
[ $fail = 0 ] && echo "ALL TESTS PASSED"
[ $fail = 0 ]
