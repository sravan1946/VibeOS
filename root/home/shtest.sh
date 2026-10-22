#!/bin/sh
# shtest.sh - regression test for /bin/sh.  Run: sh /home/shtest.sh   (prints PASS/FAIL per check, then a summary)
pass=0
fail=0
t() {   # t name expected actual
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); echo "PASS $1"
    else
        fail=$((fail + 1)); echo "FAIL $1: expected [$2] got [$3]"
    fi
}
TMP=/tmp/shtest.$$
mkdir -p $TMP

# ---- variables, quoting, expansion
A=hello
t var-set hello "$A"
sh -c 'echo ${A:-unset}' > $TMP/o
t var-local-not-exported unset "$(cat $TMP/o)"
export A
t var-export hello "$(sh -c 'echo $A')"
B="two words"
t quote-dq "two words" "$B"
t quote-sq '$A' '$A'
t quote-bs 'a b' a\ b
t default-unset dflt "${NOPE:-dflt}"
t default-empty dflt "${EMPTY:-dflt}"
EMPTY=
t default-colon dflt "${EMPTY:-dflt}"
t default-nocolon "" "${EMPTY-dflt}"
t assign-default v "${ZZ:=v}"
t assign-default-kept v "$ZZ"
t alt-set alt "${A:+alt}"
t alt-unset "" "${NOPE:+alt}"
t length 5 "${#A}"
F=/usr/local/file.tar.gz
t strip-prefix-short "usr/local/file.tar.gz" "${F#/}"
t strip-prefix-long "file.tar.gz" "${F##*/}"
t strip-suffix-short "/usr/local/file.tar" "${F%.*}"
t strip-suffix-long "/usr/local/file" "${F%%.*}"
t special-hash 3 "$(set -- a b c; echo $#)"
t tilde "$HOME" ~
t prefix-assign yes "$(X=yes sh -c 'echo $X')"
t prefix-assign-gone "" "${X:-}"

# ---- arithmetic
t arith-add 7 $((3 + 4))
t arith-prec 14 $((2 + 3 * 4))
t arith-paren 20 $(((2 + 3) * 4))
t arith-div 3 $((10 / 3))
t arith-mod 1 $((10 % 3))
t arith-neg -5 $((-5))
t arith-cmp 1 $((3 < 4))
t arith-eq 0 $((3 == 4))
t arith-and 1 $((1 && 2))
t arith-or 1 $((0 || 5))
N=6
t arith-varref 36 $((N * N))
t arith-shift 8 $((1 << 3))
t arith-hex 255 $((0xff))
t arith-nested 9 $(( $((1+2)) * 3 ))

# ---- command substitution
t cmdsub hi "$(echo hi)"
t cmdsub-bq hi "`echo hi`"
t cmdsub-strip "a" "$(printf 'a\n\n\n')"
t cmdsub-nested "x-hi" "x-$(echo $(echo hi))"
t cmdsub-quotes "a  b" "$(echo "a  b")"
t cmdsub-status 3 "$(sh -c 'exit 3'; echo $?)"
v=$(sh -c 'exit 4')
t cmdsub-assign-status 4 $?

# ---- if / while / until / for / case
if true; then r=then; else r=else; fi
t if-then then $r
if false; then r=then; else r=else; fi
t if-else else $r
if false; then r=a; elif true; then r=b; else r=c; fi
t if-elif b $r
if ! false; then r=neg; fi
t bang neg $r
i=0; s=
while [ $i -lt 5 ]; do s="$s$i"; i=$((i + 1)); done
t while 01234 $s
i=0; s=
until [ $i -ge 3 ]; do s="$s$i"; i=$((i + 1)); done
t until 012 $s
s=
for x in a b c; do s="$s$x"; done
t for abc $s
s=
for x in 1 2 3 4 5 6; do
    [ $x = 2 ] && continue
    [ $x = 5 ] && break
    s="$s$x"
done
t break-continue 134 $s
s=
for i in 1 2 3; do for j in a b c; do [ $j = b ] && continue 2; s="$s$i$j"; done; done
t continue-2 1a2a3a $s
s=
for i in 1 2 3; do for j in a b c; do [ $i = 2 ] && break 2; s="$s$i$j"; done; done
t break-2 1a1b1c $s
set -- p q r
s=
for x; do s="$s$x"; done
t for-positional pqr $s
s=
for f in /bin/ech*; do s="$s$f"; done
t for-glob /bin/echo $s
for w in apple banana cherry; do
    case $w in
        a*) r=A ;;
        b*|c*) r=BC ;;
    esac
    s="$s$r"
done
t case-glob "/bin/echoABCBC" "$s"
case "x.tar.gz" in *.zip) r=zip ;; *.tar.*) r=tar ;; *) r=other ;; esac
t case-pattern tar $r
case foo in [a-e]*) r=low ;; [f-z]*) r=high ;; esac
t case-range high $r
case abc in "a*") r=quoted ;; a?c) r=q ;; esac
t case-quoted-pattern q $r
case nomatch in x) r=x ;; esac
t case-nomatch-status 0 $?
t while-pipe-read "a1 b2 " "$(printf 'a 1\nb 2\n' | while read k v; do printf '%s%s ' $k $v; done)"

# ---- functions
f() { echo "f:$1:$#"; }
t func-args "f:x:2" "$(f x y)"
g() { return 3; }
g
t func-return 3 $?
h() { local v=inner; echo $v; }
v=outer
t func-local inner "$(h)"
t func-local-restored outer $v
fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1 - 1))) )); fi; }
t func-recursion 120 "$(fact 5)"
cnt() { n=$((n + 1)); }
n=0; cnt; cnt; cnt
t func-global 3 $n
pos() { shift; echo "$@"; }
t func-shift "b c" "$(pos a b c)"
q() { for a in "$@"; do printf '[%s]' "$a"; done; }
t dollar-at-quoted "[a b][c]" "$(q "a b" c)"
t dollar-at-empty "" "$(q)"
star() { echo "$*"; }
t dollar-star "a b c" "$(star a b c)"
t func-in-pipe "from f" "$(f2() { echo from f; }; f2 | cat)"
w() { read x; echo "got:$x"; }
t func-pipe-stage "got:hi" "$(echo hi | w)"

# ---- groups, subshells, redirection, here-docs
t group "$(printf 'a\nb')" "$({ echo a; echo b; } | cat)"
t subshell-cwd "/tmp" "$(cd /; (cd /tmp; pwd))"
cd /; ( cd /tmp )
t subshell-cwd-kept "/" "$(pwd)"
x=1
(x=2)
t subshell-isolated 1 $x
t subshell-inherit 1 "$( (echo $x) )"
(exit 5)
t subshell-status 5 $?
echo one > $TMP/f
echo two >> $TMP/f
t redirect-out "one two" "$(echo $(cat $TMP/f))"
t redirect-in "one" "$(head -n 1 < $TMP/f)"
{ echo g1; echo g2; } > $TMP/g
t group-redirect "g1 g2" "$(echo $(cat < $TMP/g))"
t redirect-err "oops" "$(sh -c 'echo oops >&2' 2>&1)"
ls /nonexistent_dir > /dev/null 2> $TMP/err
[ -s $TMP/err ]; t redirect-stderr-file 0 $?
while read l; do last=$l; done < $TMP/f
t while-redirect two "$last"
t heredoc "a hello" "$(cat <<EOF
a $A
EOF
)"
t heredoc-literal 'a $A' "$(cat <<'EOF'
a $A
EOF
)"
t heredoc-tabs "x" "$(cat <<-EOF
		x
	EOF
)"
t heredoc-cmdsub "n=2" "$(cat <<EOF
n=$((1 + 1))
EOF
)"
read p1 p2 <<EOF
first second
EOF
t read-two "first/second" "$p1/$p2"
IFS=: read p1 p2 <<EOF
1:2
EOF
t read-ifs "1/2" "$p1/$p2"
read -r rest <<EOF
a   b
EOF
t read-line "a   b" "$rest"
printf 'l1\nl2\n' | { read a1; read a2; echo "$a1-$a2" > $TMP/rd; }
t read-pipe "l1-l2" "$(cat $TMP/rd)"

# ---- builtins: test, printf, shift, set, eval, type, alias, wait
[ -e /bin/sh ]; t test-e 0 $?
[ -f /bin ]; t test-f 1 $?
[ -d /bin ]; t test-d 0 $?
[ -s /bin/sh ]; t test-s 0 $?
[ -z "" ]; t test-z 0 $?
[ -n "" ]; t test-n 1 $?
[ a = a ]; t test-str-eq 0 $?
[ a != a ]; t test-str-ne 1 $?
[ 3 -lt 5 ]; t test-lt 0 $?
[ 5 -le 5 ]; t test-le 0 $?
[ 5 -gt 5 ]; t test-gt 1 $?
[ 5 -ge 6 ]; t test-ge 1 $?
[ 2 -eq 2 -a 3 -ne 4 ]; t test-and 0 $?
[ 1 -eq 2 -o 3 -ne 4 ]; t test-or 0 $?
[ ! -e /nope ]; t test-not 0 $?
[ \( 1 -eq 1 \) -a -n x ]; t test-paren 0 $?
test -d /bin && r=yes; t test-builtin yes $r
t printf-s "ab" "$(printf '%s%s' a b)"
t printf-d "007" "$(printf '%03d' 7)"
t printf-x "ff FF" "$(printf '%x %X' 255 255)"
t printf-o "10" "$(printf '%o' 8)"
t printf-c "A" "$(printf '%c' ABC)"
t printf-width "[  ab][ab  ]" "$(printf '[%4s][%-4s]' ab ab)"
t printf-escape "a$(printf '\t')b" "$(printf 'a\tb')"
t printf-reuse "1,2,3," "$(printf '%s,' 1 2 3)"
t printf-percent "50%" "$(printf '50%%')"
t printf-b "a
b" "$(printf '%b' 'a\nb')"
t echo-n "x" "$(echo -n x)"
t echo-e "a$(printf '\t')b" "$(echo -e 'a\tb')"
set -- a b c
shift
t shift "b c" "$*"
shift 2
t shift-n 0 $#
set -- x y
t set-positional "y x" "$2 $1"
set --
t set-empty 0 $#
t eval "3" "$(eval 'echo $((1+2))')"
cmd='echo evaled'
t eval-var evaled "$(eval $cmd)"
t type-builtin "cd is a shell builtin" "$(type cd)"
t type-keyword "if is a shell keyword" "$(type if)"
t type-file "ls is /bin/ls" "$(type ls)"
t type-func "f is a function" "$(type f)"
alias hi='echo aliased'
hi > $TMP/o
t alias "aliased" "$(cat $TMP/o)"
alias hi > $TMP/o
t alias-print "alias hi='echo aliased'" "$(cat $TMP/o)"
unalias hi
alias hi 2> /dev/null
t unalias 1 $?
sleep 0 &
wait $!
t wait-status 0 $?
(sleep 0; exit 7) &
wait $!
t wait-exit-status 7 $?
t exec viaexec "$(exec echo viaexec)"
t colon-true 0 "$(:; echo $?)"
t status-var 1 "$(false; echo $?)"
t and-or "ok" "$(false && echo no || echo ok)"

# ---- set -e / set -x
t set-e-subshell "" "$( (set -e; false; echo not-reached) )"
t set-e-cond "ran" "$( (set -e; if false; then :; fi; false || true; echo ran) )"
t sh-flag-e "" "$(sh -ec 'false; echo not-reached')"
t set-x "+ echo traced" "$( (set -x; echo traced >/dev/null) 2>&1 )"

# ---- control flow edge cases
lp() { for i in 1 2 3; do [ $i = 2 ] && return 9; done; echo not-reached; }
lp
t return-in-loop 9 $?
rd() { while true; do break; done; echo after; }
t break-in-func after "$(rd)"
s=
for x in $(seq 1 3); do s="$s$x"; done
t for-cmdsub 123 $s
i=0
while :; do i=$((i + 1)); [ $i -ge 3 ] && break; done
t while-colon-break 3 $i
t pipeline-status 1 "$(echo x | grep y > /dev/null; echo $?)"
t negated-pipeline 0 "$(! echo x | grep y > /dev/null; echo $?)"
t three-stage "b" "$(printf 'a\nb\nc\n' | grep -v a | grep -v c)"

# ---- #! scripts (kernel exec path)
printf '#!/bin/sh\necho "script:$1:$#"\n' > $TMP/s1
t shebang "script:a:2" "$($TMP/s1 a b)"
printf '#! /bin/sh\nexit 6\n' > $TMP/s2
$TMP/s2
t shebang-space-status 6 $?
printf '#!/bin/echo hi\n' > $TMP/s3
t shebang-arg "hi $TMP/s3 x" "$($TMP/s3 x)"
printf 'echo plain-text-script\n' > $TMP/s4
t no-shebang "plain-text-script" "$($TMP/s4)"
printf 'echo sourced:$1\nR=set-by-source\n' > $TMP/s5
. $TMP/s5 arg > /dev/null
t source "set-by-source" "$R"
t sh-script "sourced:z" "$(sh $TMP/s5 z)"
t sh-dash-c "a b" "$(sh -c 'echo $0 $1' a b)"

# ---- multi-line constructs inside $( ) and big loops
t multiline "6" "$(
    total=0
    for i in 1 2 3; do
        total=$((total + i))
    done
    echo $total
)"
sum=0; i=1
while [ $i -le 200 ]; do sum=$((sum + i)); i=$((i + 1)); done
t loop-200 20100 $sum

# ---- trap (kernel signal handlers)
t trap-exit "body
bye" "$(sh -c 'trap "echo bye" EXIT; echo body')"
t trap-usr1 "got usr1
0" "$(sh -c 'trap "echo got usr1" USR1; kill -USR1 $$; echo $?' 2>&1)"
t trap-keeps-status 0 "$(sh -c 'trap "false" USR1; kill -USR1 $$; echo $?')"
t trap-ignore alive "$(sh -c 'trap "" TERM; kill -TERM $$; echo alive')"
t trap-reset 143 "$(sh -c 'trap "echo no" TERM; trap - TERM; kill -TERM $$; echo still'; echo $?)"
t trap-list "trap -- 'echo x' INT" "$(sh -c 'trap "echo x" INT; trap')"
t trap-default-kill 138 "$(sh -c 'kill -USR1 $$; echo survived'; echo $?)"

rm -rf $TMP
echo
echo "== $pass passed, $fail failed =="
[ $fail = 0 ] && echo "ALL TESTS PASSED"
[ $fail = 0 ]
