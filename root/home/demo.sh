#!/bin/sh
# demo.sh - a tour of the VibeOS shell: loops, functions, arithmetic, here-docs, colours.   Run: sh /home/demo.sh [name]

esc=$(printf '\033')
c() { printf '%s[%sm' "$esc" "$1"; }
reset=$(c 0)
say() { printf '%s%s%s\n' "$(c "$1")" "$2" "$reset"; }

name=${1:-world}
say '1;38;2;189;147;249' "Hello, $name!  (this is $0, $# argument(s))"
echo

say '1;36' "== multiplication table"
for i in 1 2 3 4 5; do
    row=
    for j in 1 2 3 4 5; do
        row="$row$(printf '%4d' $((i * j)))"
    done
    echo "$row"
done
echo

say '1;32' "== functions & recursion"
fib() {
    if [ "$1" -lt 2 ]; then echo "$1"; return; fi
    a=$(fib $(($1 - 1)))
    b=$(fib $(($1 - 2)))
    echo $((a + b))
}
for n in 0 1 2 3 4 5 6 7; do printf 'fib(%d)=%s ' $n "$(fib $n)"; done
echo
echo

say '1;33' "== while, case, break"
n=27; steps=0
while [ $n -ne 1 ]; do
    if [ $((n % 2)) -eq 0 ]; then n=$((n / 2)); else n=$((3 * n + 1)); fi
    steps=$((steps + 1))
done
echo "collatz(27) takes $steps steps"
for f in notes.txt photo.png archive.tar.gz script.sh; do
    case $f in
        *.txt)    kind="text" ;;
        *.png)    kind="image" ;;
        *.tar.*)  kind="archive" ;;
        *)        kind="other" ;;
    esac
    printf '  %-14s %s (ext .%s)\n' "$f" "$kind" "${f##*.}"
done
echo

say '1;35' "== here-doc & parameter expansion"
user=${USER:-vibe}
cat <<EOF
  user     : $user
  home     : $HOME
  cwd      : $(pwd)
  name len : ${#name}
  1+2*3    : $((1 + 2 * 3))
EOF
echo

say '1;31' "== a rainbow"
for code in 31 33 32 36 34 35; do printf '%s  ' "$(c $code)######$reset"; done
echo
say '2' "(try: help, type cd, alias ll='ls -l', ctrl-r, !!)"
