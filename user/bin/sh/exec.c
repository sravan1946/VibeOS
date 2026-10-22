/* exec.c - the evaluator: AST walking, pipelines, redirections, functions, jobs, child shells, script readers.
 *
 * There is no fork, so anything that must run "in another process" (a ( subshell ), a compound command or a
 * function inside a pipeline, `cmd &` for non-simple commands, $(...) ) is run as a child `sh -c TEXT`.  The child
 * inherits the exported environment, plus our plain variables (as exported ones), the function definitions
 * (re-sent as text), the positional parameters and the -e/-x flags.  Everything else runs in-process. */
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
#include "vutil.h"

int g_status, g_sargc, g_interactive, g_intr, g_ctl, g_ctln, g_errexit, g_xtrace, g_noerr, g_lastbg;
char **g_sargv;

#define MAXSTAGE 16
#define MAXFD 96

/* fds the shell opened for one command/pipeline; closed once they are no longer needed */
struct fdl { int fd[MAXFD], n; };
static int track(struct fdl *l, int fd) { if (fd >= 0 && l->n < MAXFD) l->fd[l->n++] = fd; return fd; }
static void untrack(struct fdl *l, int fd) {
    for (int i = 0; i < l->n; i++) if (l->fd[i] == fd) { close(fd); l->fd[i] = l->fd[--l->n]; return; }
}
static void close_all(struct fdl *l) { while (l->n) close(l->fd[--l->n]); }

static int cs_status, cs_used;             /* status of the last $(...) (for "x=$(cmd)") */

static void set_fg(int pid) { ioctl(0, TIOCSPGRP, &pid); }

int wait_pid(int pid) {
    int s = 0;
    pid_t r;
    while ((r = waitpid(pid, &s, 0)) < 0 && errno == EINTR) {}
    return r < 0 ? 127 : s & 0xff;
}

/* ---- background jobs ---- */
static struct job { int id, pid; char cmd[48]; } jobs[16];
static int njobs, nextjob = 1;

static void job_del(int i) { jobs[i] = jobs[--njobs]; if (!njobs) nextjob = 1; }

void reap_jobs(void) {
    int st, pid;
    while (njobs && (pid = waitpid(-1, &st, WNOHANG)) > 0)
        for (int i = 0; i < njobs; i++)
            if (jobs[i].pid == pid) {
                if (g_interactive) {
                    if (st) printf("[%d]+  Exit %d\t%s\n", jobs[i].id, st, jobs[i].cmd);
                    else printf("[%d]+  Done\t%s\n", jobs[i].id, jobs[i].cmd);
                }
                job_del(i);
                break;
            }
}

void jobs_print(void) { for (int i = 0; i < njobs; i++) printf("[%d]+  Running\t%s\n", jobs[i].id, jobs[i].cmd); }

int jobs_wait(int pid) {
    int rc = 0;
    if (pid) {
        rc = wait_pid(pid);
        for (int i = 0; i < njobs; i++) if (jobs[i].pid == pid) { job_del(i); break; }
        return rc;
    }
    while (njobs) { wait_pid(jobs[0].pid); job_del(0); }
    return 0;
}

static void job_add(int pid, const char *cmd) {
    g_lastbg = pid;
    if (njobs >= 16) return;
    struct job *j = &jobs[njobs++];
    j->id = nextjob++; j->pid = pid;
    snprintf(j->cmd, sizeof j->cmd, "%s", cmd);
    for (char *p = j->cmd; *p; p++) if (*p == '\n') *p = ' ';
    if (g_interactive) printf("[%d] %d\n", j->id, j->pid);
}

/* ---- functions (parsed once, kept until redefined) ---- */
struct fn { char *name, *src; struct prog p; int busy; };
static struct fn **fns;
static int nfn, fdepth;

static int fn_idx(const char *name) {
    for (int i = 0; i < nfn; i++) if (!strcmp(fns[i]->name, name)) return i;
    return -1;
}
int fn_exists(const char *name) { return fn_idx(name) >= 0; }

int fn_define(const char *name, const char *src) {
    struct prog p;
    if (parse_text(src, &p) != P_OK) { fprintf(stderr, "sh: bad function body for %s\n", name); return 1; }
    int i = fn_idx(name);
    struct fn *f;
    if (i >= 0) { f = fns[i]; if (!f->busy) prog_free(&f->p); free(f->src); }    /* busy: leak the old tree, it is running */
    else {
        f = calloc(1, sizeof *f);
        f->name = strdup(name);
        fns = realloc(fns, (nfn + 1) * sizeof *fns);
        fns[nfn++] = f;
    }
    f->p = p; f->src = strdup(src);
    return 0;
}

void fn_unset(const char *name) {
    int i = fn_idx(name);
    if (i < 0) return;
    struct fn *f = fns[i];
    if (!f->busy) prog_free(&f->p);
    free(f->name); free(f->src); free(f);
    fns[i] = fns[--nfn];
}

void fn_list(void (*cb)(const char *name, const char *src)) { for (int i = 0; i < nfn; i++) cb(fns[i]->name, fns[i]->src); }

/* `local`: remember the old value, restore it when the function returns */
static struct lsave { char *name, *old; int exp; } *lsv;
static int nls;
void local_mark(int *mark) { *mark = nls; }
void local_declare(const char *name) {
    const char *o = var_get(name);
    lsv = realloc(lsv, (nls + 1) * sizeof *lsv);
    lsv[nls].name = strdup(name);
    lsv[nls].old = o ? strdup(o) : NULL;
    lsv[nls].exp = var_exported(name);
    nls++;
}
void local_release(int mark) {
    while (nls > mark) {
        struct lsave *l = &lsv[--nls];
        var_unset(l->name);
        if (l->old) { if (l->exp) var_export(l->name, l->old); else var_set(l->name, l->old); }
        free(l->name); free(l->old);
    }
}

int call_function(const char *name, int argc, char **argv, int *found) {
    int i = fn_idx(name);
    *found = i >= 0;
    if (i < 0) return 127;
    if (fdepth >= 100) { fprintf(stderr, "sh: %s: function nesting too deep\n", name); return g_status = 1; }
    struct fn *f = fns[i];
    int oc = g_sargc, mark;
    char **ov = g_sargv, **na = malloc((argc + 1) * sizeof *na);
    na[0] = oc ? ov[0] : "sh";
    for (int k = 1; k < argc; k++) na[k] = argv[k];
    g_sargv = NULL; g_sargc = 0;                       /* keep the caller's vector alive across set_pos */
    set_pos(argc, na);
    free(na);
    local_mark(&mark);
    f->busy++; fdepth++;
    run_node(f->p.root);
    fdepth--; f->busy--;
    local_release(mark);
    if (g_ctl == CTL_RET) g_ctl = CTL_NONE;
    set_pos(0, NULL);
    g_sargv = ov; g_sargc = oc;
    return g_status;
}

/* ---- child shells ---- */
static void add_def(const char *name, const char *src, struct sb *b) {
    sb_adds(b, name); sb_adds(b, "() {\n"); sb_adds(b, src); sb_adds(b, "\n}\n");
}
static struct sb *defsb;
static void def_cb(const char *name, const char *src) { add_def(name, src, defsb); }

int spawn_sh(const char *text, int fds[3], int *pid) {
    struct sb t = { 0 };
    defsb = &t;
    fn_list(def_cb);
    sb_adds(&t, text);
    struct vec av = { 0 };
    char fl[4] = "-";
    if (g_errexit) strcat(fl, "e");
    if (g_xtrace) strcat(fl, "x");
    vec_add(&av, "sh");
    if (fl[1]) vec_add(&av, fl);
    vec_add(&av, "-c");
    vec_add(&av, t.p ? t.p : (char *)"");
    if (g_sargc) for (int i = 0; i < g_sargc; i++) vec_add(&av, g_sargv[i]);
    else vec_add(&av, "sh");
    char **envp = var_envp(NULL, 1);
    struct spawn_opts o = { { fds[0], fds[1], fds[2] }, NULL, 0 };
    fflush(stdout);
    int p = spawnve("/bin/sh", av.v, envp, &o);
    int e = errno;
    free(av.v); free(envp); free(t.p);
    if (p < 0) { fprintf(stderr, "sh: cannot start subshell: %s\n", strerror(e)); return -1; }
    *pid = p;
    return 0;
}

char *cmdsub(const char *text) {
    int p[2], pid;
    cs_used = 1;
    if (pipe(p) < 0) { cs_status = 1; return strdup(""); }
    int fds[3] = { -1, p[1], -1 };
    if (spawn_sh(text, fds, &pid) < 0) { close(p[0]); close(p[1]); cs_status = 127; return strdup(""); }
    close(p[1]);
    size_t n = 0;
    char *b = readall(p[0], &n);
    close(p[0]);
    cs_status = wait_pid(pid);
    set_fg(getpid());
    if (!b) return strdup("");
    while (n > 0 && b[n - 1] == '\n') b[--n] = 0;
    return b;
}

/* ---- command lookup, redirections ---- */
static int is_assign(const char *s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    const char *p = s;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    return *p == '=' ? (int)(p - s) : 0;
}

struct stage { struct node *n; char **argv; int argc; struct vec asg; int r[3]; int pid, st, kind; };
enum { K_NONE, K_EXT, K_INPROC, K_CHILD };

static int open_redirs(struct redir *rd, int r[3], struct fdl *l) {
    for (; rd; rd = rd->next) {
        if (rd->fd > 2 || (rd->type == R_DUP && rd->arg > 2)) { fprintf(stderr, "sh: only fds 0-2 can be redirected\n"); return -1; }
        if (rd->type == R_DUP) r[rd->fd] = r[rd->arg] >= 0 ? r[rd->arg] : rd->arg;
        else if (rd->type == R_HERE) {
            char *body = rd->quoted ? strdup(rd->word) : expand_here(rd->word);
            int p[2];
            if (strlen(body) > 8000) { fprintf(stderr, "sh: here-document too large\n"); free(body); return -1; }
            if (pipe(p) < 0) { fprintf(stderr, "sh: pipe: %s\n", strerror(errno)); free(body); return -1; }
            write_all(p[1], body, strlen(body));                 /* fits the pipe buffer: never blocks */
            close(p[1]);
            free(body);
            r[rd->fd] = track(l, p[0]);
        } else {
            char *f = expand_one(rd->word);
            int fl = rd->type == R_IN ? O_RDONLY : O_WRONLY | O_CREAT | (rd->type == R_APP ? O_APPEND : O_TRUNC);
            int fd = open(f, fl, 0644);
            if (fd < 0) { fprintf(stderr, "sh: %s: %s\n", f, strerror(errno)); free(f); return -1; }
            free(f);
            r[rd->fd] = track(l, fd);
        }
    }
    return 0;
}

static void apply_fds(const int r[3], int save[3]) {
    fflush(stdout);
    for (int i = 0; i < 3; i++) {
        save[i] = -1;
        if (r[i] >= 0 && r[i] != i) { save[i] = dup(i); dup2(r[i], i); }
    }
}
static void restore_fds(int save[3]) {
    fflush(stdout);
    for (int i = 0; i < 3; i++) if (save[i] >= 0) { dup2(save[i], i); close(save[i]); }
}

static void xtrace(char **argv, int argc) {
    if (!g_xtrace) return;
    fputs("+", stderr);
    for (int i = 0; i < argc; i++) fprintf(stderr, " %s", argv[i]);
    fputc('\n', stderr);
}

/* expand a simple command's words, open its redirections; -1 on error (message already printed) */
static int build_stage(struct stage *s, struct node *n, int i, int ns, int *pipes, struct fdl *l) {
    struct vec av = { 0 };
    s->n = n; s->asg = (struct vec){ 0 };
    s->r[0] = s->r[1] = s->r[2] = -1;
    s->pid = -1; s->kind = K_NONE;
    if (i > 0) s->r[0] = pipes[2 * (i - 1)];
    if (i < ns - 1) s->r[1] = pipes[2 * i + 1];
    if (n->type == N_CMD)
        for (char **w = n->w; *w; w++) {
            int eq = !av.n ? is_assign(*w) : 0;
            if (eq) {
                char *v = expand_one(*w + eq + 1), *a = malloc(eq + strlen(v) + 2);
                memcpy(a, *w, eq + 1);
                strcpy(a + eq + 1, v);
                free(v);
                vec_add(&s->asg, a);
            } else expand_word(*w, &av, M_ARGV);
        }
    s->argv = av.v; s->argc = av.n;
    return open_redirs(n->rd, s->r, l);
}

static void free_stage(struct stage *s) {
    for (int i = 0; i < s->argc; i++) free(s->argv[i]);
    for (int i = 0; i < s->asg.n; i++) free(s->asg.v[i]);
    free(s->argv); free(s->asg.v);
    s->argv = NULL; s->asg.v = NULL; s->argc = s->asg.n = 0;
}

static int spawn_ext(struct stage *s) {
    char buf[PATH_MAX];
    const char *name = s->argv[0], *path = find_cmd(name, buf, sizeof buf);
    struct stat st;
    if (!path) {
        fprintf(stderr, "sh: %s: %s\n", name, strchr(name, '/') ? "No such file or directory" : "command not found");
        return 127;
    }
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) { fprintf(stderr, "sh: %s: Is a directory\n", name); return 126; }
    struct spawn_opts o = { { s->r[0], s->r[1], s->r[2] }, NULL, 0 };
    char **envp = var_envp(&s->asg, 0);
    fflush(stdout);
    int pid = spawnve(path, s->argv, envp, &o);       /* the kernel handles #! lines */
    if (pid < 0 && errno == ENOEXEC) {                /* plain text without #!: runs under sh */
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

/* run a simple command's builtin / function in this process (fds already applied) */
static int run_inproc(struct stage *s) {
    int n = s->asg.n, found, st;
    char **nm = calloc(n + 1, sizeof *nm), **old = calloc(n + 1, sizeof *old);
    for (int i = 0; i < n; i++) {                       /* IFS= read x : prefix assignments last for this command only */
        char *eq = strchr(s->asg.v[i], '=');
        const char *o;
        nm[i] = strndup(s->asg.v[i], eq - s->asg.v[i]);
        o = var_get(nm[i]);
        old[i] = o ? strdup(o) : NULL;
        var_set(nm[i], eq + 1);
    }
    st = call_function(s->argv[0], s->argc, s->argv, &found);
    if (!found) st = run_builtin(s->argc, s->argv);
    for (int i = n - 1; i >= 0; i--) {
        if (old[i]) var_set(nm[i], old[i]); else var_unset(nm[i]);
        free(nm[i]); free(old[i]);
    }
    free(nm); free(old);
    return st;
}

static const char *stage_text(struct node *n) { return n->type == N_SUB ? n->s : n->src; }

static void leaf_done(int st) {
    if (g_errexit && !g_noerr && st) { fflush(stdout); exit(st & 255); }
}

static int run_pipeline(struct node **sn, int ns, int bg) {
    int pipes[2 * MAXSTAGE] = { 0 }, bad = 0, built = 0;
    struct stage st[MAXSTAGE] = { { 0 } };
    struct fdl l = { { 0 }, 0 };
    cs_used = 0;
    for (int i = 0; i < ns - 1; i++) {
        int p[2];
        if (pipe(p) < 0) { fprintf(stderr, "sh: pipe: %s\n", strerror(errno)); bad = 1; break; }
        pipes[2 * i] = track(&l, p[0]); pipes[2 * i + 1] = track(&l, p[1]);
    }
    for (int i = 0; i < ns && !bad; i++) {
        built = i + 1;
        if (build_stage(&st[i], sn[i], i, ns, pipes, &l) < 0) bad = 1;
    }
    if (bad) {
        for (int i = 0; i < built; i++) free_stage(&st[i]);
        close_all(&l);
        return 1;
    }
    if (ns == 1 && sn[0]->type == N_CMD && !st[0].argc) {            /* only assignments (and maybe redirections) */
        for (int i = 0; i < st[0].asg.n; i++) {
            char *eq = strchr(st[0].asg.v[i], '=');
            *eq = 0;
            var_set(st[0].asg.v[i], eq + 1);
        }
        free_stage(&st[0]);
        close_all(&l);
        int rc = cs_used ? cs_status : 0;
        leaf_done(rc);
        return rc;
    }
    if (ns == 1 && !bg) xtrace(st[0].argv, st[0].argc);
    else if (g_xtrace) for (int i = 0; i < ns; i++) xtrace(st[i].argv, st[i].argc);
    if (bg && st[0].r[0] < 0) st[0].r[0] = track(&l, open("/dev/null", O_RDONLY));
    int first = -1;
    for (int i = 0; i < ns; i++) {
        struct stage *s = &st[i];
        s->st = -1;
        if (sn[i]->type == N_CMD) {
            if (!s->argc) { s->st = 0; continue; }
            if (fn_exists(s->argv[0])) s->kind = ns == 1 && !bg ? K_INPROC : K_CHILD;
            else if (is_builtin(s->argv[0])) s->kind = bg ? K_CHILD : K_INPROC;
            else s->kind = K_EXT;
        } else s->kind = K_CHILD;                                  /* N_SUB, compound commands in a pipeline */
        if (s->kind == K_EXT) {
            int e = spawn_ext(s);
            if (e) s->st = e;
        } else if (s->kind == K_CHILD) {
            int fds[3] = { s->r[0], s->r[1], s->r[2] }, pid;
            if (spawn_sh(stage_text(sn[i]), fds, &pid) < 0) s->st = 127;
            else s->pid = pid;
        }
        if (s->pid > 0 && first < 0) first = s->pid;
    }
    /* children own their copies; close everything except what in-process stages still need */
    for (int k = l.n - 1; k >= 0; k--) {
        int used = 0;
        for (int i = 0; i < ns; i++)
            if (st[i].kind == K_INPROC && (st[i].r[0] == l.fd[k] || st[i].r[1] == l.fd[k] || st[i].r[2] == l.fd[k])) used = 1;
        if (!used) untrack(&l, l.fd[k]);
    }
    if (!bg && first > 0) set_fg(first);
    for (int i = 0; i < ns; i++)
        if (st[i].kind == K_INPROC) {
            int save[3];
            apply_fds(st[i].r, save);
            st[i].st = run_inproc(&st[i]);
            restore_fds(save);
            untrack(&l, st[i].r[1]); untrack(&l, st[i].r[2]); untrack(&l, st[i].r[0]);
        }
    close_all(&l);
    int last = 0;
    if (bg) {
        for (int i = ns - 1; i >= 0; i--)
            if (st[i].pid > 0) { job_add(st[i].pid, stage_text(sn[0])); break; }
    } else {
        int killed = 0;
        for (int i = 0; i < ns; i++) {
            if (st[i].pid > 0) {
                st[i].st = wait_pid(st[i].pid);
                if (st[i].st == 128 + SIGINT && !killed) {   /* ^C only reaches the first stage: take the rest down too */
                    killed = 1;
                    for (int j = i + 1; j < ns; j++) if (st[j].pid > 0) kill(st[j].pid, SIGKILL);
                }
            }
        }
        set_fg(getpid());
        last = killed ? 128 + SIGINT : st[ns - 1].st;
        if (last == 128 + SIGINT && !g_interactive) {                   /* ^C aborts scripts, unless trapped */
            if (trap_set(SIGINT)) trap_raise(SIGINT);
            else { fflush(stdout); exit(last); }
        }
    }
    for (int i = 0; i < ns; i++) free_stage(&st[i]);
    leaf_done(last);
    return last;
}

/* ---- the evaluator ---- */
static int loop_ctl(void) {                    /* g_ctl is set after a loop body: 1 = leave the loop */
    if (g_ctl == CTL_BREAK) { if (--g_ctln <= 0) g_ctl = CTL_NONE; return 1; }
    if (g_ctl == CTL_CONT) { if (--g_ctln <= 0) { g_ctl = CTL_NONE; return 0; } return 1; }
    return 1;
}

static int run_body(struct node *n) {
    int last = 0;
    switch (n->type) {
    case N_LIST:
        for (struct node *c = n->a; c; c = c->next) { run_node(c); if (g_ctl) break; }
        break;
    case N_GROUP: run_node(n->a); break;
    case N_IF:
        g_noerr++; run_node(n->a); g_noerr--;
        if (g_ctl) break;
        if (g_status == 0) run_node(n->b);
        else if (n->c) run_node(n->c);
        else g_status = 0;
        break;
    case N_WHILE: case N_UNTIL:
        for (;;) {
            g_noerr++; run_node(n->a); g_noerr--;
            if (g_ctl || (g_status == 0) == (n->type == N_UNTIL)) break;
            run_node(n->b);
            last = g_status;
            if (g_ctl && loop_ctl()) break;
        }
        if (!g_ctl) g_status = last;
        break;
    case N_FOR: {
        struct vec it = { 0 };
        if (n->hasin) for (char **w = n->w; *w; w++) expand_word(*w, &it, M_ARGV);
        else for (int i = 1; i < g_sargc; i++) vec_add(&it, strdup(g_sargv[i]));
        for (int i = 0; i < it.n; i++) {
            var_set(n->s, it.v[i]);
            run_node(n->a);
            last = g_status;
            if (g_ctl && loop_ctl()) break;
        }
        for (int i = 0; i < it.n; i++) free(it.v[i]);
        free(it.v);
        if (!g_ctl) g_status = last;
        break;
    }
    case N_CASE: {
        char *word = expand_one(n->s);
        int hit = 0;
        for (struct citem *c = n->items; c && !hit; c = c->next)
            for (char **p = c->pats; *p && !hit; p++) {
                char *pat = expand_pat(*p);
                if (gmatch(pat, word)) { hit = 1; run_node(c->body); }
                free(pat);
            }
        free(word);
        if (!hit) g_status = 0;
        break;
    }
    }
    return g_status;
}

static int run_redirected(struct node *n) {
    int r[3] = { -1, -1, -1 }, save[3];
    struct fdl l = { { 0 }, 0 };
    if (open_redirs(n->rd, r, &l) < 0) { close_all(&l); return g_status = 1; }
    apply_fds(r, save);
    run_body(n);
    restore_fds(save);
    close_all(&l);
    return g_status;
}

int run_node(struct node *n) {
    struct node *sn[MAXSTAGE];
    int ns = 0;
    switch (n->type) {
    case N_CMD: case N_SUB: sn[0] = n; g_status = run_pipeline(sn, 1, 0); break;
    case N_PIPE:
        for (struct node *p = n->a; p; p = p->next) {
            if (ns == MAXSTAGE) { fprintf(stderr, "sh: pipeline too long\n"); return g_status = 2; }
            sn[ns++] = p;
        }
        g_status = run_pipeline(sn, ns, 0);
        break;
    case N_AND: case N_OR:
        g_noerr++; run_node(n->a); g_noerr--;
        if (!g_ctl && (n->type == N_AND) == (g_status == 0)) run_node(n->b);
        break;
    case N_NOT: g_noerr++; run_node(n->a); g_noerr--; if (!g_ctl) g_status = !g_status; break;
    case N_BG:
        n = n->a;
        if (n->type == N_PIPE) {
            for (struct node *p = n->a; p && ns < MAXSTAGE; p = p->next) sn[ns++] = p;
            run_pipeline(sn, ns, 1);
        } else if (n->type == N_CMD || n->type == N_SUB) { sn[0] = n; run_pipeline(sn, 1, 1); }
        else {                                            /* && lists, compound commands: child shell */
            int fds[3] = { -1, -1, -1 }, pid, dn = open("/dev/null", O_RDONLY);
            fds[0] = dn;
            if (spawn_sh(n->src, fds, &pid) == 0) job_add(pid, n->src);
            if (dn >= 0) close(dn);
        }
        g_status = 0;
        break;
    case N_FUNC: g_status = fn_define(n->s, n->src); break;
    default: g_status = n->rd ? run_redirected(n) : run_body(n);
    }
    if (g_trap_pending) run_traps();
    return g_status;
}

/* ---- reading commands from somewhere ---- */
int run_text(const char *text) {
    struct prog p;
    int rc = parse_text(text, &p);
    if (rc != P_OK) {
        fprintf(stderr, "sh: %s\n", rc == P_INCOMPLETE ? "syntax error: unexpected end of file" : g_perr);
        return g_status = 2;
    }
    run_node(p.root);
    prog_free(&p);
    return g_status;
}

int run_lines(const char *text) {
    run_text(text);
    g_ctl = CTL_NONE;
    return g_status;
}

/* history entry for a (possibly multi-line) command: newlines become "; " unless the line already continues */
static char *flatten(const char *buf) {
    struct sb o = { 0 };
    for (const char *p = buf; *p; p++) {
        if (*p != '\n') { sb_add(&o, *p); continue; }
        size_t n = o.n;
        while (n && o.p[n - 1] == ' ') n--;
        int cont = !n || strchr(";&|{(\\", o.p[n - 1]);
        static const char *const kw[] = { "then", "do", "else", "elif", "in" };
        for (unsigned i = 0; !cont && i < sizeof kw / sizeof *kw; i++) {
            size_t k = strlen(kw[i]);
            if (n >= k && !strncmp(o.p + n - k, kw[i], k) && (n == k || strchr(" ;", o.p[n - k - 1]))) cont = 1;
        }
        sb_add(&o, cont ? ' ' : ';');
        if (!cont) sb_add(&o, ' ');
    }
    return sb_take(&o);
}

int run_reader(char *(*next)(void *, int), void *ctx) {
    char *buf = NULL;
    for (;;) {
        char *ln = next(ctx, buf != NULL);
        if (g_intr) {                                    /* ^C: drop whatever was half typed */
            g_intr = 0; g_status = 130;
            free(ln); free(buf); buf = NULL;
            continue;
        }
        if (!ln) {
            if (buf) { fprintf(stderr, "sh: syntax error: unexpected end of file\n"); g_status = 2; free(buf); }
            return g_status;
        }
        if (!buf) buf = ln;
        else {
            size_t a = strlen(buf), b = strlen(ln);
            buf = realloc(buf, a + b + 2);
            buf[a++] = '\n';
            memcpy(buf + a, ln, b + 1);
            free(ln);
        }
        struct prog p;
        int rc = parse_text(buf, &p);
        if (rc == P_INCOMPLETE) continue;
        if (g_interactive && buf[strspn(buf, " \t\n")] && !strstr(buf, "<<")) {
            char *h = flatten(buf);
            hist_add(h);
            free(h);
        }
        free(buf);
        buf = NULL;
        if (rc == P_ERROR) {
            fprintf(stderr, "sh: %s\n", g_perr);
            g_status = 2;
            if (!g_interactive) return g_status;
            continue;
        }
        run_node(p.root);
        prog_free(&p);
        int ret = g_ctl == CTL_RET;
        g_ctl = CTL_NONE;
        if (ret && !g_interactive) return g_status;      /* `return` ends a sourced script */
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
    if (argc > 0) { g_sargv = NULL; g_sargc = 0; set_pos(argc, argv); }
    g_interactive = 0;
    struct frd r = { f, NULL, 0 };
    run_reader(file_next, &r);
    if (argc > 0) { set_pos(0, NULL); g_sargv = ov; g_sargc = oc; }
    g_interactive = oi;
    free(r.buf);
    fclose(f);
    return g_status;
}
