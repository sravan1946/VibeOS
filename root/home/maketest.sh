#!/bin/sh
# maketest.sh - regression test for /bin/make, tcc -run and the Makefile project in ~/projects/wordfreq.
# Run: sh /home/maketest.sh   (prints PASS/FAIL per check, then a summary)
pass=0
fail=0
t() {   # t name expected actual
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); echo "PASS $1"
    else
        fail=$((fail + 1)); echo "FAIL $1: expected [$2] got [$3]"
    fi
}
TMP=/tmp/maketest.$$
mkdir -p $TMP
cd $TMP

# ---- basics
printf 'all: b\n\t@echo all\nb: a\n\t@echo B; echo b > b\na:\n\t@echo A; echo a > a\n' > Makefile
t basic "A
B
all" "$(make)"
t phony-reruns "all" "$(make)"      # a and b exist now, but all is not a file
rm -f a b
t dry-run "echo A; echo a > a
echo B; echo b > b
echo all" "$(make -n)"
printf 'all:\n\techo loud\n' > ms
t silent-flag "loud" "$(make -s -f ms)"
t not-silent "echo loud
loud" "$(make -f ms)"
t no-rule "make: *** No rule to make target 'zz'.  Stop." "$(make zz 2>&1)"
t exit2 2 "$(make zz > /dev/null 2>&1; echo $?)"
printf 'all:\n\tfalse\n' > m2
t recipe-error "false
make: *** [m2:2: all] Error 1" "$(make -f m2 2>&1)"
t ignore-error "false
make: [m2:2: all] Error 1 (ignored)" "$(make -i -f m2 2>&1)"
printf 'all:\n\t-false\n\t@echo after\n' > m3
t dash-prefix "false
after" "$(make -f m3 2> /dev/null)"

# ---- variables and functions
printf 'A = 1\nB := $(A)\nA = 2\nC ?= c\nC ?= d\nD = x\nD += y\nall:\n\t@echo $(A) $(B) $(C) $(D)\n' > m4
t var-flavors "2 1 c x y" "$(make -f m4)"
t var-cmdline "9 9 c x y" "$(make -f m4 A=9)"
printf 'all:\n\t@echo $(V)\n' > m4e
t var-env "from-env" "$(V=from-env make -f m4e)"
t var-env-override "mk" "$(V=from-env make -f m4e V=mk)"
printf 'L = a.c b.c\nall:\n\t@echo $(L:.c=.o) $(patsubst %%.c,%%.x,$(L)) $(subst a,A,$(L)) $(basename $(L)) $(notdir /x/y.z) $(addprefix p/,$(L)) $(addsuffix .s,a b)\n' > m5
t functions "a.o b.o a.x b.x A.c b.c a b y.z p/a.c p/b.c a.s b.s" "$(make -f m5)"
touch w1.c w2.c
printf 'all:\n\t@echo $(wildcard w*.c) $(sort b a b) $(words a b c) $(word 2,x y z) $(shell echo hi) $(if x,yes,no) $(foreach v,1 2,[$(v)])\n' > m6
t functions2 "w1.c w2.c a b 3 y hi yes [1] [2]" "$(make -f m6)"
printf 'out: in1 in2\n\t@echo "$@|$<|$^"\nin1 in2:\n\t@touch $@\n' > m8
t auto-vars "out|in1|in1 in2" "$(make -f m8)"
printf 'ifeq ($(X),1)\nR = one\nelse\nR = other\nendif\nall:\n\t@echo $(R)\n' > m9
t ifeq-else "other
one" "$(make -f m9; make -f m9 X=1)"

# ---- rules
printf '%%.o: %%.c\n\t@echo cc $< $@; cp $< $@\nall: w1.o w2.o\n' > m10
t pattern "cc w1.c w1.o
cc w2.c w2.o" "$(make -f m10)"
t pattern-uptodate "make: Nothing to be done for 'all'." "$(make -f m10)"
sleep 2; touch w1.c
t pattern-rebuild "cc w1.c w1.o" "$(make -f m10)"
printf '.SUFFIXES: .in .out\n.in.out:\n\t@echo conv $< $@; cp $< $@\nall: s.out\n' > m11
echo x > s.in
t suffix-rule "conv s.in s.out" "$(make -f m11)"
printf '.PHONY: clean\nclean:\n\t@echo cleaning\nall:\n' > m12
touch clean
t phony "cleaning" "$(make -f m12 clean)"
printf 'c1: c2\n\t@echo c1\nc2: c1\n\t@echo c2\n' > m13
t circular "make: Circular c2 <- c1 dependency dropped.
c2
c1" "$(make -f m13 2>&1)"
printf 'all: | d\n\t@echo ok\nd:\n\t@mkdir d\n' > m14
t order-only "ok" "$(make -f m14)"
printf 'define R\n@echo one\n@echo two\nendef\nall:\n\t$(R)\n' > m15
t define "one
two" "$(make -f m15)"
printf 'include inc.mk\n-include none.mk\nall:\n\t@echo $(INC)\n' > m16
echo 'INC = included' > inc.mk
t include "included" "$(make -f m16)"
printf 'all:\n\t@$(MAKE) -C sub\n' > m17
mkdir sub; printf 'all:\n\t@echo in sub\n' > sub/Makefile
t recursive "make[1]: Entering directory '$TMP/sub'
in sub
make[1]: Leaving directory '$TMP/sub'" "$(make -f m17)"
t C-flag "make: Entering directory '$TMP/sub'
in sub
make: Leaving directory '$TMP/sub'" "$(make -C sub)"
printf 'all: x\n\t@echo done\nx:\n\t@echo X; exit 1\n' > m18
t keep-going "X
make: *** [m18:4: x] Error 1
make: Target 'all' not remade because of errors." "$(make -k -f m18 2>&1)"

# ---- tcc with make, and tcc -run through a pipe
cd /home/projects/wordfreq
make clean > /dev/null
t proj-build "CC main.c
CC words.c
CC table.c
LINK wordfreq" "$(make)"
t proj-run "3 the" "$(printf 'the cat the dog\nthe end\n' | ./wordfreq | head -1)"
t proj-test "test: ok" "$(make test)"
t proj-uptodate "make: Nothing to be done for 'all'." "$(make)"
sleep 2; touch words.h
t proj-header-dep "CC main.c
CC words.c
CC table.c
LINK wordfreq" "$(make)"
make clean > /dev/null
t proj-clean "" "$(ls *.o wordfreq 2> /dev/null)"
cd $TMP
printf '#include <stdio.h>\n#include <stdlib.h>\nint main(int argc, char **argv) { printf("hello %%s %%s\\n", argv[1], getenv("RUNV")); return 3; }\n' > r.c
t run-pipe "hello x v" "$(RUNV=v tcc -run r.c x | cat)"
tcc -run r.c x > /dev/null; t run-exit 3 $?
t run-args "hello a b" "$(RUNV=b tcc -run r.c a | cat)"
t run-no-leftover "" "$(ls /tmp/tccrun.* 2> /dev/null)"
printf 'int main( {\n' > bad.c
tcc -run bad.c > /dev/null 2>&1; t run-compile-error 1 $?

cd /
rm -rf $TMP
echo
echo "== $pass passed, $fail failed =="
[ $fail = 0 ] && echo "ALL TESTS PASSED"
[ $fail = 0 ]
