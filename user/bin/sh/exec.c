/* exec.c - running pipelines, redirections, jobs, builtins and scripts */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "sh.h"

int g_status, g_sargc, g_interactive, g_intr;
char **g_sargv;

#define MAXSTAGE 16
struct stage { char **argv; int argc; char **asg; int r[3]; int pid, st; };

/* every fd the shell opened for the current pipeline; closed once it has been launched */
static int ofd[96], nofd;
static int track(int fd) { if (nofd < 96) ofd[nofd++] = fd; return fd; }
static void close_tracked(void) { while (nofd) close(ofd[--nofd]); }

/* ---- background jobs ---- */
static struct job { int id, pid; char cmd[48]; } jobs[16];
static int njobs, nextjob = 1;

void reap_jobs(void) {
    int st, pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
        for (int i = 0; i < njobs; i++)
            if (jobs[i].pid == pid) {
                if (g_interactive) {
                    if (st) printf("[%d]+  Exit %d\t%s\n", jobs[i].id, st, jobs[i].cmd);
                    else printf("[%d]+  Done\t%s\n", jobs[i].id, jobs[i].cmd);
                }
                jobs[i] = jobs[--njobs];
                break;
            }
    if (!njobs) nextjob = 1;
}

/* ---- command lookup ---- */
const char *find_cmd(const char *name, char *buf, size_t n) {
    struct stat st;
    if (strchr(name, '/')) return stat(name, &st) == 0 ? name : NULL;
    const char *p = getenv("PATH");
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

/* ---- builtins ---- */
static int bi_cd(int argc, char **argv) {
    if (argc > 2) { fprintf(stderr, "sh: cd: too many arguments\n"); return 1; }
    const char *d = argc > 1 ? argv[1] : getenv("HOME");
    int dash = d && !strcmp(d, "-");
    if (dash && !(d = getenv("OLDPWD"))) { fprintf(stderr, "sh: cd: OLDPWD not set\n"); return 1; }
    if (!d) d = "/";
    char old[PATH_MAX], cur[PATH_MAX];
    if (!getcwd(old, sizeof old)) old[0] = 0;
    if (chdir(d) < 0) { fprintf(stderr, "sh: cd: %s: %s\n", d, strerror(errno)); return 1; }
    if (old[0]) setenv("OLDPWD", old, 1);
    if (getcwd(cur, sizeof cur)) { setenv("PWD", cur, 1); if (dash) puts(cur); }
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

static int bi_export(int argc, char **argv) {
    if (argc == 1) { for (char **e = environ; e && *e; e++) printf("export %s\n", *e); return 0; }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;                                  /* every variable is already exported */
        *eq = 0;
        setenv(argv[i], eq + 1, 1);
        *eq = '=';
    }
    return 0;
}

static int bi_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) unsetenv(argv[i]);
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
    printf("%sVibeOS sh%s - builtins\n", b, r);
    static const char *const h[][2] = {
        { "cd [dir|-]", "change directory (no arg = $HOME)" }, { "pwd", "print working directory" },
        { "export A=b", "set an environment variable" }, { "unset A", "remove a variable" },
        { "env", "print the environment" }, { "history [-c|N]", "show / clear / tail the history" },
        { "source f [args]", "run a script in this shell (also: . f)" }, { "which cmd...", "locate commands" },
        { "jobs", "list background jobs" }, { "clear", "clear the screen" },
        { "true / false", "exit 0 / 1" }, { "exit [n]", "leave the shell" }, { "help", "this text" },
    };
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) printf("  %s%-16s%s %s\n", b, h[i][0], r, h[i][1]);
    printf("%ssyntax%s  a | b   > >> < 2> 2>&1   ; && || &   'q' \"q\" \\x   $VAR ${VAR} $? $$   ~ * ? [a-z]\n", d, r);
    printf("%skeys%s    Tab complete  Up/Down history  ^A ^E home/end  ^U ^K ^W kill  ^L clear  ^D exit\n", d, r);
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

static int bi_source(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "sh: %s: filename argument required\n", argv[0]); return 2; }
    char buf[PATH_MAX];
    struct stat st;
    const char *p = argv[1];
    if (stat(p, &st) < 0 && !strchr(p, '/') && find_cmd(p, buf, sizeof buf)) p = buf;
    return run_source(p, argc > 2 ? argc - 1 : 0, argv + 1);
}

static int bi_jobs(int argc, char **argv) {
    for (int i = 0; i < njobs; i++) printf("[%d]+  Running\t%s\n", jobs[i].id, jobs[i].cmd);
    return 0;
}

static int bi_clear(int argc, char **argv) { fputs("\x1b[H\x1b[2J", stdout); return 0; }
static int bi_true(int argc, char **argv) { return 0; }
static int bi_false(int argc, char **argv) { return 1; }

static const struct { const char *name; int (*fn)(int, char **); } bt[] = {
    { "cd", bi_cd }, { "pwd", bi_pwd }, { "exit", bi_exit }, { "export", bi_export }, { "unset", bi_unset },
    { "env", bi_env }, { "history", bi_history }, { "help", bi_help }, { "source", bi_source }, { ".", bi_source },
    { "which", bi_which }, { "jobs", bi_jobs }, { "clear", bi_clear }, { "true", bi_true }, { "false", bi_false },
    { ":", bi_true },
};
#define NBT ((int)(sizeof bt / sizeof bt[0]))

const char *builtin_name(int i) { return i < NBT ? bt[i].name : NULL; }

static int (*builtin_fn(const char *name))(int, char **) {
    for (int i = 0; i < NBT; i++) if (!strcmp(bt[i].name, name)) return bt[i].fn;
    return NULL;
}
int is_builtin(const char *name) { return builtin_fn(name) != NULL; }

/* ---- pipelines ---- */
static int is_assign(const char *s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    const char *p = s;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    return *p == '=' ? (int)(p - s) : 0;
}

static int syntax_err(const char *near) {
    fprintf(stderr, "sh: syntax error near unexpected token `%s'\n", near);
    return -1;
}

/* expand one command's words, open its redirections; -1 on error (message already printed) */
static int build_stage(struct stage *s, struct tok *t, int n, int i, int ns, int *pipes) {
    struct vec av = { 0 }, asg = { 0 };
    int rc = 0;
    s->r[0] = s->r[1] = s->r[2] = -1;
    s->pid = -1;
    if (i > 0) s->r[0] = pipes[2 * (i - 1)];
    if (i < ns - 1) s->r[1] = pipes[2 * i + 1];
    for (int k = 0; k < n && !rc; k++) {
        struct tok *x = &t[k];
        if (x->type == T_WORD) {
            int eq = !av.n ? is_assign(x->s) : 0;
            if (eq) {
                char *v = expand_one(x->s + eq + 1), *a = malloc(eq + strlen(v) + 2);
                memcpy(a, x->s, eq + 1);
                strcpy(a + eq + 1, v);
                free(v);
                vec_add(&asg, a);
            } else expand_word(x->s, &av, 1);
        } else if (x->type == T_RDUP) {
            if (x->fd > 2 || x->arg > 2) { fprintf(stderr, "sh: only fds 0-2 can be redirected\n"); rc = -1; }
            else s->r[x->fd] = s->r[x->arg] >= 0 ? s->r[x->arg] : x->arg;
        } else {
            if (k + 1 >= n || t[k + 1].type != T_WORD) { rc = syntax_err("newline"); break; }
            char *f = expand_one(t[++k].s);
            int fl = x->type == T_RIN ? O_RDONLY : O_WRONLY | O_CREAT | (x->type == T_RAPP ? O_APPEND : O_TRUNC);
            if (x->fd > 2) { fprintf(stderr, "sh: only fds 0-2 can be redirected\n"); rc = -1; }
            else {
                int fd = open(f, fl, 0644);
                if (fd < 0) { fprintf(stderr, "sh: %s: %s\n", f, strerror(errno)); rc = -1; }
                else s->r[x->fd] = track(fd);
            }
            free(f);
        }
    }
    s->argv = av.v; s->argc = av.n; s->asg = asg.v;
    return rc;
}

static void free_stage(struct stage *s) {
    for (int i = 0; i < s->argc; i++) free(s->argv[i]);
    for (char **a = s->asg; a && *a; a++) free(*a);
    free(s->argv); free(s->asg);
}

static int spawn_stage(struct stage *s) {
    char buf[PATH_MAX];
    const char *name = s->argv[0], *path = find_cmd(name, buf, sizeof buf);
    struct stat st;
    if (!path) {
        fprintf(stderr, "sh: %s: %s\n", name, strchr(name, '/') ? "No such file or directory" : "command not found");
        return 127;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) { fprintf(stderr, "sh: %s: Is a directory\n", name); return 126; }
    struct spawn_opts o = { { s->r[0], s->r[1], s->r[2] }, NULL, 0 };
    int na = 0, ne = 0;
    while (s->asg && s->asg[na]) na++;
    while (environ && environ[ne]) ne++;
    char **envp = malloc((na + ne + 1) * sizeof *envp);      /* prefix assignments first: getenv finds the first match */
    for (int i = 0; i < na; i++) envp[i] = s->asg[i];
    for (int i = 0; i < ne; i++) envp[na + i] = environ[i];
    envp[na + ne] = 0;
    fflush(stdout);
    int pid = spawnve(path, s->argv, envp, &o);
    if (pid < 0 && errno == ENOEXEC) {                       /* no #! support: plain text runs under sh */
        char **av2 = malloc((s->argc + 2) * sizeof *av2);
        av2[0] = "sh"; av2[1] = (char *)path;
        for (int i = 1; i <= s->argc; i++) av2[i + 1] = s->argv[i];
        pid = spawnve("/bin/sh", av2, envp, &o);
        free(av2);
    }
    free(envp);
    if (pid < 0) { fprintf(stderr, "sh: %s: %s\n", name, strerror(errno)); return 126; }
    s->pid = pid;
    return 0;
}

static int run_builtin_stage(struct stage *s, int (*fn)(int, char **)) {
    int save[3] = { -1, -1, -1 };
    fflush(stdout);
    for (int i = 0; i < 3; i++)
        if (s->r[i] >= 0 && s->r[i] != i) { save[i] = dup(i); dup2(s->r[i], i); }
    int st = fn(s->argc, s->argv);
    fflush(stdout);
    for (int i = 0; i < 3; i++)
        if (save[i] >= 0) { dup2(save[i], i); close(save[i]); }
    return st;
}

static void set_fg(int pid) { ioctl(0, TIOCSPGRP, &pid); }

static int run_pipeline(struct tok *t, int n, int bg) {
    int lo[MAXSTAGE + 1], ns = 0, pipes[2 * MAXSTAGE] = { 0 };
    struct stage st[MAXSTAGE];
    lo[0] = 0;
    for (int k = 0; k < n; k++)
        if (t[k].type == T_PIPE) {
            if (ns + 1 >= MAXSTAGE) { fprintf(stderr, "sh: pipeline too long\n"); return 2; }
            lo[++ns] = k + 1;
        }
    ns++;
    lo[ns] = n + 1;
    for (int i = 0; i < ns; i++) {
        int e = lo[i + 1] - 1;
        if (e == lo[i] || t[lo[i]].type == T_PIPE) { syntax_err("|"); return 2; }
    }
    int bad = 0;
    for (int i = 0; i < ns - 1; i++) {
        int p[2];
        if (pipe(p) < 0) { fprintf(stderr, "sh: pipe: %s\n", strerror(errno)); bad = 1; break; }
        pipes[2 * i] = track(p[0]); pipes[2 * i + 1] = track(p[1]);
    }
    int built = 0;
    for (int i = 0; i < ns && !bad; i++) {
        built = i + 1;
        if (build_stage(&st[i], t + lo[i], lo[i + 1] - 1 - lo[i], i, ns, pipes) < 0) bad = 1;
    }
    if (bad) {
        for (int i = 0; i < built; i++) free_stage(&st[i]);
        close_tracked();
        return 1;
    }
    /* assignments only: set shell variables */
    if (ns == 1 && !st[0].argc) {
        for (char **a = st[0].asg; a && *a; a++) { char *eq = strchr(*a, '='); *eq = 0; setenv(*a, eq + 1, 1); }
        free_stage(&st[0]);
        close_tracked();
        return 0;
    }
    if (bg && st[0].r[0] < 0) st[0].r[0] = track(open("/dev/null", O_RDONLY));
    int last = 0, first = -1;
    for (int i = 0; i < ns; i++) {
        st[i].st = -1;
        if (!st[i].argc) { st[i].st = 0; continue; }
        if (is_builtin(st[i].argv[0])) continue;
        int e = spawn_stage(&st[i]);
        if (e) st[i].st = e;
        else if (first < 0) first = st[i].pid;
    }
    close_tracked();                                    /* children own their copies; readers now see EOF */
    if (!bg && first > 0) set_fg(first);
    for (int i = 0; i < ns; i++)
        if (st[i].st < 0 && st[i].pid < 0) {            /* builtin */
            fflush(stdout);
            st[i].st = run_builtin_stage(&st[i], builtin_fn(st[i].argv[0]));
        }
    if (bg) {
        for (int i = ns - 1; i >= 0; i--)
            if (st[i].pid > 0 && njobs < 16) {
                struct job *j = &jobs[njobs++];
                j->id = nextjob++; j->pid = st[i].pid;
                size_t w = 0;
                j->cmd[0] = 0;
                for (int k = 0; k < st[0].argc && w < sizeof j->cmd - 2; k++)
                    w += snprintf(j->cmd + w, sizeof j->cmd - w, "%s%s", k ? " " : "", st[0].argv[k]);
                if (g_interactive) printf("[%d] %d\n", j->id, j->pid);
                break;
            }
        last = 0;
    } else {
        int killed = 0;
        for (int i = 0; i < ns; i++) {
            if (st[i].pid > 0) {
                int s = 0;
                pid_t r;
                while ((r = waitpid(st[i].pid, &s, 0)) < 0 && errno == EINTR) {}
                st[i].st = r < 0 ? 1 : s & 0xff;
                if (st[i].st == 128 + SIGINT && !killed) {   /* ^C only reaches the first stage: take the rest down too */
                    killed = 1;
                    for (int j = i + 1; j < ns; j++) if (st[j].pid > 0) kill(st[j].pid, SIGKILL);
                }
            }
        }
        set_fg(getpid());
        last = killed ? 128 + SIGINT : st[ns - 1].st;
    }
    for (int i = 0; i < ns; i++) free_stage(&st[i]);
    return last;
}

int run_toks(struct toks *T) {
    int n = T->n, i = 0, skip = 0;
    struct tok *t = T->v;
    while (i < n) {
        int j = i;
        while (j < n && (t[j].type == T_WORD || t[j].type >= T_RIN || t[j].type == T_PIPE)) j++;
        int conn = j < n ? t[j].type : T_SEMI;
        if (j == i) {
            static const char *const nm[] = { "", "|", "&&", "||", ";", "&" };
            syntax_err(nm[t[j].type]);
            return g_status = 2;
        }
        if (!skip) g_status = run_pipeline(t + i, j - i, conn == T_BG);
        skip = conn == T_AND ? g_status != 0 : conn == T_OR ? g_status == 0 : 0;
        i = j + 1;
    }
    return g_status;
}

/* ---- reading commands from somewhere ---- */
int run_reader(char *(*next)(void *, int), void *ctx) {
    char *buf = NULL;
    int cont2 = 0;
    for (;;) {
        char *ln = next(ctx, buf != NULL);
        if (g_intr) {                                    /* ^C: drop whatever was half typed */
            g_intr = 0; g_status = 130;
            free(ln); free(buf); buf = NULL; cont2 = 0;
            continue;
        }
        if (!ln) {
            if (buf) { fprintf(stderr, "sh: syntax error: unexpected end of file\n"); g_status = 2; free(buf); }
            return g_status;
        }
        if (!buf) buf = ln;
        else {
            size_t a = strlen(buf), b = strlen(ln);
            if (cont2) a--;                              /* drop the backslash, join lines */
            buf = realloc(buf, a + b + 2);
            if (!cont2) buf[a++] = '\n';
            memcpy(buf + a, ln, b + 1);
            free(ln);
        }
        struct toks t = { 0 };
        int rc = lex(buf, &t);
        if (rc) { toks_free(&t); cont2 = rc == 2; continue; }
        if (g_interactive && buf[strspn(buf, " \t")]) {
            for (char *p = buf; *p; p++) if (*p == '\n') *p = ' ';
            hist_add(buf);
        }
        if (t.n) run_toks(&t);
        toks_free(&t);
        free(buf);
        buf = NULL;
        cont2 = 0;
    }
}

struct frd { FILE *f; char *buf; size_t cap; };
static char *file_next(void *ctx, int cont) {
    struct frd *r = ctx;
    ssize_t n = getline(&r->buf, &r->cap, r->f);
    if (n < 0) return NULL;
    while (n > 0 && (r->buf[n - 1] == '\n' || r->buf[n - 1] == '\r')) r->buf[--n] = 0;
    return strdup(r->buf);
}

int run_source(const char *path, int argc, char **argv) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "sh: %s: %s\n", path, strerror(errno)); return g_status = 1; }
    int oc = g_sargc, oi = g_interactive;
    char **ov = g_sargv;
    if (argc > 0) { g_sargc = argc; g_sargv = argv; }
    g_interactive = 0;
    struct frd r = { f, NULL, 0 };
    run_reader(file_next, &r);
    g_sargc = oc; g_sargv = ov; g_interactive = oi;
    free(r.buf);
    fclose(f);
    return g_status;
}

static char *str_next(void *ctx, int cont) {
    const char **p = ctx;
    if (!**p) return NULL;
    size_t n = strcspn(*p, "\n");
    char *r = strndup(*p, n);
    *p += n + ((*p)[n] == '\n');
    return r;
}

int run_lines(const char *text) {
    const char *p = text;
    return run_reader(str_next, &p);
}
