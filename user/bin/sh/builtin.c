/* builtin.c - builtin commands (they run inside the shell process) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sh.h"

/* ---- command lookup ---- */
const char *find_cmd(const char *name, char *buf, size_t n) {
    struct stat st;
    if (strchr(name, '/')) return stat(name, &st) == 0 ? name : NULL;
    const char *p = var_get("PATH");
    if (!p || !*p) p = "/bin";
    while (*p) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        if (dl + strlen(name) + 2 <= n) {
            memcpy(buf, p, dl);
            buf[dl] = '/';
            strcpy(buf + dl + 1, name);
            if (stat(dl ? buf : buf + 1, &st) == 0 && S_ISREG(st.st_mode)) return dl ? buf : buf + 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

/* ---- aliases ---- */
static struct alias { char *name, *val; } *al;
static int nal;
const char *alias_get(const char *name) {
    for (int i = 0; i < nal; i++) if (!strcmp(al[i].name, name)) return al[i].val;
    return NULL;
}

static int bi_alias(int argc, char **argv) {
    int rc = 0;
    if (argc == 1) for (int i = 0; i < nal; i++) printf("alias %s='%s'\n", al[i].name, al[i].val);
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = alias_get(argv[i]);
            if (v) printf("alias %s='%s'\n", argv[i], v); else { fprintf(stderr, "sh: alias: %s: not found\n", argv[i]); rc = 1; }
            continue;
        }
        char *nm = strndup(argv[i], eq - argv[i]);
        int k = 0;
        while (k < nal && strcmp(al[k].name, nm)) k++;
        if (k == nal) { al = realloc(al, (nal + 1) * sizeof *al); al[nal].name = nm; al[nal].val = NULL; nal++; }
        else free(nm);
        free(al[k].val);
        al[k].val = strdup(eq + 1);
    }
    return rc;
}

static int bi_unalias(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) { while (nal) { free(al[--nal].name); free(al[nal].val); } continue; }
        int k = 0;
        while (k < nal && strcmp(al[k].name, argv[i])) k++;
        if (k == nal) { fprintf(stderr, "sh: unalias: %s: not found\n", argv[i]); rc = 1; continue; }
        free(al[k].name); free(al[k].val);
        al[k] = al[--nal];
    }
    return rc;
}

/* ---- escapes shared by echo -e, printf ---- */
/* p points just after the backslash; stores the byte in *out, returns characters consumed (0 = unknown escape) */
static int esc(const char *p, char *out) {
    static const char from[] = "ntrabfve\\\"'", to[] = "\n\t\r\a\b\f\v\x1b\\\"'";
    const char *q = *p ? strchr(from, *p) : NULL;
    if (q) { *out = to[q - from]; return 1; }
    if (*p == 'x' && isxdigit((unsigned char)p[1])) {
        int v = 0, n = 1;
        for (; n <= 2 && isxdigit((unsigned char)p[n]); n++) v = v * 16 + (isdigit((unsigned char)p[n]) ? p[n] - '0' : (p[n] | 32) - 'a' + 10);
        *out = v;
        return n;
    }
    if (*p >= '0' && *p <= '7') {
        int v = 0, n = 0;
        for (; n < 3 && p[n] >= '0' && p[n] <= '7'; n++) v = v * 8 + p[n] - '0';
        *out = v;
        return n;
    }
    return 0;
}

static int bi_echo(int argc, char **argv) {
    int nl = 1, ee = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        while (*f == 'n' || *f == 'e' || *f == 'E') f++;
        if (*f) break;
        for (f = argv[i] + 1; *f; f++) { if (*f == 'n') nl = 0; else ee = *f == 'e'; }
    }
    for (; i < argc; i++) {
        const char *s = argv[i];
        for (; *s; s++) {
            char c;
            int n;
            if (ee && *s == '\\' && s[1] == 'c') return fflush(stdout), 0;
            if (ee && *s == '\\' && (n = esc(s + 1, &c))) { putchar(c); s += n; }
            else putchar(*s);
        }
        if (i + 1 < argc) putchar(' ');
    }
    if (nl) putchar('\n');
    return 0;
}

static int bi_printf(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "sh: printf: usage: printf format [arguments]\n"); return 1; }
    const char *fmt = argv[1];
    int ai = 2, rc = 0;
    for (;;) {
        int used = 0;
        for (const char *p = fmt; *p; p++) {
            char c;
            int n;
            if (*p == '\\' && (n = esc(p + 1, &c))) { putchar(c); p += n; continue; }
            if (*p != '%') { putchar(*p); continue; }
            if (p[1] == '%') { putchar('%'); p++; continue; }
            char spec[32];
            int k = 0;
            spec[k++] = *p++;
            while (*p && strchr("-+ #0", *p) && k < 20) spec[k++] = *p++;
            while ((isdigit((unsigned char)*p) || *p == '.') && k < 28) spec[k++] = *p++;
            const char *arg = ai < argc ? argv[ai] : NULL;
            char conv = *p;
            if (!conv) break;
            used |= arg != NULL;
            if (arg) ai++;
            if (!arg) arg = conv == 's' || conv == 'b' || conv == 'c' ? "" : "0";
            char *e = NULL;
            if (strchr("diouxX", conv)) {
                long v = arg[0] == '\'' || arg[0] == '"' ? (unsigned char)arg[1] : strtol(arg, &e, 0);
                if (e && *e) { fprintf(stderr, "sh: printf: %s: invalid number\n", arg); rc = 1; }
                spec[k++] = 'l'; spec[k++] = conv == 'i' ? 'd' : conv; spec[k] = 0;
                printf(spec, v);
            } else if (conv == 'c') { spec[k++] = 'c'; spec[k] = 0; printf(spec, *arg); }
            else if (conv == 's') { spec[k++] = 's'; spec[k] = 0; printf(spec, arg); }
            else if (conv == 'b') {
                struct sb b = { 0 };
                for (const char *s = arg; *s; s++) {
                    if (*s == '\\' && (n = esc(s + 1, &c))) { sb_add(&b, c); s += n; } else sb_add(&b, *s);
                }
                spec[k++] = 's'; spec[k] = 0;
                printf(spec, b.p ? b.p : "");
                free(b.p);
            } else { fprintf(stderr, "sh: printf: %%%c: unsupported conversion\n", conv); return 1; }
        }
        if (ai >= argc || !used) break;
    }
    return rc;
}

/* ---- test / [ ---- */
static char **ta;
static int tn, ti, terr;
static int t_or(void);
static int isbin(const char *s) {
    static const char *const o[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
    for (unsigned i = 0; i < sizeof o / sizeof *o; i++) if (!strcmp(s, o[i])) return 1;
    return 0;
}
static int tint(const char *s, long *v) {
    char *e;
    *v = strtol(s, &e, 10);
    if (!*s || *e) { fprintf(stderr, "sh: test: %s: integer expression expected\n", s); terr = 1; return 0; }
    return 1;
}
static int t_prim(void) {
    if (ti >= tn) { terr = 1; return 0; }
    if (!strcmp(ta[ti], "(")) {
        ti++;
        int v = t_or();
        if (ti < tn && !strcmp(ta[ti], ")")) ti++; else terr = 1;
        return v;
    }
    if (ti + 2 < tn && isbin(ta[ti + 1])) {
        const char *a = ta[ti], *o = ta[ti + 1], *b = ta[ti + 2];
        ti += 3;
        if (!strcmp(o, "=") || !strcmp(o, "==")) return !strcmp(a, b);
        if (!strcmp(o, "!=")) return strcmp(a, b) != 0;
        long x, y;
        if (!tint(a, &x) || !tint(b, &y)) return 0;
        switch (o[1]) {
        case 'e': return x == y;
        case 'n': return x != y;
        case 'l': return o[2] == 't' ? x < y : x <= y;
        default: return o[2] == 't' ? x > y : x >= y;
        }
    }
    if (ti + 1 < tn && ta[ti][0] == '-' && ta[ti][1] && !ta[ti][2] && strchr("efdsznrwxL", ta[ti][1])) {
        char op = ta[ti][1];
        const char *a = ta[ti + 1];
        ti += 2;
        struct stat st;
        switch (op) {
        case 'z': return !*a;
        case 'n': return *a != 0;
        case 'e': case 'r': case 'w': case 'x': return stat(a, &st) == 0;
        case 'f': return stat(a, &st) == 0 && S_ISREG(st.st_mode);
        case 'd': return stat(a, &st) == 0 && S_ISDIR(st.st_mode);
        case 's': return stat(a, &st) == 0 && st.st_size > 0;
        default: return 0;
        }
    }
    return *ta[ti++] != 0;
}
static int t_not(void) {
    if (ti + 1 < tn && !strcmp(ta[ti], "!")) { ti++; return !t_not(); }
    return t_prim();
}
static int t_and(void) {
    int v = t_not();
    while (ti < tn && !strcmp(ta[ti], "-a")) { ti++; int r = t_not(); v = v && r; }
    return v;
}
static int t_or(void) {
    int v = t_and();
    while (ti < tn && !strcmp(ta[ti], "-o")) { ti++; int r = t_and(); v = v || r; }
    return v;
}
static int bi_test(int argc, char **argv) {
    ta = argv + 1; tn = argc - 1; ti = 0; terr = 0;
    if (!strcmp(argv[0], "[")) {
        if (tn < 1 || strcmp(ta[tn - 1], "]")) { fprintf(stderr, "sh: [: missing ]\n"); return 2; }
        tn--;
    }
    if (!tn) return 1;
    int v = t_or();
    if (terr || ti < tn) { if (!terr) fprintf(stderr, "sh: test: syntax error\n"); return 2; }
    return !v;
}

/* ---- read, shift, set, eval, exec ---- */
static int bi_read(int argc, char **argv) {
    const char *prompt = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) prompt = argv[++i];
        else if (strcmp(argv[i], "-r")) break;
    }
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    struct sb line = { 0 };
    int got = 0;
    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got = 1;
        if (c == '\n') break;
        sb_add(&line, c);
    }
    const char *sp = var_get("IFS");
    if (!sp) sp = " \t\n";
    const char *p = line.p ? line.p : "";
    char *none[] = { "REPLY", NULL };
    char **names = i < argc ? argv + i : none;
    int nn = i < argc ? argc - i : 1;
    for (int k = 0; k < nn; k++) {
        while (*p && strchr(sp, *p) && strchr(" \t\n", *p)) p++;
        const char *s = p;
        if (k == nn - 1) {
            p += strlen(p);
            while (p > s && strchr(sp, p[-1]) && strchr(" \t\n", p[-1])) p--;
        } else while (*p && !strchr(sp, *p)) p++;
        char *v = strndup(s, p - s);
        var_set(names[k], v);
        free(v);
        if (*p && k < nn - 1) p++;
    }
    free(line.p);
    return got ? 0 : 1;
}

static int bi_shift(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 0 || n > g_sargc - 1) { fprintf(stderr, "sh: shift: can't shift that many\n"); return 1; }
    for (int i = 1; i <= n; i++) free(g_sargv[i]);
    memmove(g_sargv + 1, g_sargv + 1 + n, (g_sargc - n) * sizeof *g_sargv);   /* includes the NULL terminator */
    g_sargc -= n;
    return 0;
}

void set_options(const char *f) {
    int on = *f == '-';
    for (f++; *f; f++) {
        if (*f == 'e') g_errexit = on;
        else if (*f == 'x') g_xtrace = on;
    }
}

static int pr_var(const char *name, const char *val, int exported) { printf("%s=%s\n", name, val); return 0; }

static int bi_set(int argc, char **argv) {
    int i = 1;
    if (argc == 1) { var_list(pr_var); return 0; }
    for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        set_options(argv[i]);
    }
    if (i < argc || (i > 1 && !strcmp(argv[i - 1], "--"))) {
        char **na = malloc((argc - i + 2) * sizeof *na);
        int k = 0;
        na[k++] = g_sargc ? g_sargv[0] : "sh";
        for (; i < argc; i++) na[k++] = argv[i];
        set_pos(k, na);
        free(na);
    }
    return 0;
}

static int bi_eval(int argc, char **argv) {
    struct sb b = { 0 };
    for (int i = 1; i < argc; i++) { if (i > 1) sb_add(&b, ' '); sb_adds(&b, argv[i]); }
    int rc = b.p ? run_text(b.p) : 0;
    free(b.p);
    return rc;
}

static int bi_exec(int argc, char **argv) {
    if (argc < 2) return 0;
    char buf[PATH_MAX];
    const char *path = find_cmd(argv[1], buf, sizeof buf);
    if (!path) { fprintf(stderr, "sh: exec: %s: not found\n", argv[1]); return 127; }
    char **envp = var_envp(NULL, 0);
    fflush(stdout);
    int pid = spawnve(path, argv + 1, envp, NULL);
    if (pid < 0) { fprintf(stderr, "sh: exec: %s: %s\n", argv[1], strerror(errno)); return 126; }
    exit(wait_pid(pid));                                   /* no real exec: spawn, wait, exit with its status */
}

/* ---- the classic ones ---- */
static int bi_cd(int argc, char **argv) {
    if (argc > 2) { fprintf(stderr, "sh: cd: too many arguments\n"); return 1; }
    const char *d = argc > 1 ? argv[1] : var_get("HOME");
    int dash = d && !strcmp(d, "-");
    if (dash && !(d = var_get("OLDPWD"))) { fprintf(stderr, "sh: cd: OLDPWD not set\n"); return 1; }
    if (!d) d = "/";
    char old[PATH_MAX], cur[PATH_MAX];
    if (!getcwd(old, sizeof old)) old[0] = 0;
    if (chdir(d) < 0) { fprintf(stderr, "sh: cd: %s: %s\n", d, strerror(errno)); return 1; }
    if (old[0]) var_export("OLDPWD", old);
    if (getcwd(cur, sizeof cur)) { var_export("PWD", cur); if (dash) puts(cur); }
    return 0;
}

static int bi_pwd(int argc, char **argv) {
    char cur[PATH_MAX];
    if (!getcwd(cur, sizeof cur)) { fprintf(stderr, "sh: pwd: %s\n", strerror(errno)); return 1; }
    puts(cur);
    return 0;
}

static int bi_exit(int argc, char **argv) {
    fflush(stdout);
    exit(argc > 1 ? atoi(argv[1]) & 255 : g_status);
}

static int pr_export(const char *name, const char *val, int exported) {
    if (exported) printf("export %s='%s'\n", name, val);
    return 0;
}

static int bi_export(int argc, char **argv) {
    if (argc == 1 || !strcmp(argv[1], "-p")) { var_list(pr_export); return 0; }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; var_export(argv[i], eq + 1); *eq = '='; }
        else var_export(argv[i], NULL);
    }
    return 0;
}

static int bi_unset(int argc, char **argv) {
    int f = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) f = 1;
        else if (!strcmp(argv[i], "-v")) f = 0;
        else if (f) fn_unset(argv[i]);
        else var_unset(argv[i]);
    }
    return 0;
}

static int bi_env(int argc, char **argv) {
    for (char **e = environ; e && *e; e++) puts(*e);
    return 0;
}

static int bi_history(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-c")) { hist_clear(); return 0; }
    int n = hist_count(), from = argc > 1 ? n - atoi(argv[1]) : 0;
    for (int i = from < 0 ? 0 : from; i < n; i++) printf("%5d  %s\n", i + 1, hist_get(i));
    return 0;
}

static int bi_help(int argc, char **argv) {
    int c = isatty(1);
    const char *b = c ? "\x1b[1;38;2;139;233;253m" : "", *d = c ? "\x1b[2m" : "", *r = c ? "\x1b[0m" : "";
    printf("%sVibeOS sh%s - a small POSIX-style shell\n", b, r);
    static const char *const h[][2] = {
        { "cd [dir|-]  pwd", "change / print the working directory" },
        { "export [A=b]", "mark (and set) an environment variable; plain A=b stays local" },
        { "unset [-f] A", "remove a variable (or function)" },
        { "set [-ex] [-- args]", "options (-e exit on error, -x trace) / positional params" },
        { "shift [n]  local x", "drop positional params / function-local variable" },
        { "read [-p s] a b", "read a line from stdin into variables" },
        { "test / [ ... ]", "-e -f -d -s -z -n = != -eq -ne -lt -le -gt -ge ! -a -o ( )" },
        { "echo [-ne]  printf", "print text / formatted (%s %d %x %o %c %b, width, precision)" },
        { "eval  exec  source", "run text / replace / run a script here (also: . f)" },
        { "alias  unalias", "command-name shortcuts" },
        { "type  which", "how a name resolves" },
        { "return  break  continue", "leave function / loop [n]" },
        { "wait [pid]  jobs", "wait for / list background jobs" },
        { "history [-c|N]", "show / clear the history" },
        { "true false : exit clear", "" },
    };
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) printf("  %s%-24s%s %s\n", b, h[i][0], r, h[i][1]);
    printf("%scommands%s  a | b   > >> < 2>&1 <<EOF <<'EOF' <<-EOF   ; && || & !   ( list )  { list; }\n", d, r);
    printf("%s         %s  if..elif..else..fi  while/until..do..done  for x in w; do..done  case w in p|q) ..;; esac\n", d, r);
    printf("%s         %s  name() { ...; }   $(cmd) `cmd`   $((1+2))   ${v:-d} ${v:=d} ${v:+d} ${#v} ${v#p} ${v%%p}\n", d, r);
    printf("%squoting%s   'q' \"q\" \\x   $VAR $? $$ $! $# $@ $*   ~ * ? [a-z]    scripts may start with #!/bin/sh\n", d, r);
    printf("%skeys%s      Tab complete  Up/Down history  ^R search  !! !prefix !N  ^A ^E  ^U ^K ^W  ^L  ^D exit\n", d, r);
    printf("%sdemo%s      sh /home/demo.sh        regression test: sh /home/shtest.sh\n", d, r);
    return 0;
}

static int bi_which(int argc, char **argv) {
    int rc = 0;
    char buf[PATH_MAX];
    for (int i = 1; i < argc; i++) {
        const char *p;
        if (is_builtin(argv[i])) printf("%s: shell builtin\n", argv[i]);
        else if ((p = find_cmd(argv[i], buf, sizeof buf))) puts(p);
        else { printf("%s not found\n", argv[i]); rc = 1; }
    }
    return rc;
}

static int bi_type(int argc, char **argv) {
    static const char *const kw[] = { "if", "then", "elif", "else", "fi", "while", "until", "do", "done", "for", "in", "case", "esac", "{", "}", "!" };
    int rc = 0;
    char buf[PATH_MAX];
    for (int i = 1; i < argc; i++) {
        const char *p, *n = argv[i];
        int isk = 0;
        for (unsigned k = 0; k < sizeof kw / sizeof *kw; k++) if (!strcmp(kw[k], n)) isk = 1;
        if ((p = alias_get(n))) printf("%s is aliased to `%s'\n", n, p);
        else if (isk) printf("%s is a shell keyword\n", n);
        else if (fn_exists(n)) printf("%s is a function\n", n);
        else if (is_builtin(n)) printf("%s is a shell builtin\n", n);
        else if ((p = find_cmd(n, buf, sizeof buf))) printf("%s is %s\n", n, p);
        else { printf("%s: not found\n", n); rc = 1; }
    }
    return rc;
}

static int bi_source(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "sh: %s: filename argument required\n", argv[0]); return 2; }
    char buf[PATH_MAX];
    struct stat st;
    const char *p = argv[1];
    if (stat(p, &st) < 0 && !strchr(p, '/') && find_cmd(p, buf, sizeof buf)) p = buf;
    return run_source(p, argc > 2 ? argc - 1 : 0, argv + 1);
}

static int bi_jobs(int argc, char **argv) { jobs_print(); return 0; }
static int bi_wait(int argc, char **argv) { return jobs_wait(argc > 1 ? atoi(argv[1]) : 0); }
static int bi_clear(int argc, char **argv) { fputs("\x1b[H\x1b[2J", stdout); return 0; }
static int bi_true(int argc, char **argv) { return 0; }
static int bi_false(int argc, char **argv) { return 1; }

static int bi_local(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) *eq = 0;
        local_declare(argv[i]);
        if (eq) { var_set(argv[i], eq + 1); *eq = '='; }
        else var_unset(argv[i]);
    }
    return 0;
}

static int bi_return(int argc, char **argv) {
    g_status = argc > 1 ? atoi(argv[1]) & 255 : g_status;
    g_ctl = CTL_RET;
    return g_status;
}
static int bi_break(int argc, char **argv) {
    g_ctl = argv[0][0] == 'b' ? CTL_BREAK : CTL_CONT;
    g_ctln = argc > 1 && atoi(argv[1]) > 0 ? atoi(argv[1]) : 1;
    return 0;
}

static const struct { const char *name; int (*fn)(int, char **); } bt[] = {
    { "cd", bi_cd }, { "pwd", bi_pwd }, { "exit", bi_exit }, { "export", bi_export }, { "unset", bi_unset },
    { "env", bi_env }, { "history", bi_history }, { "help", bi_help }, { "source", bi_source }, { ".", bi_source },
    { "which", bi_which }, { "jobs", bi_jobs }, { "clear", bi_clear }, { "true", bi_true }, { "false", bi_false },
    { ":", bi_true }, { "echo", bi_echo }, { "printf", bi_printf }, { "test", bi_test }, { "[", bi_test },
    { "read", bi_read }, { "shift", bi_shift }, { "set", bi_set }, { "eval", bi_eval }, { "exec", bi_exec },
    { "type", bi_type }, { "alias", bi_alias }, { "unalias", bi_unalias }, { "wait", bi_wait },
    { "local", bi_local }, { "return", bi_return }, { "break", bi_break }, { "continue", bi_break },
};
#define NBT ((int)(sizeof bt / sizeof bt[0]))

const char *builtin_name(int i) { return i < NBT ? bt[i].name : NULL; }

int is_builtin(const char *name) {
    for (int i = 0; i < NBT; i++) if (!strcmp(bt[i].name, name)) return 1;
    return 0;
}

int run_builtin(int argc, char **argv) {
    for (int i = 0; i < NBT; i++) if (!strcmp(bt[i].name, argv[0])) return bt[i].fn(argc, argv);
    return 127;
}
