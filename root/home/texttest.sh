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
+i" "$(diff -u d1 d2 | sed 1,2d)"
t diff-U0 "@@ -3 +3 @@
-c
+X
@@ -8,0 +9 @@
+i" "$(diff -U0 d1 d2 | sed 1,2d)"
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

# ---- awk
printf 'Beth\t4.00\t0\nDan\t3.75\t0\nKathy\t4.00\t10\nMark\t5.00\t20\nMary\t5.50\t22\nSusie\t4.25\t18\n' > emp
t awk-field "Beth
Dan" "$(awk '{print $1}' emp | head -2)"
t awk-fs-tab "Kathy 40
Mark 100
Mary 121
Susie 76.5" "$(awk -F'\t' '$3 > 0 {print $1, $2*$3}' emp)"
t awk-pattern "Beth	4.00	0
Dan	3.75	0" "$(awk '$3 == 0' emp)"
t awk-sum "6 26.5 4.41667" "$(awk '{ pay += $2 } END { print NR, pay, pay/NR }' emp)"
t awk-nf-nr "6 18" "$(awk '{ nw += NF } END { print NR, nw }' emp)"
t awk-begin "7 4 1.5 1 9 -3" "$(awk 'BEGIN { x = 3; print x * 2 + 1, x + 1, x / 2, x % 2, x ^ 2, -x }')"
t awk-num-format "0.333333 10000000000 9007199254740992 0.3 1000000 1e-05 -3 3" "$(awk 'BEGIN { print 1/3, 100000 * 100000, 2^53, 0.1+0.2, 1e6, 1e-5, int(-3.7), int(3.7) }')"
t awk-string-fns "5 ell hello lo 3 HELLO abc" "$(awk 'BEGIN { s = "hello"; print length(s), substr(s, 2, 3), substr(s, 0), substr(s, 4, 100), index(s, "ll"), toupper(s), tolower("ABC") }')"
t awk-split "3 a c a;b;c;" "$(awk 'BEGIN { n = split("a,b,c", arr, ","); printf "%d %s %s ", n, arr[1], arr[3]; for (i = 1; i <= n; i++) printf "%s;", arr[i]; print "" }')"
t awk-split-ws "3 c" "$(awk 'BEGIN { n = split("a b  c", b); print n, b[3] }')"
t awk-gsub "2 X bar X" "$(awk 'BEGIN { s = "foo bar foo"; n = gsub(/foo/, "X", s); print n, s }')"
t awk-sub-amp "foo [bar] foo" "$(awk 'BEGIN { s = "foo bar foo"; sub(/bar/, "[&]", s); print s }')"
t awk-gsub-record "B#th" "$(awk '{ gsub(/[aeiou]/, "#"); print }' emp | head -1 | cut -f1)"
t awk-assign-field "Beth  0" "$(echo 'Beth 4.00 0' | awk '{ $2 = ""; print }')"
t awk-nf-grow "a b c  x 5" "$(echo 'a b c' | awk '{ $5 = "x"; print $0, NF }')"
t awk-nf-assign "a b" "$(echo 'a b c' | awk '{ NF = 2; print }')"
t awk-ofs "a-b-c" "$(echo 'a b c' | awk 'BEGIN { OFS = "-" } { $1 = $1; print }')"
t awk-printf "   42|42   |00042|ff|10|A|h|str|     3.142|1.234500e+03|   ab|ab   |" "$(awk 'BEGIN { printf "%5d|%-5d|%05d|%x|%o|%c|%c|%s|%10.3f|%e|%5s|%-5s|\n", 42, 42, 42, 255, 8, 65, "hello", "str", 3.14159, 1234.5, "ab", "ab" }')"
t awk-printf-star "   42|7   |" "$(awk 'BEGIN { printf "%*d|%-*d|\n", 5, 42, 4, 7 }')"
t awk-printf-pct "45.68%" "$(awk 'BEGIN { printf "%5.2f%%\n", 45.678 }')"
t awk-printf-big "8589934592" "$(awk 'BEGIN { printf "%d\n", 2147483648 * 4 }')"
t awk-array "6 5.50 1 0
0 5" "$(awk '{ a[$1] = $2 } END { for (k in a) n++; print n, a["Mary"], ("Dan" in a), ("Zed" in a); delete a["Dan"]; print ("Dan" in a), length(a) }' emp)"
t awk-multidim "1 2 yes" "$(awk 'BEGIN { a[1,2] = 3; for (k in a) { split(k, p, SUBSEP); printf "%s %s ", p[1], p[2] }; if ((1,2) in a) print "yes" }')"
t awk-function "3628800" "$(awk 'function f(n) { return n <= 1 ? 1 : n * f(n-1) } BEGIN { print f(10) }')"
t awk-func-array "25 5" "$(awk 'function fill(arr, n,  i) { for (i = 1; i <= n; i++) arr[i] = i*i } BEGIN { fill(sq, 5); print sq[5], length(sq) }')"
t awk-func-byval "1" "$(awk 'function g(x) { x = 5 } BEGIN { y = 1; g(y); print y }')"
t awk-loops "1
3" "$(awk 'BEGIN { i = 0; while (i < 5) { i++; if (i == 2) continue; if (i == 4) break; print i } }')"
t awk-do-for "3 4" "$(awk 'BEGIN { do { n++ } while (n < 3); for (;;) { if (++k > 3) break }; print n, k }')"
t awk-regex-pat "Dan
Kathy
Mark
Mary" "$(awk '/a/ && !/s/ { print $1 }' emp)"
t awk-range "Beth
Dan
Kathy" "$(awk '/Beth/,/Kathy/ { print $1 }' emp)"
t awk-nr-range "2: Dan
3: Kathy
4: Mark" "$(awk 'NR==2, NR==4 { print NR": "$1 }' emp)"
t awk-match-op "Mark
Mary" "$(awk '$1 ~ /^M/ { print $1 }' emp)"
t awk-nomatch-op "Beth
Mark
Mary" "$(awk '$1 !~ /a/ || $1 ~ /Mar/ { print $1 }' emp | head -3)"
t awk-dyn-regex "m m2" "$(awk 'BEGIN { if ("abc" ~ "b") printf "m "; r = "^a.c$"; if ("abc" ~ r) print "m2" }')"
t awk-match-fn "2 2 2" "$(awk 'BEGIN { print match("foobar", /o+/), RSTART, RLENGTH }')"
t awk-compare "1 1 1 0 0" "$(awk 'BEGIN { print 1 == 1.0, "a" < "b", "10" < "9", 10 < 9, "abc" < 1 }')"
t awk-field-compare "1 1 1" "$(echo '10 10.0 abc' | awk '{ print ($1 == $2), ($1 == "10"), ($1 < $3) }')"
t awk-ternary "lo" "$(echo 4 | awk '{ print ($1 > 4) ? "hi" : "lo" }')"
t awk-v "10" "$(awk -v n=5 'BEGIN { print n * 2 }')"
t awk-v-escape "a	b" "$(awk -v 's=a\tb' 'BEGIN { print s }')"
t awk-argv "3 foo bar" "$(awk 'BEGIN { print ARGC, ARGV[1], ARGV[2] }' foo bar)"
t awk-filename "emp 1 1
emp 2 2" "$(awk '{ print FILENAME, FNR, NR }' emp | head -2)"
t awk-cmdline-assign "1 Beth" "$(awk '{ print v, $1 }' v=1 emp | head -1)"
t awk-next-exit "Dan
Kathy
end 3" "$(awk 'NR==1 { next } { print $1 } NR==3 { exit } END { print "end", NR }' emp)"
awk 'BEGIN { exit 3 }'; t awk-exit-code 3 $?
t awk-end-record "Susie 3" "$(awk 'END { print $1, NF }' emp)"
t awk-getline-file "6 Beth" "$(awk 'BEGIN { while ((getline line < "emp") > 0) n++; close("emp"); getline < "emp"; print n, $1 }')"
t awk-getline-plain "after getline: Dan 2" "$(awk 'NR==1 { getline; print "after getline:", $1, NR }' emp)"
t awk-getline-var "Beth	4.00	0" "$(awk 'BEGIN { getline x < "emp"; print x }')"
t awk-rs "1: a
2: b
3: c" "$(printf 'a;b;c' | awk 'BEGIN { RS = ";" } { print NR": "$0 }')"
t awk-paragraph "1: a|b
3
2: d|e
3" "$(printf 'a b\nc\n\nd e f\n' | awk 'BEGIN { RS = "" } { print NR": "$1"|"$2; print NF }')"
t awk-fs-single "y" "$(echo x:y:z | awk 'BEGIN { FS = ":" } { print $2 }')"
t awk-fs-regex "yz" "$(echo 'x:y;z' | awk -F'[:;]' '{ print $2 $3 }')"
t awk-fs-pipe "b" "$(echo 'a|b|c' | awk -F'|' '{ print $2 }')"
t awk-nf-dollar "z y" "$(echo x:y:z | awk -F: '{ print $NF, $(NF-1) }')"
t awk-field-assign "4 4" "$(echo '3 4' | awk '{ s = $1; $1 = s + 1; print }')"
t awk-dollar0-assign "3 b" "$(awk 'BEGIN { $0 = "a b c"; print NF, $2 }')"
t awk-uninit "uninit ok 0 0" "$(awk 'BEGIN { if (x == 0 && x == "") printf "uninit ok "; print length(x), x+0 }')"
t awk-dedupe "Beth	4.00	0" "$(awk '!seen[$2]++ && $2 == "4.00"' emp)"
t awk-reverse "Susie
Mary" "$(awk '{ a[NR] = $1 } END { for (i = NR; i > 0; i--) print a[i] }' emp | head -2)"
t awk-wordcount "the 3" "$(printf 'the quick the fox\nthe end\n' | awk '{ for (i = 1; i <= NF; i++) c[$i]++ } END { for (w in c) if (c[w] > 2) print w, c[w] }')"
t awk-pipe-out "a
b
c" "$(printf 'b\na\nc\n' | awk '{ print $1 | "sort" } END { close("sort") }')"
t awk-pipe-getline "hi
there" "$(awk 'BEGIN { while (("echo hi; echo there" | getline l) > 0) print l }')"
t awk-system "from sh" "$(awk 'BEGIN { system("echo from sh") }')"
t awk-redirect "6" "$(awk '{ print > "awkout" } END { close("awkout"); while ((getline l < "awkout") > 0) c++; print c }' emp)"
t awk-f-file "10" "$(printf 'BEGIN { print 5 * 2 }\n' > prog.awk; awk -f prog.awk)"
t awk-comments "c" "$(awk '# comment
BEGIN { print "c" } # trailing')"
t awk-multiline-if "a
d" "$(awk 'BEGIN {
  if (1)
    print "a"
  else
    print "b"
  if (0) print "c"; else print "d"
}')"
t awk-incr "7 3 4" "$(awk 'BEGIN { x = 5; x += 2; y = 3; z = y++ + 0; print x, z, y }')"
t awk-power "-4 512 1 0 1 0" "$(awk 'BEGIN { print -2^2, 2^3^2, !0, !1, !"", !"a" }')"
t awk-concat-prec "A A A 1 2 22" "$(awk 'BEGIN { x = "A"; y = x " " x " " x; print y, 1 " " 2, 1+1 "" 2 }')"
awk 'BEGIN { print 1/0 }' > /dev/null 2>&1; t awk-div-zero 2 $?
awk 'BEGIN { print ( }' > /dev/null 2>&1; t awk-syntax-error 2 $?
t awk-printf-no-newline "ab" "$(awk 'BEGIN { printf "a"; printf "b" }')"
t awk-substr-edge "ello||" "$(awk 'BEGIN { print substr("hello", 2) "|" substr("x", 1, 0) "|" substr("abc", 2, -1) }')"
t awk-length-forms "5 6 18" "$(printf 'hello\n' | awk '{ print length, length($0) + 1, length("abc") * 6 }')"
t awk-toupper-first "Beth" "$(echo beth | awk '{ print toupper(substr($1,1,1)) substr($1,2) }')"
t awk-srand-rand "1" "$(awk 'BEGIN { srand(1); r = rand(); print (r >= 0 && r < 1) }')"
t awk-math "1 0 2.71828 3 3.14" "$(awk 'BEGIN { printf "%d %d %.5f %d %.2f\n", sin(1.5707963) + 0.5, cos(1.5707963) + 0.1, exp(1), sqrt(9), atan2(0, -1) }')"
# ---- md5sum sha256sum
printf 'abc' > h1
t md5-abc "900150983cd24fb0d6963f7d28e17f72  h1" "$(md5sum h1)"
t sha256-abc "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  h1" "$(sha256sum h1)"
t md5-empty "d41d8cd98f00b204e9800998ecf8427e  -" "$(md5sum < /dev/null)"
t sha256-empty "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  -" "$(sha256sum < /dev/null)"
t sha256-stdin "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  -" "$(printf abc | sha256sum)"
t md5-long "7707d6ae4e027c70eea2a935c2296f21  -" "$(awk 'BEGIN { for (i = 0; i < 1000000; i++) printf "a" }' | md5sum)"
t sha256-long "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0  -" "$(awk 'BEGIN { for (i = 0; i < 1000000; i++) printf "a" }' | sha256sum)"
sha256sum h1 > h1.sum
t sha256-check "h1: OK" "$(sha256sum -c h1.sum)"
md5sum h1 > h1.md5
t md5-check "h1: OK" "$(md5sum -c h1.md5)"
printf 'abd' > h1
sha256sum -c h1.sum > /dev/null 2>&1; t sha256-check-fail 1 $?
t md5-check-fail "h1: FAILED" "$(md5sum -c h1.md5 2> /dev/null)"
sha256sum nonexist > /dev/null 2>&1; t sha256-missing 1 $?

rm -rf $TMP
echo
echo "== $pass passed, $fail failed =="
[ $fail = 0 ] && echo "ALL TESTS PASSED"
[ $fail = 0 ]
