/* make [-nskiBr] [-f FILE] [-C DIR] [-j N] [VAR=val...] [target...]   a POSIX-ish make, written from scratch.
 * Rules, pattern/suffix rules, variables (= := ?= += !=), functions, include, conditionals, define, order-only
 * prerequisites. Recipes run one line at a time through /bin/sh. Not supported: target-specific variables,
 * VPATH, double-colon semantics (treated as ':'), parallel jobs (-j is accepted and ignored), $(eval).
 * Builds on the host too with -DMAKE_HOST (tools/maketest). */
#ifdef MAKE_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
extern char **environ;
#define DEFAULT_CC "cc"
#else
#include <vutil.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#define DEFAULT_CC "tcc"
#endif

/* ---------------------------------------------------------------- strings and vectors */
typedef struct { char *s; size_t n, cap; } SB;
typedef struct { char **v; int n; } SV;

static void oom(void) { fputs("make: out of memory\n", stderr); exit(2); }
static void sba(SB *b, const char *p, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; if (!(b->s = realloc(b->s, b->cap))) oom(); }
    memcpy(b->s + b->n, p, n);
    b->s[b->n += n] = 0;
}
static void sbs(SB *b, const char *p) { sba(b, p, strlen(p)); }
static void sbc(SB *b, char c) { sba(b, &c, 1); }
static char *sbz(SB *b) { if (!b->s) sba(b, "", 0); return b->s; }
static void sbw(SB *b, const char *p, size_t n) { if (b->n) sbc(b, ' '); sba(b, p, n); }   /* a word, space separated */
static char *mdup(const char *s, size_t n) { char *p = malloc(n + 1); if (!p) oom(); memcpy(p, s, n); p[n] = 0; return p; }
static char *sdup(const char *s) { return mdup(s, strlen(s)); }
static char *cat(const char *a, const char *b) { SB x = {0}; sbs(&x, a); sbs(&x, b); return sbz(&x); }
static void svadd(SV *a, char *s) { if (!(a->v = realloc(a->v, (a->n + 1) * sizeof *a->v))) oom(); a->v[a->n++] = s; }
static int cmps(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
/* word iterator: WORDS(w, l, str) { ... } with `const char *w; size_t l;` declared by the caller */
static const char *nw(const char **pp, size_t *len) {
    const char *p = *pp;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return NULL;
    const char *s = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    *pp = p; *len = p - s;
    return s;
}
#define WORDS(w, l, str) for (const char *_p = (str); (w = nw(&_p, &l)); )
static SV words(const char *s) { SV r = {0}; const char *w; size_t l; WORDS(w, l, s) svadd(&r, mdup(w, l)); return r; }

/* ---------------------------------------------------------------- messages */
static const char *prog = "make", *cur_fn;
static int level, cur_ln, silent, dry, keep, ign_all, always, no_builtin, print_dir, did_dir;

static void pre(FILE *f) { fflush(stdout); if (level) fprintf(f, "%s[%d]: ", prog, level); else fprintf(f, "%s: ", prog); }
static void warn(const char *fmt, ...) {
    va_list ap;
    pre(stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
static void die(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    if (cur_fn && cur_ln) fprintf(stderr, "%s:%d: *** ", cur_fn, cur_ln);
    else { pre(stderr); fputs("*** ", stderr); }
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputs(".  Stop.\n", stderr);
    exit(2);
}
static void leaving(void) {
    char d[PATH_MAX];
    if (did_dir && getcwd(d, sizeof d)) { pre(stdout); printf("Leaving directory '%s'\n", d); fflush(stdout); }
}

/* ---------------------------------------------------------------- variables */
typedef struct Var { char *name, *val; char simple, org, exp, busy; struct Var *next; } Var;
enum { O_DEF = 1, O_ENV = 2, O_FILE = 4, O_ENVE = 5, O_CMD = 6, O_OVER = 8 };   /* O_ENVE: environment under -e */
static int env_org = O_ENV;
static Var *vt[512];
static unsigned vh(const char *s, size_t n) { unsigned h = 5381; while (n--) h = h * 33 + (unsigned char)*s++; return h & 511; }
static Var *vfind(const char *n, size_t len) {
    for (Var *v = vt[vh(n, len)]; v; v = v->next) if (!strncmp(v->name, n, len) && !v->name[len]) return v->val ? v : NULL;
    return NULL;
}
static Var *vraw(const char *n, size_t len) {            /* also finds undefined (val NULL) entries; creates */
    for (Var *v = vt[vh(n, len)]; v; v = v->next) if (!strncmp(v->name, n, len) && !v->name[len]) return v;
    Var *v = calloc(1, sizeof *v);
    if (!v) oom();
    v->name = mdup(n, len);
    unsigned h = vh(n, len);
    v->next = vt[h]; vt[h] = v;
    return v;
}
static Var *vset(const char *name, const char *val, int simple, int org) {
    Var *v = vraw(name, strlen(name));
    if (v->val && org < v->org) return v;
    if (v->val && (v->org == O_ENV || v->org == O_ENVE) && org >= O_FILE) v->exp = 1;    /* a modified environment variable stays exported */
    v->val = sdup(val); v->simple = simple; v->org = org;
    return v;
}

/* ---------------------------------------------------------------- expansion */
static char *expand(const char *s);
static void ex(SB *b, const char *s, size_t n);
static struct { const char *at, *lt, *ca, *pl, *q, *st, *oo; } au;

static const char *matchparen(const char *s, const char *e, char open) {   /* s is just after the opener */
    char close = open == '(' ? ')' : '}';
    for (int d = 1; s < e; s++) { if (*s == open) d++; else if (*s == close && !--d) return s; }
    return NULL;
}
static const char *skipref(const char *s) {              /* s at '$': the character after the reference */
    if (s[1] == '(' || s[1] == '{') { const char *c = matchparen(s + 2, s + strlen(s), s[1]); return c ? c + 1 : s + strlen(s); }
    return s[1] ? s + 2 : s + 1;
}
static const char *scan(const char *s, int ch) {         /* first ch outside variable references */
    while (*s) {
        if (*s == '$' && s[1]) { s = skipref(s); continue; }
        if (*s == ch) return s;
        s++;
    }
    return NULL;
}

static int pmatch(const char *pat, const char *s, char **stem) {
    const char *pc = strchr(pat, '%');
    if (stem) *stem = NULL;
    if (!pc) return !strcmp(pat, s);
    size_t pl = pc - pat, sl = strlen(pc + 1), n = strlen(s);
    if (n < pl + sl || strncmp(s, pat, pl) || strcmp(s + n - sl, pc + 1)) return 0;
    if (stem) *stem = mdup(s + pl, n - pl - sl);
    return 1;
}
static void psub(SB *r, const char *pat, const char *rep, const char *text) {
    const char *w; size_t l;
    WORDS(w, l, text) {
        char *x = mdup(w, l), *st; const char *pc = strchr(rep, '%');
        if (r->n) sbc(r, ' ');
        if (pmatch(pat, x, &st)) { if (pc && st) { sba(r, rep, pc - rep); sbs(r, st); sbs(r, pc + 1); } else sbs(r, rep); }
        else sbs(r, x);
    }
}
static void mapdf(SB *out, const char *v, int mode) {    /* 0 notdir/$(@F), 1 $(@D), 2 $(dir) */
    const char *w; size_t l;
    SB x = {0}, *b = &x;
    WORDS(w, l, v) {
        const char *sl = NULL;
        for (const char *p = w; p < w + l; p++) if (*p == '/') sl = p;
        if (b->n) sbc(b, ' ');
        if (mode) { if (sl) sba(b, w, mode == 2 ? sl + 1 - w : sl > w ? sl - w : 1); else sbs(b, mode == 2 ? "./" : "."); }
        else if (sl) sba(b, sl + 1, w + l - sl - 1); else sba(b, w, l);
    }
    sbs(out, sbz(b));
}
static void lookup(SB *b, const char *n, size_t len) {
    if (len && len <= 2 && strchr("@<^+?*|%", n[0]) && (len == 1 || n[1] == 'D' || n[1] == 'F')) {
        const char *v = n[0] == '@' ? au.at : n[0] == '<' ? au.lt : n[0] == '^' ? au.ca : n[0] == '+' ? au.pl :
                        n[0] == '?' ? au.q : n[0] == '*' ? au.st : n[0] == '|' ? au.oo : NULL;
        if (!v) return;
        if (len == 1) sbs(b, v); else mapdf(b, v, n[1] == 'D' ? 1 : 0);
        return;
    }
    Var *v = vfind(n, len);
    if (!v) return;
    if (v->simple) { sbs(b, v->val); return; }
    if (v->busy) die("Recursive variable '%s' references itself (eventually)", v->name);
    v->busy = 1;
    ex(b, v->val, strlen(v->val));
    v->busy = 0;
}

/* functions */
enum { F_SUBST, F_PATSUBST, F_STRIP, F_FINDSTRING, F_FILTER, F_FILTEROUT, F_SORT, F_WORD, F_WORDLIST, F_WORDS, F_FIRSTWORD,
       F_LASTWORD, F_DIR, F_NOTDIR, F_SUFFIX, F_BASENAME, F_ADDSUFFIX, F_ADDPREFIX, F_JOIN, F_WILDCARD, F_REALPATH,
       F_ABSPATH, F_SHELL, F_ERROR, F_WARNING, F_INFO, F_ORIGIN, F_IF, F_OR, F_AND, F_FOREACH, F_CALL, F_N };
static const char *const fname[F_N] = { "subst", "patsubst", "strip", "findstring", "filter", "filter-out", "sort", "word",
    "wordlist", "words", "firstword", "lastword", "dir", "notdir", "suffix", "basename", "addsuffix", "addprefix", "join",
    "wildcard", "realpath", "abspath", "shell", "error", "warning", "info", "origin", "if", "or", "and", "foreach", "call" };
static const char fargs[F_N] = { 3, 3, 1, 2, 2, 2, 1, 2, 3, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 3, 20, 20, 3, 20 };

static int gmatch(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (;; s++) { if (gmatch(p, s)) return 1; if (!*s) return 0; }
        }
        if (!*s) return 0;
        if (*p == '?') { p++; s++; }
        else if (*p == '[') {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^', ok = 0;
            if (neg) q++;
            for (int first = 1; *q && (*q != ']' || first); first = 0, q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2]) ok = 1; q += 2; }
                else if (*q == *s) ok = 1;
            }
            if (!*q) { if (*s != '[') return 0; p++; s++; continue; }
            if (ok == neg) return 0;
            p = q + 1; s++;
        } else { if (*p == '\\' && p[1]) p++; if (*p != *s) return 0; p++; s++; }
    }
    return !*s;
}
static int isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void gl(const char *base, const char *pat, SV *out) {
    const char *sl = strchr(pat, '/'), *rest = sl ? sl + 1 : NULL;
    size_t cl = sl ? (size_t)(sl - pat) : strlen(pat);
    char *c = mdup(pat, cl);
    if (!cl && sl) { gl(cat(base, "/"), rest, out); return; }
    if (!strpbrk(c, "*?[")) {
        char *p = cat(base, c);
        struct stat st;
        if (rest && *rest) { if (isdir(p)) gl(cat(p, "/"), rest, out); }
        else if (stat(p, &st) == 0 && (!rest || S_ISDIR(st.st_mode))) svadd(out, rest ? cat(p, "/") : p);
        return;
    }
    DIR *d = opendir(*base ? base : ".");
    if (!d) return;
    SV ent = {0};
    for (struct dirent *e; (e = readdir(d)); )
        if ((e->d_name[0] != '.' || c[0] == '.') && gmatch(c, e->d_name)) svadd(&ent, sdup(e->d_name));
    closedir(d);
    qsort(ent.v, ent.n, sizeof *ent.v, cmps);
    for (int i = 0; i < ent.n; i++) {
        char *p = cat(base, ent.v[i]);
        if (rest && *rest) { if (isdir(p)) gl(cat(p, "/"), rest, out); }
        else if (!rest) svadd(out, p);
        else if (isdir(p)) svadd(out, cat(p, "/"));
    }
}
static SV glob(const char *pat) { SV r = {0}; gl("", pat, &r); return r; }

static char *abspath(const char *w) {
    SB b = {0};
    if (*w != '/') { char d[PATH_MAX]; if (getcwd(d, sizeof d)) sbs(&b, d); }
    sbc(&b, '/'); sbs(&b, w);
    SB o = {0};
    for (char *p = b.s; *p; ) {
        while (*p == '/') p++;
        char *c = p;
        while (*p && *p != '/') p++;
        size_t n = p - c;
        if (!n || (n == 1 && *c == '.')) continue;
        if (n == 2 && c[0] == '.' && c[1] == '.') { while (o.n && o.s[o.n - 1] != '/') o.s[--o.n] = 0; if (o.n) o.s[--o.n] = 0; continue; }
        sbc(&o, '/'); sba(&o, c, n);
    }
    if (!o.n) sbc(&o, '/');
    return sbz(&o);
}

static char *shell_out(const char *cmd) {
    SB b = {0};
    fflush(NULL);
#ifdef MAKE_HOST
    FILE *f = popen(cmd, "r");
    if (f) { char buf[512]; size_t n; while ((n = fread(buf, 1, sizeof buf, f))) sba(&b, buf, n); pclose(f); }
#else
    int p[2];
    char *av[] = { "sh", "-c", (char *)cmd, NULL };
    struct spawn_opts o = { { -1, -1, -1 }, NULL, 0 };
    if (pipe(p) == 0) {
        o.fd[1] = p[1];
        int pid = spawnve("/bin/sh", av, environ, &o);
        close(p[1]);
        if (pid >= 0) { char buf[512]; ssize_t n; while ((n = read(p[0], buf, sizeof buf)) > 0) sba(&b, buf, n); waitpid(pid, NULL, 0); }
        close(p[0]);
    }
#endif
    char *s = sbz(&b);
    while (b.n && s[b.n - 1] == '\n') s[--b.n] = 0;
    for (char *q = s; *q; q++) if (*q == '\n') *q = ' ';
    return s;
}

static int splitraw(const char *s, int max, const char **st, size_t *ln) {   /* top-level commas; the last part keeps the rest */
    int n = 0, d = 0;
    const char *a = s;
    for (const char *p = s;; p++) {
        if (!*p || (*p == ',' && !d && n < max - 1)) { st[n] = a; ln[n++] = p - a; if (!*p) break; a = p + 1; }
        else if (*p == '(' || *p == '{') d++;
        else if (*p == ')' || *p == '}') d--;
    }
    return n;
}
static int sargs(const char *s, int max, char **out) {
    const char *st[32]; size_t ln[32];
    int n = splitraw(s, max, st, ln);
    for (int i = 0; i < n; i++) { SB t = {0}; ex(&t, st[i], ln[i]); out[i] = sbz(&t); }
    for (int i = n; i < 3; i++) out[i] = "";
    return n;
}
static const char *origin_of(const char *n) {
    if (strlen(n) <= 2 && strchr("@<^+?*|%", n[0]) && n[0]) return "automatic";
    Var *v = vfind(n, strlen(n));
    return !v ? "undefined" : v->org == O_DEF ? "default" : v->org == O_ENV || v->org == O_ENVE ? "environment" : v->org == O_FILE ? "file" : v->org == O_CMD ? "command line" : "override";
}

static void func(SB *b, int id, const char *raw) {
    SB r = {0};
    const char *w; size_t l;
    if (id == F_IF || id == F_OR || id == F_AND || id == F_FOREACH || id == F_CALL) {
        const char *st[32]; size_t ln[32];
        int n = splitraw(raw, fargs[id], st, ln);
        SB c = {0};
        ex(&c, st[0], ln[0]);
        if (id == F_IF) {
            char *t = trim(sbz(&c));
            if (*t) ex(&r, st[1], n > 1 ? ln[1] : 0);
            else if (n > 2) ex(&r, st[2], ln[2]);
        } else if (id == F_OR || id == F_AND) {
            for (int i = 0; i < n; i++) {
                SB v = {0};
                ex(&v, st[i], ln[i]);
                char *t = trim(sbz(&v));
                if ((*t != 0) == (id == F_OR)) { sbs(&r, t); break; }
                if (id == F_AND && i == n - 1) sbs(&r, t);
            }
        } else if (id == F_FOREACH) {
            char *vn = trim(sbz(&c));
            Var *v = vraw(vn, strlen(vn));
            char *sv = v->val, so = v->org, ss = v->simple;
            SB li = {0};
            if (n > 1) ex(&li, st[1], ln[1]);
            WORDS(w, l, sbz(&li)) {
                v->val = mdup(w, l); v->org = O_OVER; v->simple = 1;
                if (r.n) sbc(&r, ' ');
                if (n > 2) ex(&r, st[2], ln[2]);
            }
            v->val = sv; v->org = so; v->simple = ss;
        } else {                                         /* call */
            char *name = trim(sbz(&c));
            Var *f = vfind(name, strlen(name)), *pv[20];
            char *sv[20], so[20], ss[20];
            for (int i = 1; i < n; i++) {
                SB a = {0};
                char nm[8];
                ex(&a, st[i], ln[i]);
                snprintf(nm, sizeof nm, "%d", i);
                pv[i] = vraw(nm, strlen(nm));
                sv[i] = pv[i]->val; so[i] = pv[i]->org; ss[i] = pv[i]->simple;
                pv[i]->val = sbz(&a); pv[i]->org = O_OVER; pv[i]->simple = 1;
            }
            if (f) lookup(&r, name, strlen(name));
            for (int i = 1; i < n; i++) { pv[i]->val = sv[i]; pv[i]->org = so[i]; pv[i]->simple = ss[i]; }
        }
        sbs(b, sbz(&r));
        return;
    }
    char *a[32];
    int na = sargs(raw, fargs[id], a);
    (void)na;
    switch (id) {
    case F_SUBST:
        if (!*a[0]) sbs(&r, a[2]);
        else for (const char *p = a[2], *q; ; p = q + strlen(a[0])) { q = strstr(p, a[0]); if (!q) { sbs(&r, p); break; } sba(&r, p, q - p); sbs(&r, a[1]); }
        break;
    case F_PATSUBST: psub(&r, a[0], a[1], a[2]); break;
    case F_STRIP: WORDS(w, l, a[0]) sbw(&r, w, l); break;
    case F_FINDSTRING: if (strstr(a[1], a[0])) sbs(&r, a[0]); break;
    case F_FILTER: case F_FILTEROUT:
        WORDS(w, l, a[1]) {
            char *x = mdup(w, l);
            int hit = 0;
            const char *pw; size_t pl;
            WORDS(pw, pl, a[0]) { char *p = mdup(pw, pl); if (pmatch(p, x, NULL)) { hit = 1; break; } }
            if (hit == (id == F_FILTER)) sbw(&r, w, l);
        }
        break;
    case F_SORT: {
        SV v = words(a[0]);
        qsort(v.v, v.n, sizeof *v.v, cmps);
        for (int i = 0; i < v.n; i++) if (!i || strcmp(v.v[i], v.v[i - 1])) { if (r.n) sbc(&r, ' '); sbs(&r, v.v[i]); }
        break;
    }
    case F_WORD: case F_WORDLIST: {
        int s = atoi(trim(a[0])), e = id == F_WORD ? s : atoi(trim(a[1])), i = 0;
        if (s < 1) die("first argument to '%s' function must be greater than 0", fname[id]);
        WORDS(w, l, a[id == F_WORD ? 1 : 2]) if (++i >= s && i <= e) sbw(&r, w, l);
        break;
    }
    case F_WORDS: { int i = 0; WORDS(w, l, a[0]) i++; char t[16]; snprintf(t, sizeof t, "%d", i); sbs(&r, t); break; }
    case F_FIRSTWORD: { WORDS(w, l, a[0]) { sba(&r, w, l); break; } break; }
    case F_LASTWORD: { const char *lw = NULL; size_t ll = 0; WORDS(w, l, a[0]) { lw = w; ll = l; } if (lw) sba(&r, lw, ll); break; }
    case F_DIR: mapdf(&r, a[0], 2); break;
    case F_NOTDIR: mapdf(&r, a[0], 0); break;
    case F_SUFFIX: case F_BASENAME:
        WORDS(w, l, a[0]) {
            const char *dot = NULL;
            for (const char *p = w; p < w + l; p++) { if (*p == '/') dot = NULL; else if (*p == '.') dot = p; }
            if (id == F_SUFFIX) { if (dot) sbw(&r, dot, w + l - dot); }
            else sbw(&r, w, dot ? (size_t)(dot - w) : l);
        }
        break;
    case F_ADDSUFFIX: WORDS(w, l, a[1]) { if (r.n) sbc(&r, ' '); sba(&r, w, l); sbs(&r, a[0]); } break;
    case F_ADDPREFIX: WORDS(w, l, a[1]) { if (r.n) sbc(&r, ' '); sbs(&r, a[0]); sba(&r, w, l); } break;
    case F_JOIN: {
        SV x = words(a[0]), y = words(a[1]);
        for (int i = 0; i < x.n || i < y.n; i++) { if (i) sbc(&r, ' '); if (i < x.n) sbs(&r, x.v[i]); if (i < y.n) sbs(&r, y.v[i]); }
        break;
    }
    case F_WILDCARD:
        WORDS(w, l, a[0]) { SV g = glob(mdup(w, l)); for (int i = 0; i < g.n; i++) { if (r.n) sbc(&r, ' '); sbs(&r, g.v[i]); } }
        break;
    case F_REALPATH: case F_ABSPATH:
        WORDS(w, l, a[0]) { char *p = abspath(mdup(w, l)); struct stat st; if (id == F_ABSPATH || stat(p, &st) == 0) { if (r.n) sbc(&r, ' '); sbs(&r, p); } }
        break;
    case F_SHELL: sbs(&r, shell_out(a[0])); break;
    case F_ERROR: die("%s", a[0]); break;
    case F_WARNING:
        fflush(stdout);
        if (cur_fn) fprintf(stderr, "%s:%d: %s\n", cur_fn, cur_ln, a[0]); else warn("%s", a[0]);
        break;
    case F_INFO: puts(a[0]); fflush(stdout); break;
    case F_ORIGIN: sbs(&r, origin_of(trim(a[0]))); break;
    }
    sbs(b, sbz(&r));
}

static void ref(SB *b, const char *s, size_t n) {
    size_t k = 0;
    while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '-' || s[k] == '_')) k++;
    if (k && k < n && (s[k] == ' ' || s[k] == '\t'))
        for (int id = 0; id < F_N; id++)
            if (strlen(fname[id]) == k && !strncmp(fname[id], s, k)) {
                const char *a = s + k;
                while (a < s + n && isspace((unsigned char)*a)) a++;
                func(b, id, mdup(a, s + n - a));
                return;
            }
    SB nb = {0};
    ex(&nb, s, n);
    char *nm = sbz(&nb), *col = strchr(nm, ':'), *eq = col ? strchr(col, '=') : NULL;
    if (eq && !vfind(nm, strlen(nm))) {                  /* $(VAR:a=b) */
        *col = 0; *eq = 0;
        SB v = {0}, r = {0};
        lookup(&v, nm, strlen(nm));
        const char *from = col + 1, *to = eq + 1;
        if (strchr(from, '%')) psub(&r, from, to, sbz(&v));
        else psub(&r, cat("%", from), cat("%", to), sbz(&v));
        sbs(b, sbz(&r));
        return;
    }
    lookup(b, nm, strlen(nm));
}
static void ex(SB *b, const char *s, size_t n) {
    const char *e = s + n;
    while (s < e) {
        if (*s != '$') { const char *p = memchr(s, '$', e - s); if (!p) p = e; sba(b, s, p - s); s = p; continue; }
        if (++s >= e) break;
        if (*s == '(' || *s == '{') {
            const char *c = matchparen(s + 1, e, *s);
            if (!c) die("unterminated variable reference");
            ref(b, s + 1, c - s - 1);
            s = c + 1;
        } else { if (*s == '$') sbc(b, '$'); else lookup(b, s, 1); s++; }
    }
}
static char *expand(const char *s) { SB b = {0}; ex(&b, s, strlen(s)); return sbz(&b); }

/* ---------------------------------------------------------------- data model */
typedef struct { char *t; int ln; } Line;
typedef struct { Line *l; int n; const char *fn; } Rec;
typedef struct File File;
typedef struct { File **v; int n; } FV;
struct File { char *name; FV deps, oo; Rec *rec; File *next; char phony, silent, tgt, state, upd, ran, hascmd, err; long long mt; };
typedef struct PRule { char *t; SV d, oo; Rec *rec; struct PRule *next; } PRule;
#define MISSING (-1LL)

static File *ftab[512];
static PRule *pr_user, **pr_utail = &pr_user, *pr_builtin;
static SV suffixes;

static char *def_goal;
static int sil_all, delete_on_error;

static File *fget(const char *name, int create) {
    while (name[0] == '.' && name[1] == '/') { name += 2; while (*name == '/') name++; }
    size_t n = strlen(name);
    unsigned h = vh(name, n);
    for (File *f = ftab[h]; f; f = f->next) if (!strcmp(f->name, name)) return f;
    if (!create) return NULL;
    File *f = calloc(1, sizeof *f);
    if (!f) oom();
    f->name = mdup(name, n);
    f->next = ftab[h]; ftab[h] = f;
    return f;
}
static void fvadd(FV *a, File *f) { if (!(a->v = realloc(a->v, (a->n + 1) * sizeof *a->v))) oom(); a->v[a->n++] = f; }

static long long mtime(const char *p) {
    struct stat st;
    if (stat(p, &st)) return MISSING;
#ifdef MAKE_HOST
    return st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
    return st.st_mtime;
#endif
}

/* ---------------------------------------------------------------- reading makefiles */
static struct { Rec *rec; FV tg; PRule *pr[64]; int npr, in; } cur;
static struct { char active, taken, els, parent; } cs[64];
static int cn;
static int export_all;

static void add_line(const char *t, int ln) {
    if (!cur.in) die("recipe commences before first target");
    if (!cur.rec) {
        cur.rec = calloc(1, sizeof *cur.rec);
        cur.rec->fn = cur_fn;
        for (int i = 0; i < cur.tg.n; i++) cur.tg.v[i]->rec = cur.rec;
        for (int i = 0; i < cur.npr; i++) cur.pr[i]->rec = cur.rec;
    }
    Rec *r = cur.rec;
    if (!(r->l = realloc(r->l, (r->n + 1) * sizeof *r->l))) oom();
    r->l[r->n].t = sdup(t); r->l[r->n++].ln = ln;
}

static void do_assign(const char *name, int op, const char *val, int org) {
    Var *v = vfind(name, strlen(name));
    switch (op) {
    case '=': vset(name, val, 0, org); break;
    case ':': vset(name, expand(val), 1, org); break;
    case '?': if (!v) vset(name, val, 0, org); break;
    case '!': vset(name, shell_out(expand(val)), 1, org); break;
    case '+':
        if (!v) { vset(name, val, 0, org); break; }
        if (org < v->org) break;
        { SB b = {0}; sbs(&b, v->val); const char *add = v->simple ? expand(val) : val; if (b.n && *add) sbc(&b, ' '); sbs(&b, add); v->val = sbz(&b); }
        break;
    }
}
static int find_assign(char *t, char **nm, int *op, char **val) {
    for (char *s = t; *s; s++) {
        char *e;
        if (*s == '$' && s[1]) { s = (char *)skipref(s) - 1; continue; }
        if (*s == ':') {
            *op = ':'; e = s;
            if (s[1] == '=') *val = s + 2;
            else if (s[1] == ':' && s[2] == '=') *val = s + 3;
            else if (s[1] == ':' && s[2] == ':' && s[3] == '=') *val = s + 4;
            else return 0;
        } else if (*s == '=') {
            *op = '='; e = s; *val = s + 1;
            if (s > t && strchr("?+!", s[-1])) { *op = s[-1]; e = s - 1; }
        } else continue;
        *nm = trim(mdup(t, e - t));
        while (isspace((unsigned char)**val)) (*val)++;
        return 1;
    }
    return 0;
}

static void special_target(const char *name, SV *deps) {
    if (!strcmp(name, ".PHONY")) for (int i = 0; i < deps->n; i++) { File *f = fget(deps->v[i], 1); f->phony = f->tgt = 1; }
    else if (!strcmp(name, ".SUFFIXES")) { if (!deps->n) { suffixes.n = 0; pr_builtin = NULL; } for (int i = 0; i < deps->n; i++) svadd(&suffixes, deps->v[i]); }
    else if (!strcmp(name, ".SILENT")) { if (!deps->n) sil_all = 1; for (int i = 0; i < deps->n; i++) fget(deps->v[i], 1)->silent = 1; }
    else if (!strcmp(name, ".IGNORE")) { if (!deps->n) ign_all = 1; }
    else if (!strcmp(name, ".DELETE_ON_ERROR")) delete_on_error = 1;
}

static char *subst1(const char *pat, const char *stem) {   /* replace the first % */
    const char *pc = strchr(pat, '%');
    if (!pc) return sdup(pat);
    SB b = {0};
    sba(&b, pat, pc - pat); sbs(&b, stem); sbs(&b, pc + 1);
    return sbz(&b);
}

static void do_rule(const char *line, const char *fn, int ln, int tab) {
    const char *c = scan(line, ':');
    if (!c) { if (*trim(expand(line))) die(tab ? "recipe commences before first target" : "missing separator"); return; }   /* $(info ..) and friends */
    char *lhs = expand(mdup(line, c - line));
    const char *rest = c + 1;
    if (*rest == ':') rest++;
    const char *sm = scan(rest, ';');
    const char *inl = NULL;
    char *right = sm ? mdup(rest, sm - rest) : sdup(rest);
    if (sm) { inl = sm + 1; while (isspace((unsigned char)*inl)) inl++; }
    cur.rec = NULL; cur.tg.n = 0; cur.npr = 0;
    SV tg = words(lhs), deps = {0}, oo = {0};
    const char *c2 = scan(right, ':');
    if (c2) {                                            /* static pattern rule: targets: pattern: prereq-patterns */
        char *tp = trim(expand(mdup(right, c2 - right)));
        SV dp = words(expand(c2 + 1));
        for (int i = 0; i < tg.n; i++) {
            char *st;
            if (!pmatch(tp, tg.v[i], &st)) die("target '%s' doesn't match the target pattern", tg.v[i]);
            File *f = fget(tg.v[i], 1);
            f->tgt = 1;
            for (int k = 0; k < dp.n; k++) fvadd(&f->deps, fget(subst1(dp.v[k], st ? st : ""), 1));
            fvadd(&cur.tg, f);
            if (!def_goal && !i && (f->name[0] != '.' || strchr(f->name, '/'))) def_goal = f->name;
        }
    } else {
        int bar = 0;
        SV pw = words(expand(right));
        for (int i = 0; i < pw.n; i++) {
            if (!strcmp(pw.v[i], "|")) { bar = 1; continue; }
            if (strpbrk(pw.v[i], "*?[") && !strchr(pw.v[i], '%')) {
                SV g = glob(pw.v[i]);
                if (g.n) { for (int k = 0; k < g.n; k++) svadd(bar ? &oo : &deps, g.v[k]); continue; }
            }
            svadd(bar ? &oo : &deps, pw.v[i]);
        }
        for (int i = 0; i < tg.n; i++) {
            const char *t = tg.v[i];
            if (strchr(t, '%')) {
                PRule *r = calloc(1, sizeof *r);
                if (!r) oom();
                r->t = sdup(t); r->d = deps; r->oo = oo;
                *pr_utail = r; pr_utail = &r->next;
                if (cur.npr < 64) cur.pr[cur.npr++] = r;
                continue;
            }
            File *f = fget(t, 1);
            f->tgt = 1;
            for (int k = 0; k < deps.n; k++) fvadd(&f->deps, fget(deps.v[k], 1));
            for (int k = 0; k < oo.n; k++) fvadd(&f->oo, fget(oo.v[k], 1));
            fvadd(&cur.tg, f);
            if (!def_goal && !i && (t[0] != '.' || strchr(t, '/'))) def_goal = f->name;
            special_target(t, &deps);
        }
    }
    cur.in = 1;
    (void)fn;
    if (inl) add_line(inl, ln);
}

static char *read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    SB sb = {0};
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) sba(&sb, buf, r);
    close(fd);
    return sbz(&sb);
}

static char *next_line(char **pp, int *ln) {             /* one logical line: continuations joined */
    char *p = *pp;
    if (!*p) return NULL;
    SB b = {0};
    int recipe = *p == '\t';
    for (;;) {
        char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p), k = n;
        while (k && p[k - 1] == '\\') k--;
        int cont = ((n - k) & 1) && e && e[1];
        (*ln)++;
        if (!cont) { sba(&b, p, n); p += n + (e ? 1 : 0); break; }
        if (recipe) { sba(&b, p, n); sbc(&b, '\n'); p = e + 1; if (*p == '\t') p++; }
        else {
            size_t m = n - 1;
            while (m && (p[m - 1] == ' ' || p[m - 1] == '\t')) m--;
            sba(&b, p, m); sbc(&b, ' ');
            p = e + 1;
            while (*p == ' ' || *p == '\t') p++;
        }
    }
    sbz(&b);
    if (b.n && b.s[b.n - 1] == '\r') b.s[--b.n] = 0;
    *pp = p;
    return b.s;
}
static void uncomment(char *s) {
    char *d = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == '#') { *d++ = '#'; s++; }
        else if (*s == '#') break;
        else *d++ = *s;
    }
    *d = 0;
}

static int word_is(const char *t, const char *w) { size_t n = strlen(w); return !strncmp(t, w, n) && (!t[n] || isspace((unsigned char)t[n])); }
static int skipping(void) { return cn && !cs[cn - 1].active; }

static int quoted(char **p, char **out) {                 /* "a" or 'a' */
    char q = **p;
    if (q != '"' && q != '\'') return 0;
    char *e = strchr(*p + 1, q);
    if (!e) return 0;
    *out = mdup(*p + 1, e - *p - 1);
    *p = e + 1;
    return 1;
}
static int eval_cond(const char *kw, char *args) {        /* ifdef ifndef ifeq ifneq */
    args = trim(args);
    if (!strcmp(kw, "ifdef") || !strcmp(kw, "ifndef")) {
        char *nm = trim(expand(args));
        Var *v = vfind(nm, strlen(nm));
        int def = v && *v->val;
        return !strcmp(kw, "ifdef") ? def : !def;
    }
    char *x = NULL, *y = NULL;
    if (*args == '(') {
        const char *cl = matchparen(args + 1, args + strlen(args), '('), *cm;
        if (!cl) die("invalid syntax in conditional");
        char *in = mdup(args + 1, cl - args - 1);
        cm = scan(in, ',');
        if (!cm) die("invalid syntax in conditional");
        x = trim(expand(mdup(in, cm - in)));
        y = trim(expand(cm + 1));
    } else {
        char *p = args, *a = NULL, *b = NULL;
        if (!quoted(&p, &a)) die("invalid syntax in conditional");
        while (isspace((unsigned char)*p)) p++;
        if (!quoted(&p, &b)) die("invalid syntax in conditional");
        x = expand(a); y = expand(b);
    }
    return !strcmp(kw, "ifeq") ? !strcmp(x, y) : !!strcmp(x, y);
}
static int cond_kw(const char *t, char **args, char *kw) {
    static const char *const k[] = { "ifdef", "ifndef", "ifeq", "ifneq" };
    for (int i = 0; i < 4; i++) if (word_is(t, k[i])) { strcpy(kw, k[i]); *args = (char *)t + strlen(k[i]); return 1; }
    return 0;
}

static void parse_buf(char *p, const char *fn);
static void include_file(const char *name, int opt, int ln, const char *from) {
    char *b = read_file(name);
    if (!b) {
        if (opt) return;
        fflush(stdout);
        fprintf(stderr, "%s:%d: %s: %s\n", from, ln, name, strerror(errno));
        cur_fn = NULL;
        die("No rule to make target '%s'", name);
    }
    Var *ml = vfind("MAKEFILE_LIST", 13);
    SB l = {0};
    sbs(&l, ml ? ml->val : ""); if (l.n) sbc(&l, ' '); sbs(&l, name);
    vset("MAKEFILE_LIST", sbz(&l), 1, O_DEF);
    parse_buf(b, name);
}

static void parse_buf(char *p, const char *fn) {
    int ln = 1;
    const char *sfn = cur_fn; int sln = cur_ln, scn = cn;
    cur_fn = fn;
    for (char *line; (cur_ln = ln, line = next_line(&p, &ln)); ) {
        int start = cur_ln, tab = line[0] == '\t';
        if (tab && !skipping() && cur.in) { add_line(line + 1, start); continue; }
        uncomment(line);
        char *t = line;
        while (isspace((unsigned char)*t)) t++;
        if (!*t) continue;
        char kw[8], *args;
        if (cond_kw(t, &args, kw)) {
            if (cn >= 64) die("conditionals nested too deeply");
            int par = !skipping(), on = par && eval_cond(kw, args);
            cs[cn].parent = par; cs[cn].active = on; cs[cn].taken = on || !par; cs[cn++].els = 0;
            continue;
        }
        if (word_is(t, "else")) {
            if (!cn) die("extraneous 'else'");
            if (cs[cn - 1].els) die("only one 'else' per conditional");
            char *r = trim(t + 4), k2[8] = "", *a2 = NULL;
            if (*r && !cond_kw(r, &a2, k2)) die("extraneous text after 'else' directive");
            if (cs[cn - 1].taken) cs[cn - 1].active = 0;
            else { int on = *r ? eval_cond(k2, a2) : 1; cs[cn - 1].active = on; cs[cn - 1].taken = on; }
            if (!*r) cs[cn - 1].els = 1;
            continue;
        }
        if (word_is(t, "endif")) { if (!cn) die("extraneous 'endif'"); cn--; continue; }
        if (skipping()) continue;

        int org = O_FILE, exp = 0, un = 0;
        for (;;) {                                       /* prefixes */
            if (word_is(t, "override")) { org = O_OVER; t = trim(t + 8); }
            else if (word_is(t, "export")) { exp = 1; t = trim(t + 6); if (!*t) { export_all = 1; break; } }
            else if (word_is(t, "unexport")) { un = 1; t = trim(t + 8); }
            else if (word_is(t, "private")) t = trim(t + 7);
            else break;
        }
        if (!*t) continue;
        if (word_is(t, "include") || word_is(t, "-include") || word_is(t, "sinclude")) {
            int opt = t[0] != 'i';
            SV f = words(expand(t + (t[0] == '-' ? 8 : t[0] == 's' ? 8 : 7)));
            cur.in = 0;
            for (int i = 0; i < f.n; i++) {
                SV g = strpbrk(f.v[i], "*?[") ? glob(f.v[i]) : (SV){0};
                if (g.n) for (int k = 0; k < g.n; k++) include_file(g.v[k], opt, start, fn);
                else include_file(f.v[i], opt, start, fn);
            }
            cur_fn = fn;
            continue;
        }
        if (word_is(t, "vpath")) continue;
        if (word_is(t, "define")) {
            char *hd = trim(t + 6), *nm = hd, *val; int op = '=';
            if (find_assign(hd, &nm, &op, &val)) {} else nm = trim(hd);
            SB body = {0};
            int depth = 1;
            while (*p) {
                char *e = strchr(p, '\n');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                char *ph = mdup(p, n);
                p += n + (e ? 1 : 0); ln++;
                char *tt = trim(ph);
                if (word_is(tt, "define")) depth++;
                else if (word_is(tt, "endef") && !--depth) goto done;
                if (body.s) sbc(&body, '\n');
                sbs(&body, ph);
            }
            die("missing 'endef', unterminated 'define'");
        done:
            do_assign(expand(nm), op, sbz(&body), org);
            if (exp) vfind(expand(nm), strlen(expand(nm)))->exp = 1;
            cur.in = 0;
            continue;
        }
        char *nm, *val;
        int op;
        if (find_assign(t, &nm, &op, &val)) {
            nm = expand(nm);
            do_assign(nm, op, val, org);
            Var *v = vfind(nm, strlen(nm));
            if (v && exp) v->exp = 1;
            cur.in = 0;
            continue;
        }
        if (exp || un) {
            SV nv = words(expand(t));
            for (int i = 0; i < nv.n; i++) {
                Var *v = vraw(nv.v[i], strlen(nv.v[i]));
                if (!v->val) { v->val = sdup(""); v->simple = 1; v->org = O_FILE; }
                v->exp = exp;
                if (un) unsetenv(nv.v[i]);
            }
            cur.in = 0;
            continue;
        }
        do_rule(t, fn, start, tab);
    }
    if (cn != scn) die("missing 'endif'");
    cur_fn = sfn; cur_ln = sln;
}

/* ---------------------------------------------------------------- implicit rules */
static void add_builtin(const char *t, const char *d, const char *recipe) {
    PRule *r = calloc(1, sizeof *r);
    if (!r) oom();
    r->t = sdup(t);
    svadd(&r->d, sdup(d));
    r->rec = calloc(1, sizeof *r->rec);
    r->rec->fn = "<builtin>";
    r->rec->l = malloc(sizeof(Line));
    r->rec->l[0].t = sdup(recipe); r->rec->l[0].ln = 0; r->rec->n = 1;
    r->next = pr_builtin; pr_builtin = r;
}
static void finalize_suffix(void) {                       /* .c.o: and .c: become pattern rules */
    for (int h = 0; h < 512; h++)
        for (File *f = ftab[h]; f; f = f->next) {
            if (f->name[0] != '.' || !f->rec || f->deps.n || f->phony) continue;
            for (int i = 0; i < suffixes.n; i++) {
                const char *a = suffixes.v[i], *rest = NULL;
                size_t la = strlen(a);
                if (!strcmp(f->name, a)) rest = "";
                else if (!strncmp(f->name, a, la)) for (int k = 0; k < suffixes.n; k++) if (!strcmp(f->name + la, suffixes.v[k])) rest = f->name + la;
                if (!rest) continue;
                PRule *r = calloc(1, sizeof *r);
                if (!r) oom();
                r->t = cat("%", rest);
                svadd(&r->d, cat("%", a));
                r->rec = f->rec;
                *pr_utail = r; pr_utail = &r->next;
                f->rec = NULL; f->tgt = 0;
                break;
            }
        }
}

static PRule *find_impl(const char *name, char **stem, char **dir, int depth);
static int makeable(const char *n, int depth) {
    if (mtime(n) != MISSING) return 1;
    if (fget(n, 0)) return 1;                             /* mentioned in the makefile: ought to exist */
    char *s, *d;
    return depth < 3 && find_impl(n, &s, &d, depth + 1) != NULL;
}
static PRule *find_impl(const char *name, char **stem, char **dir, int depth) {
    for (int pass = 0; pass < 2; pass++) {
        PRule *best = NULL;
        size_t bl = 0;
        char *bst = NULL, *bdir = NULL;
        for (PRule *r = pass ? pr_builtin : pr_user; r; r = r->next) {
            if (!r->rec) continue;
            const char *base = name;
            size_t dl = 0;
            if (!strchr(r->t, '/')) { const char *sl = strrchr(name, '/'); if (sl) { base = sl + 1; dl = sl + 1 - name; } }
            char *st;
            if (!pmatch(r->t, base, &st)) continue;
            if (!st) st = "";
            char *dr = mdup(name, dl);
            int ok = 1;
            for (int i = 0; ok && i < r->d.n; i++) ok = makeable(cat(dr, subst1(r->d.v[i], st)), depth);
            if (!ok) continue;
            if (!best || strlen(st) < bl) { best = r; bl = strlen(st); bst = st; bdir = dr; }
        }
        if (best) { *stem = bst; *dir = bdir; return best; }
    }
    return NULL;
}

/* ---------------------------------------------------------------- running */
static int run_sh(const char *cmd) {
    fflush(NULL);
#ifdef MAKE_HOST
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
#else
    char *av[] = { "sh", "-c", (char *)cmd, NULL };
    int pid = spawnve("/bin/sh", av, environ, NULL), st = 0;
    if (pid < 0) return 127;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    return st;
#endif
}

static int rec_ran;                                       /* did the last run_rec start any command? */
static int run_rec(File *f, Rec *rec) {
    char **xs = malloc((rec->n + 1) * sizeof *xs);
    if (!xs) oom();
    rec_ran = 0;
    for (int i = 0; i < rec->n; i++) {                    /* every line is expanded before the first one runs */
        cur_fn = rec->fn; cur_ln = rec->l[i].ln;
        xs[i] = expand(rec->l[i].t);
    }
    cur_fn = NULL; cur_ln = 0;
    for (int i = 0; i < rec->n; i++) {
        char *x = xs[i];
        int is_make = strstr(rec->l[i].t, "$(MAKE)") || strstr(rec->l[i].t, "${MAKE}");
        for (char *s = x; *s; ) {
            char *e = s;
            while (*e && !(*e == '\n' && (e == s || e[-1] != '\\'))) e++;
            char *cmd = mdup(s, e - s);
            s = *e ? e + 1 : e;
            int quiet = sil_all || f->silent || silent, ign = ign_all, force = 0;
            char *c = cmd;
            for (;; c++) { if (*c == '@') quiet = 1; else if (*c == '-') ign = 1; else if (*c == '+') force = 1; else if (!isspace((unsigned char)*c)) break; }
            if (!*c) continue;
            rec_ran = 1;
            if (!quiet || dry) { puts(c); fflush(stdout); }
            if (dry && !force && !is_make) continue;
            int st = run_sh(c);
            if (!st) continue;
            char loc[PATH_MAX + 16];
            if (rec->l[i].ln) snprintf(loc, sizeof loc, "%s:%d: %s", rec->fn, rec->l[i].ln, f->name);
            else snprintf(loc, sizeof loc, "%s: %s", rec->fn, f->name);
            if (ign) { if (!silent) { pre(stderr); fprintf(stderr, "[%s] Error %d (ignored)\n", loc, st); } continue; }
            pre(stderr);
            fprintf(stderr, "*** [%s] Error %d\n", loc, st);
            if (delete_on_error && !f->phony && mtime(f->name) != MISSING) { unlink(f->name); pre(stderr); fprintf(stderr, "*** Deleting file '%s'\n", f->name); }
            return st;
        }
    }
    return 0;
}

static int inlist(SB *b, const char *name) {
    const char *w; size_t l;
    WORDS(w, l, sbz(b)) if (l == strlen(name) && !strncmp(w, name, l)) return 1;
    return 0;
}

static int update(File *f, File *par) {
    if (f->state == 2) return f->err;
    if (f->state == 1) { if (par) warn("Circular %s <- %s dependency dropped.", par->name, f->name); return 0; }
    f->state = 1;
    f->mt = mtime(f->name);
    Rec *rec = f->rec;
    char *stem = "", *dir = "";
    FV xd = {0}, xo = {0};
    if (!rec && !f->phony) {
        PRule *r = find_impl(f->name, &stem, &dir, 0);
        if (r) {
            rec = r->rec;
            for (int i = 0; i < r->d.n; i++) fvadd(&xd, fget(cat(dir, subst1(r->d.v[i], stem)), 1));
            for (int i = 0; i < r->oo.n; i++) fvadd(&xo, fget(cat(dir, subst1(r->oo.v[i], stem)), 1));
        }
    }
    if (!rec && f->mt == MISSING && !f->tgt) {
        File *d = fget(".DEFAULT", 0);
        if (d && d->rec) rec = d->rec;
        else {
            f->state = 2; f->err = 1;
            if (keep) { warn("*** No rule to make target '%s'%s%s%s.", f->name, par ? ", needed by '" : "", par ? par->name : "", par ? "'" : ""); return 1; }
            if (par) die("No rule to make target '%s', needed by '%s'", f->name, par->name);
            die("No rule to make target '%s'", f->name);
        }
    }
    f->hascmd = rec != NULL;
    int ood = f->phony || always || f->mt == MISSING, err = 0;
    SB q = {0}, ca = {0}, pl = {0}, oos = {0};
    const char *lt = NULL;
    FV *lists[3] = { xd.n ? &xd : &f->deps, xd.n ? &f->deps : &xd, &f->oo };
    FV xo_list = xo;
    for (int pass = 0; pass < 4 && (!err || keep); pass++) {
        FV *fv = pass < 3 ? lists[pass] : &xo_list;
        int isoo = pass >= 2;
        for (int i = 0; i < fv->n; i++) {
            File *d = fv->v[i];
            if (update(d, f)) { err = 1; if (!keep) break; continue; }
            if (d->ran) f->ran = 1;
            if (isoo) { if (oos.n) sbc(&oos, ' '); sbs(&oos, d->name); continue; }
            if (!lt) lt = d->name;
            int newer = d->upd || f->mt == MISSING || (d->mt != MISSING && d->mt > f->mt);
            if (newer) { ood = 1; if (!inlist(&q, d->name)) { if (q.n) sbc(&q, ' '); sbs(&q, d->name); } }
            if (pl.n) sbc(&pl, ' ');
            sbs(&pl, d->name);
            if (!inlist(&ca, d->name)) { if (ca.n) sbc(&ca, ' '); sbs(&ca, d->name); }
        }
    }
    if (err) { f->state = 2; f->err = 2; return 1; }
    if (ood) {
        if (rec) {
            au.at = f->name; au.lt = lt ? lt : ""; au.ca = sbz(&ca); au.pl = sbz(&pl); au.q = sbz(&q); au.st = stem; au.oo = sbz(&oos);
            int st = run_rec(f, rec);
            au.at = au.lt = au.ca = au.pl = au.q = au.st = au.oo = NULL;
            if (st) { f->state = 2; f->err = 1; return 1; }
            if (rec_ran) f->ran = 1;
            f->mt = mtime(f->name);
        }
        f->upd = 1;
    }
    f->state = 2;
    return 0;
}

/* ---------------------------------------------------------------- main */
static void setflag(char c) {
    switch (c) { case 'n': dry = 1; break; case 's': silent = 1; break; case 'k': keep = 1; break; case 'i': ign_all = 1; break;
                 case 'B': always = 1; break; case 'r': no_builtin = 1; break; }
}
static void usage(const char *bad) {
    if (bad) fprintf(stderr, "%s: invalid option -- '%s'\n", prog, bad);
    fputs("Usage: make [options] [target] ...\n  -f FILE  -C DIR  -n  -s  -k  -i  -B  -r  -j N  VAR=value\n", stderr);
    exit(2);
}
static int is_assign_arg(const char *a) {
    const char *e = strchr(a, '=');
    if (!e || e == a) return 0;
    for (const char *p = a; p < e; p++) if (isspace((unsigned char)*p) || *p == ':') return 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *b = strrchr(argv[0], '/');
    prog = b ? b + 1 : argv[0];
    SV files = {0}, goals = {0};
    const char *ml = getenv("MAKELEVEL");
    level = ml ? atoi(ml) : 0;
    const char *mf = getenv("MAKEFLAGS");
    if (mf) for (; *mf && *mf != ' '; mf++) setflag(*mf);
    vset("CC", DEFAULT_CC, 0, O_DEF); vset("MAKE", argv[0], 1, O_DEF); vset("RM", "rm -f", 0, O_DEF);
    vset("AR", "ar", 0, O_DEF);
    vset("COMPILE.c", "$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c", 0, O_DEF);
    vset("LINK.c", "$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)", 0, O_DEF);
    vset("LINK.o", "$(CC) $(LDFLAGS) $(TARGET_ARCH)", 0, O_DEF);
    vset("OUTPUT_OPTION", "-o $@", 0, O_DEF);
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') {
            if (!strcmp(a, "--version")) { puts("make (VibeOS) 1.0"); return 0; }
            static const struct { const char *n; char c; } lo[] = { { "--just-print", 'n' }, { "--dry-run", 'n' }, { "--recon", 'n' },
                { "--silent", 's' }, { "--quiet", 's' }, { "--keep-going", 'k' }, { "--always-make", 'B' }, { "--ignore-errors", 'i' },
                { "--no-builtin-rules", 'r' } };
            int hit = 0;
            for (unsigned k = 0; k < sizeof lo / sizeof *lo; k++) if (!strcmp(a, lo[k].n)) { setflag(lo[k].c); hit = 1; }
            if (hit || !strcmp(a, "--no-print-directory")) continue;
            if (!strncmp(a, "--file=", 7) || !strncmp(a, "--makefile=", 11)) { svadd(&files, strchr(a, '=') + 1); continue; }
            if (!strncmp(a, "--directory=", 12)) { if (chdir(a + 12) < 0) { warn("*** %s: %s.  Stop.", a + 12, strerror(errno)); return 2; } print_dir = 1; continue; }
            if (!strcmp(a, "--")) continue;
            usage(a + 2);
        } else if (a[0] == '-' && a[1]) {
            for (char *f = a + 1; *f; f++) {
                if (*f == 'f' || *f == 'C') {
                    const char *v = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
                    if (!v) { fprintf(stderr, "%s: option requires an argument -- '%c'\n", prog, *f); usage(NULL); }
                    if (*f == 'f') svadd(&files, (char *)v);
                    else { if (chdir(v) < 0) { warn("*** %s: %s.  Stop.", v, strerror(errno)); return 2; } print_dir = 1; }
                    break;
                } else if (*f == 'j') {
                    if (!f[1] && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) i++;
                    break;
                } else if (*f == 'w') ;
                else if (*f == 'e') env_org = O_ENVE;
                else if (strchr("nskiBr", *f)) setflag(*f);
                else { char t[2] = { *f, 0 }; usage(t); }
            }
        } else if (is_assign_arg(a)) {
            char *eq = strchr(a, '=');
            char *nm = mdup(a, eq - a);
            vset(nm, eq + 1, 0, O_CMD)->exp = 1;
        } else svadd(&goals, a);
    }
    for (char **e = environ; e && *e; e++) {             /* environment: below the makefile (above it with -e) */
        char *eq = strchr(*e, '=');
        if (eq && eq != *e) { char *nm = mdup(*e, eq - *e); if (strcmp(nm, "MAKEFLAGS") && strcmp(nm, "MAKELEVEL")) vset(nm, eq + 1, 0, env_org); }
    }
    if (silent) print_dir = 0;
    char fl[16] = "", *fp = fl;
    if (dry) *fp++ = 'n';
    if (silent) *fp++ = 's';
    if (keep) *fp++ = 'k';
    if (ign_all) *fp++ = 'i';
    if (always) *fp++ = 'B';
    if (no_builtin) *fp++ = 'r';
    *fp = 0;
    setenv("MAKEFLAGS", fl, 1);
    char lv[16];
    snprintf(lv, sizeof lv, "%d", level + 1);
    setenv("MAKELEVEL", lv, 1);
    if (level && !silent) print_dir = 1;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    vset("CURDIR", cwd, 1, O_DEF);
    SB mg = {0};
    for (int i = 0; i < goals.n; i++) sbw(&mg, goals.v[i], strlen(goals.v[i]));
    vset("MAKECMDGOALS", sbz(&mg), 1, O_DEF);
    if (print_dir) { pre(stdout); printf("Entering directory '%s'\n", cwd); fflush(stdout); did_dir = 1; atexit(leaving); }
    if (!no_builtin) {
        add_builtin("%", "%.o", "$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@");
        add_builtin("%", "%.c", "$(LINK.c) $^ $(LOADLIBES) $(LDLIBS) -o $@");
        add_builtin("%.o", "%.c", "$(COMPILE.c) $(OUTPUT_OPTION) $<");
    }
    static const char *const dsuf[] = { ".out", ".a", ".o", ".c", ".cc", ".cpp", ".s", ".S", ".h", ".l", ".y", ".sh" };
    for (unsigned i = 0; i < sizeof dsuf / sizeof *dsuf; i++) svadd(&suffixes, (char *)dsuf[i]);
    if (!files.n) {
        static const char *const dflt[] = { "GNUmakefile", "makefile", "Makefile" };
        for (int i = 0; i < 3; i++) if (mtime(dflt[i]) != MISSING) { svadd(&files, (char *)dflt[i]); break; }
    }
    if (!files.n && !goals.n) { cur_fn = NULL; warn("*** No targets specified and no makefile found.  Stop."); return 2; }
    for (int i = 0; i < files.n; i++) {
        char *buf = read_file(files.v[i]);
        if (!buf) { warn("%s: %s", files.v[i], strerror(errno)); die("No rule to make target '%s'", files.v[i]); }
        Var *m = vfind("MAKEFILE_LIST", 13);
        SB l = {0};
        sbs(&l, m ? m->val : ""); if (l.n) sbc(&l, ' '); sbs(&l, files.v[i]);
        vset("MAKEFILE_LIST", sbz(&l), 1, O_DEF);
        cur.in = 0; cn = 0;
        parse_buf(buf, files.v[i]);
    }
    cur_fn = NULL; cur_ln = 0;
    finalize_suffix();
    for (int h = 0; h < 512; h++)                        /* the environment the recipes see */
        for (Var *v = vt[h]; v; v = v->next) if (v->val && (v->exp || (export_all && v->org >= O_FILE))) setenv(v->name, expand(v->val), 1);
    if (!goals.n) {
        Var *dg = vfind(".DEFAULT_GOAL", 13);
        if (dg && *dg->val) svadd(&goals, dg->val);
        else if (def_goal) svadd(&goals, def_goal);
        else { warn("*** No targets.  Stop."); return 2; }
    }
    int rc = 0;
    for (int i = 0; i < goals.n; i++) {
        File *g = fget(goals.v[i], 1);
        int e = update(g, NULL);
        if (e) {
            if (!keep) return 2;
            rc = 2;
            if (g->err == 2) warn("Target '%s' not remade because of errors.", g->name);
        } else if (!g->ran && !silent) { pre(stdout); printf(g->phony || !g->hascmd ? "Nothing to be done for '%s'.\n" : "'%s' is up to date.\n", g->name); fflush(stdout); }
    }
    return rc;
}
