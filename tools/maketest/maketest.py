#!/usr/bin/env python3
"""Differential test of user/bin/make.c (built for the host with -DMAKE_HOST) against GNU make.

Every case is a set of files plus steps (make runs, touches, writes); both makes run the steps in fresh
directories, with the inputs given old timestamps. Combined stdout+stderr, exit codes and the resulting files
must be identical. Usage: maketest.py OURMAKE [name-filter]   (GNU make is `make` on PATH)"""
import os, subprocess, sys, tempfile, shutil

OLD, ANCIENT = 1_000_000_000, 900_000_000   # inputs are OLD; touching a product ANCIENT makes it stale
CASES = []
def case(name, files, *steps): CASES.append((name, files, steps))
def mk(*args): return ("make", list(args))

# ---- basics
case("basic", {"Makefile": "all: a b\n\t@echo all\na:\n\t@echo A > a\nb: a\n\t@echo B > b; cat a b\n"}, mk(), mk(), mk("clean"))
case("default-goal", {"Makefile": "x:\n\t@echo x\ny:\n\t@echo y\n"}, mk(), mk("y"), mk("y", "x"))
case("up-to-date", {"Makefile": "a: b\n\tcp b a\n", "b": "1\n"}, mk(), mk(), ("touch", "a", "ancient"), mk(), mk())
case("nothing", {"Makefile": "all: a\na: ;\n.PHONY: all\n", "a": "x"}, mk(), mk("a"))
case("no-rule", {"Makefile": "all: nothere\n\t@echo hi\n"}, mk(), mk("zzz"), mk("-k"), mk("-k", "zzz", "all"))
case("no-makefile", {}, mk(), mk("x"))
case("missing-file", {}, mk("-f", "nofile"))
case("multi-target", {"Makefile": "a b: c\n\t@echo $@ from $<\nc:\n\t@touch c\n"}, mk("a", "b"), mk("a", "b"))
case("echo-error", {"Makefile": "all:\n\techo one\n\tfalse\n\techo two\n"}, mk(), mk("-i"), mk("-k"))
case("prefix-chars", {"Makefile": "all:\n\t@echo quiet\n\t-false\n\t-@false\n\t@-false\n\techo loud\n\t+echo plus\n"}, mk(), mk("-s"), mk("-n"))
case("exit-status", {"Makefile": "all:\n\t@exit 3\n"}, mk())
case("dry-run", {"Makefile": "all: a\n\t@echo done\na:\n\ttouch a\n"}, mk("-n"), mk("-n"), mk(), mk("-n"))
case("always-make", {"Makefile": "all: a\na:\n\t@echo build a\n\t@touch a\n"}, mk(), mk(), mk("-B"), mk("-B", "-n"))
case("silent-flag", {"Makefile": "all:\n\techo hello\n"}, mk("-s"), mk("--silent"))
case("order-only", {"Makefile": "all: x | dir\n\t@echo $^ / $|\nx:\n\t@touch x\ndir:\n\t@mkdir dir\n"}, mk(), mk())
case("circular", {"Makefile": "a: b\n\t@echo a\nb: a\n\t@echo b\n"}, mk(), mk("b"))
case("phony", {"Makefile": ".PHONY: clean\nclean:\n\t@echo cleaning\nclean.txt:\n\t@echo file\n"}, mk("clean"), ("write", "clean", "x"), mk("clean"))
case("force-idiom", {"Makefile": "out: FORCE\n\t@echo run\nFORCE:\n"}, mk(), mk(), ("write", "out", "x"), mk())
case("chain-fail", {"Makefile": "all: a b\na:\n\t@false\nb:\n\t@echo b\n"}, mk(), mk("-k"))
case("delete-on-error", {"Makefile": ".DELETE_ON_ERROR:\nout:\n\t@echo x > out; false\n"}, mk())
case("silent-special", {"Makefile": ".SILENT:\nall:\n\techo quiet\n"}, mk())
case("silent-special-targets", {"Makefile": ".SILENT: a\nall: a b\na:\n\techo a\nb:\n\techo b\n"}, mk())
case("ignore-special", {"Makefile": ".IGNORE:\nall:\n\tfalse\n\t@echo after\n"}, mk())
case("default-rule", {"Makefile": ".DEFAULT:\n\t@echo default for $@\nall: foo\n"}, mk(), mk("bar"))
case("continuation", {"Makefile": "A = a \\\n    b \\\n  c\nall:\n\t@echo $(A)\n\t@echo one \\\n\ttwo\n"}, mk())
case("comments", {"Makefile": "# hi\nA = 1 # tail\nall: # c\n\t@echo '$(A)|' # shell sees this\n# end\n\n"}, mk())
case("escaped-hash", {"Makefile": "A = x\\#y\nall:\n\t@echo '$(A)'\n"}, mk())
case("dollar-dollar", {"Makefile": "all:\n\t@echo $$HOME_UNSET$$$$ | sed 's/[0-9]*$$/N/'\n\t@V=ok; echo $$V\n"}, mk())
case("inline-recipe", {"Makefile": "all: ; @echo inline\nb: c ; @echo b from $<\nc:\n\t@touch c\n"}, mk(), mk("b"))
case("recipe-before-target", {"Makefile": "\techo x\nall:\n"}, mk())
case("missing-separator", {"Makefile": "all:\n\t@echo a\nthis is wrong\n"}, mk())
case("include", {"Makefile": "include inc.mk\n-include nope.mk\nall:\n\t@echo $(V)\n", "inc.mk": "V = from-inc\n"}, mk())
case("include-missing", {"Makefile": "include nope.mk\nall:\n\t@echo hi\n"}, mk())
case("include-wild", {"Makefile": "include *.mk\nall:\n\t@echo $(A)$(B)\n", "a.mk": "A=1\n", "b.mk": "B=2\n"}, mk())
case("makefile-list", {"Makefile": "include a.mk\nall:\n\t@echo $(MAKEFILE_LIST)\n", "a.mk": ""}, mk())
case("f-flag", {"foo.mk": "all:\n\t@echo foo\n", "bar.mk": "all2:\n\t@echo bar\n"}, mk("-f", "foo.mk"), mk("-f", "foo.mk", "-f", "bar.mk", "all2"), mk("-ffoo.mk"))
case("makefile-names", {"makefile": "all:\n\t@echo lower\n", "Makefile": "all:\n\t@echo upper\n"}, mk())
case("C-flag", {"sub/Makefile": "all:\n\t@echo in sub; pwd | sed 's|.*/||'\n"}, mk("-C", "sub"), mk("-C", "sub", "-s"))
case("recursive-make", {"Makefile": "all:\n\t@$(MAKE) -C sub\n\t@$(MAKE) -C sub x=1\n", "sub/Makefile": "all:\n\t@echo sub $(x)\n"}, mk(), mk("-n"))
case("recursive-fail", {"Makefile": "all:\n\t$(MAKE) -C sub\n", "sub/Makefile": "all:\n\tfalse\n"}, mk())
case("recursive-n", {"Makefile": "all:\n\techo top\n\t$(MAKE) -C sub\n", "sub/Makefile": "all:\n\techo sub\n"}, mk("-n"))
# ---- variables
case("var-flavors", {"Makefile": "A = $(B)\nB = b\nC := $(B)x\nB = bb\nD ?= d\nD ?= e\nE = 1\nE += 2\nF := 1\nF += $(B)\nall:\n\t@echo $(A) $(C) $(D) $(E) $(F)\n"}, mk())
case("var-cmdline", {"Makefile": "A = file\nall:\n\t@echo $(A) $(B)\n"}, mk("A=cmd", "B=2"), mk("A=cmd", "A=x"))
case("var-env", {"Makefile": "all:\n\t@echo $(FOO) $$FOO\nFOO = mk\n"}, ("env", "FOO", "env"), mk(), mk("-e"))
case("var-override", {"Makefile": "override A = ov\nB = b\nall:\n\t@echo $(A) $(B)\n"}, mk("A=cmd", "B=cmd"))
case("var-shell-assign", {"Makefile": "A != echo hi\nB := $(shell echo a   b)\nall:\n\t@echo $(A) $(B)\n"}, mk())
case("var-refs", {"Makefile": "X = x\nXY = xy\nN = X\nall:\n\t@echo $($(N)) ${X} $(XY) $(X)Y $X\n"}, mk())
case("var-subst-ref", {"Makefile": "L = a.c b.c c.h\nall:\n\t@echo $(L:.c=.o) $(L:%.c=%.x) $(L:%.h=h/%)\n"}, mk())
case("var-empty", {"Makefile": "E =\nall:\n\t@echo '[$(E)][$(UNDEF)]'\n"}, mk())
case("var-spaces", {"Makefile": "A   =   spaced   \nB=nospace\nall:\n\t@echo '[$(A)][$(B)]'\n"}, mk())
case("var-in-target", {"Makefile": "T = out\n$(T): $(T).in\n\t@cp $< $@; echo made $@\n", "out.in": "z"}, mk())
case("define", {"Makefile": "define MSG\nline1\nline2 $(X)\nendef\nX = 5\nall:\n\t@$(info $(MSG))\ndefine TWO =\na b\nendef\nz:\n\t@echo $(TWO)\n"}, mk(), mk("z"))
case("define-recipe", {"Makefile": "define R\n@echo one\n@echo two\nendef\nall:\n\t$(R)\n"}, mk())
case("export", {"Makefile": "export FOO = bar\nBAZ = no\nexport QUX\nQUX = q\nall:\n\t@echo $$FOO $$BAZ $$QUX\n"}, mk())
case("automatic", {"Makefile": "all: d/a.o b.o d/a.o\n\t@echo '$@|$<|$^|$+|$?|$(@D)|$(@F)|$(<D)|$(<F)'\nd/a.o b.o:\n\t@mkdir -p d; touch $@\n"}, mk(), mk())
case("stem", {"Makefile": "%.out: %.in\n\t@echo '$* $@ $<'; touch $@\n", "x.in": "1"}, mk("x.out"))
case("makecmdgoals", {"Makefile": "a b:\n\t@echo '$(MAKECMDGOALS)'\n"}, mk("a", "b"))
case("curdir", {"Makefile": "all:\n\t@echo $(CURDIR)\n"}, mk())
case("origin", {"Makefile": "A = 1\nall:\n\t@echo $(origin A) $(origin B) $(origin CC) $(origin PATH) $(origin @)\n"}, mk(), mk("B=1"))
# ---- functions
case("fn-text", {"Makefile": "all:\n\t@echo '$(subst ee,EE,feet on the street)|$(patsubst %.c,%.o,x.c y.c z.h)|$(strip   a   b  )|$(findstring a,abc)|$(findstring z,abc)|$(filter %.c %.h,a.c b.o c.h)|$(filter-out %.c,a.c b.o c.h)|$(sort foo bar foo baz)'\n"}, mk())
case("fn-words", {"Makefile": "L = one two three four\nall:\n\t@echo '$(word 2,$(L))|$(wordlist 2,3,$(L))|$(words $(L))|$(firstword $(L))|$(lastword $(L))|$(word 9,$(L))'\n"}, mk())
case("fn-word-zero", {"Makefile": "all:\n\t@echo $(word 0,a b)\n"}, mk())
case("fn-paths", {"Makefile": "L = src/a.c b.tar.gz dir.d/file /x/y.z\nall:\n\t@echo '$(dir $(L))|$(notdir $(L))|$(suffix $(L))|$(basename $(L))'\n"}, mk())
case("fn-affix", {"Makefile": "all:\n\t@echo '$(addprefix src/,a b)|$(addsuffix .o,a b)|$(join a b c,1 2)'\n"}, mk())
case("fn-wildcard", {"Makefile": "all:\n\t@echo '$(wildcard *.c)|$(wildcard *.nope)|$(wildcard a.c b.c)|$(wildcard s*/*.h)|$(wildcard [ab].c)|$(wildcard ?.c)'\n", "a.c": "", "b.c": "", "c.h": "", "sub/x.h": "", ".hid.c": ""}, mk())
case("fn-shell", {"Makefile": "A := $(shell printf 'a\\nb\\n')\nall:\n\t@echo '$(A)|$(shell echo hi; echo there)|$(shell exit 3)'\n"}, mk())
case("fn-if", {"Makefile": "E =\nall:\n\t@echo '$(if $(E),yes,no)|$(if x,yes,no)|$(if x,yes)|[$(if $(E),yes)]|$(or $(E),b,c)|$(and a,b)|[$(and a,$(E))]'\n"}, mk())
case("fn-foreach", {"Makefile": "all:\n\t@echo '$(foreach x,a b c,[$(x)])|$(foreach d,1 2,$(foreach e,a b,$(d)$(e)))'\n"}, mk())
case("fn-call", {"Makefile": "rev = $(2) $(1)\nsq = $(foreach w,$(1),$(w)$(w))\nall:\n\t@echo '$(call rev,a,b)|$(call sq,x y)|[$(call rev,only)]'\n"}, mk())
case("fn-nested-comma", {"Makefile": "all:\n\t@echo '$(subst a,b,$(patsubst %.c,%,a.c,x))'\n"}, mk())
case("fn-info-warning", {"Makefile": "$(info info-msg)\n$(warning warn-msg)\nall:\n\t@echo run\n"}, mk())
case("fn-error", {"Makefile": "all:\n\t@echo before\n$(error stop here)\n"}, mk())
case("fn-error-recipe", {"Makefile": "all:\n\t@echo before\n\t$(error in recipe)\n"}, mk())
case("fn-abspath", {"Makefile": "all:\n\t@echo '$(notdir $(abspath ../x/./y/../z))|$(abspath /a/b/../c)|$(abspath /)'\n"}, mk())
case("fn-unknown", {"Makefile": "all:\n\t@echo '[$(nosuchfn a,b)]'\n"}, mk())
# ---- conditionals
case("ifeq", {"Makefile": "A = x\nifeq ($(A),x)\nR = eq\nelse\nR = ne\nendif\nifeq \"$(A)\" 'y'\nS = y\nelse ifeq ($(A), x)\nS = xx\nelse\nS = n\nendif\nifneq ($(A),x)\nT = no\nendif\nall:\n\t@echo $(R) $(S) $(T)\n"}, mk())
case("ifdef", {"Makefile": "A = 1\nE =\nifdef A\nR1 = def\nendif\nifdef E\nR2 = def\nelse\nR2 = empty\nendif\nifndef U\nR3 = undef\nendif\nall:\n\t@echo $(R1) $(R2) $(R3)\n"}, mk())
case("if-nested", {"Makefile": "A=1\nifdef A\nifeq ($(A),2)\nX=two\nelse\nX=notwo\nendif\nelse\nifdef Q\nX=q\nelse\nX=none\nendif\nendif\nall:\n\t@echo $(X)\n"}, mk())
case("if-recipe", {"Makefile": "all:\nifdef NOPE\n\t@echo nope\nelse\n\t@echo yep\nendif\n"}, mk())
case("if-missing-endif", {"Makefile": "ifdef A\nall:\n"}, mk())
# ---- implicit and pattern rules
case("pattern", {"Makefile": "%.o: %.c\n\t@echo compile $< to $@; cp $< $@\nall: a.o b.o\n", "a.c": "a", "b.c": "b"}, mk(), mk(), ("touch", "a.o", "ancient"), mk())
case("pattern-prereqs", {"Makefile": "%.o: %.c common.h\n\t@echo cc $^\nall: a.o\n", "a.c": "", "common.h": ""}, mk())
case("suffix-rule", {"Makefile": ".SUFFIXES: .in .out\n.in.out:\n\t@echo conv $< $@; cp $< $@\nall: x.out\n", "x.in": "1"}, mk(), mk())
case("suffix-single", {"Makefile": ".SUFFIXES: .sh\n.sh:\n\t@echo script $<; cp $< $@\n", "tool.sh": "1"}, mk("tool"))
case("suffixes-clear", {"Makefile": ".SUFFIXES:\n.c.o:\n\t@echo never\nall: a.o\n", "a.c": ""}, mk())
case("builtin-c-o", {"Makefile": "CC = echo\nall: a.o\n", "a.c": ""}, mk(), mk("-r"))
case("builtin-link", {"Makefile": "CC = echo\nCFLAGS = -g\nall: prog\n", "prog.c": ""}, mk())
case("builtin-no-makefile", {"hello.c": ""}, mk("-n", "hello"), mk("-n", "hello.o"))
case("impl-extra-deps", {"Makefile": "CC = echo\na.o: a.h\nall: a.o\n", "a.c": "", "a.h": ""}, mk(), mk("-B"))
case("impl-shortest-stem", {"Makefile": "lib%.a: lib%.c\n\t@echo specific\n%.a: %.c\n\t@echo general\nall: libx.a\n", "libx.c": ""}, mk())
case("impl-dir", {"Makefile": "%.o: %.c\n\t@echo $< $@; touch $@\nall: sub/a.o\n", "sub/a.c": ""}, mk(), mk())
case("static-pattern", {"Makefile": "OBJS = a.o b.o\n$(OBJS): %.o: %.c\n\t@echo build $@ from $<; touch $@\nall: $(OBJS)\n", "a.c": "", "b.c": ""}, mk(), mk())
case("pattern-no-match", {"Makefile": "%.o: %.c\n\t@echo no\nall: x.o\n"}, mk())
case("prereq-glob", {"Makefile": "all: *.in\n\t@echo $^\n", "a.in": "", "b.in": ""}, mk())
case("goal-after-assign", {"Makefile": "A = 1\n.PHONY: p\nfirst:\n\t@echo first\nsecond:\n\t@echo second\n"}, mk())
case("dot-targets-not-goal", {"Makefile": ".hidden:\n\t@echo hidden\nreal:\n\t@echo real\n"}, mk())
case("two-rules-deps", {"Makefile": "a: b\na: c\na:\n\t@echo $^\nb c:\n\t@touch $@\n"}, mk())
case("jflag", {"Makefile": "all:\n\t@echo ok\n"}, mk("-j4"), mk("-j", "4"), mk("-j"))
case("combined-flags", {"Makefile": "all:\n\t@echo ok\n\tfalse\n"}, mk("-ki"), mk("-sk"))
case("not-remade-k", {"Makefile": "all: a\na:\n\tfalse\n"}, mk("-k"))

def run(exe, path_dirs, case_files, steps):
    d = tempfile.mkdtemp(prefix="maketest.")
    out = []
    try:
        for name, text in case_files.items():
            p = os.path.join(d, name)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").write(text)
            os.utime(p, (OLD, OLD))
        import time
        env = dict(os.environ, PATH=path_dirs, LC_ALL="C")
        for k in ("MAKEFLAGS", "MAKELEVEL", "MFLAGS", "FOO"): env.pop(k, None)
        for st in steps:
            if st[0] == "make":
                p = subprocess.run(["make"] + st[1], executable=exe, cwd=d, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=20)
                o = p.stdout.decode(errors="replace").replace(d, "TMP")
                out.append("$ make %s\n%s[rc=%d]\n" % (" ".join(st[1]), o, p.returncode))
            elif st[0] == "touch":
                t = {"old": OLD, "ancient": ANCIENT}[st[2]]
                os.utime(os.path.join(d, st[1]), (t, t))
            elif st[0] == "write":
                open(os.path.join(d, st[1]), "w").write(st[2])
            elif st[0] == "env":
                env[st[1]] = st[2]
        for root, dirs, fs in sorted(os.walk(d)):
            for f in sorted(fs):
                p = os.path.join(root, f)
                out.append("file %s %r\n" % (os.path.relpath(p, d), open(p, errors="replace").read()[:200]))
            for sub in sorted(dirs): out.append("dir %s\n" % os.path.relpath(os.path.join(root, sub), d))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return "".join(out)

def main():
    ours = os.path.abspath(sys.argv[1])
    flt = sys.argv[2] if len(sys.argv) > 2 else ""
    gnu = shutil.which("make")
    if not gnu: print("GNU make not found"); return 1
    bad = n = 0
    for name, files, steps in CASES:
        if flt not in name: continue
        n += 1
        a = run(gnu, os.environ["PATH"], files, steps)
        b = run(ours, os.path.dirname(ours) + ":" + os.environ["PATH"], files, steps)
        if a == b: print("PASS", name)
        else:
            bad += 1
            print("FAIL", name)
            import difflib
            print("".join(difflib.unified_diff(a.splitlines(1), b.splitlines(1), "gnu", "ours")))
    print("%d cases, %d failed" % (n, bad))
    return 1 if bad else 0
sys.exit(main())
