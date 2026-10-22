/* expand.c - variables, word expansion ($x ${x:-y} $(cmd) $((1+2)) quotes ~ field splitting) and globbing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "sh.h"

static int glob_expand(const char *pat, struct vec *out);

/* ---- globbing ---- */
int gmatch(const char *p, const char *s) {
    for (;; p++, s++) {
        switch (*p) {
        case 0: return !*s;
        case '*':
            while (p[1] == '*') p++;
            for (p++;; s++) { if (gmatch(p, s)) return 1; if (!*s) return 0; }
        case '?': if (!*s) return 0; break;
        case '[': {
            const char *q = p + 1;
            int neg = 0, ok = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            if (!*s) return 0;
            for (int first = 1; *q && (*q != ']' || first); first = 0, q++) {
                char lo = *q, hi;
                if (lo == '\\' && q[1]) lo = *++q;
                hi = lo;
                if (q[1] == '-' && q[2] && q[2] != ']') { hi = q[2]; q += 2; }
                if (*s >= lo && *s <= hi) ok = 1;
            }
            if (!*q) { if (*s != '[') return 0; break; }       /* no closing ]: literal '[' */
            if (ok == neg) return 0;
            p = q;
            break;
        }
        case '\\': if (p[1]) p++;
            /* fall through */
        default: if (*p != *s) return 0;
        }
    }
}

static int has_wild(const char *s) {
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; continue; }
        if (*s == '*' || *s == '?' || *s == '[') return 1;
    }
    return 0;
}

static char *join(const char *a, const char *b, const char *c) {
    char *r = malloc(strlen(a) + strlen(b) + strlen(c) + 1);
    strcpy(r, a); strcat(r, b); strcat(r, c);
    return r;
}

static int cmp_s(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int isdir_(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* base = directory prefix already matched (ends in '/' or empty); pat = remaining pattern */
static void gl(const char *base, const char *pat, struct vec *out) {
    if (out->n > 4096) return;
    size_t sl = strcspn(pat, "/");
    const char *rest = pat[sl] ? pat + sl + 1 : NULL;
    int dironly = 0;
    if (rest && !*rest) { rest = NULL; dironly = 1; }
    if (!sl) { if (rest) gl(base, rest, out); return; }
    char seg[NAME_MAX + 1];
    if (sl > NAME_MAX) return;
    memcpy(seg, pat, sl); seg[sl] = 0;
    if (!has_wild(seg)) {
        char lit[NAME_MAX + 1], *d = lit;
        for (const char *s = seg; *s; s++) { if (*s == '\\' && s[1]) s++; *d++ = *s; }
        *d = 0;
        char *np = join(base, lit, "");
        struct stat st;
        if (stat(np, &st) < 0 || ((rest || dironly) && !S_ISDIR(st.st_mode))) { free(np); return; }
        if (rest) { char *b2 = join(np, "/", ""); gl(b2, rest, out); free(b2); free(np); }
        else if (dironly) { vec_add(out, join(np, "/", "")); free(np); }
        else vec_add(out, np);
        return;
    }
    DIR *d = opendir(*base ? base : ".");
    if (!d) return;
    struct vec names = { 0 };
    struct dirent *de;
    while ((de = readdir(d)))
        if ((de->d_name[0] != '.' || seg[0] == '.') && strcmp(de->d_name, ".") && strcmp(de->d_name, "..") &&
            gmatch(seg, de->d_name))
            vec_add(&names, strdup(de->d_name));
    closedir(d);
    if (names.n) qsort(names.v, names.n, sizeof(char *), cmp_s);
    for (int i = 0; i < names.n; i++) {
        char *np = join(base, names.v[i], "");
        free(names.v[i]);
        if ((rest || dironly) && !isdir_(np)) { free(np); continue; }
        if (rest) { char *b2 = join(np, "/", ""); gl(b2, rest, out); free(b2); free(np); }
        else if (dironly) { vec_add(out, join(np, "/", "")); free(np); }
        else vec_add(out, np);
    }
    free(names.v);
}

static int glob_expand(const char *pat, struct vec *out) {
    int before = out->n;
    const char *p = pat, *base = "";
    if (*p == '/') { base = "/"; while (*p == '/') p++; }
    gl(base, p, out);
    return out->n - before;
}


/* ---- variables: exported ones live in environ, plain ones in a private table ---- */
static struct lv { char *name, *val; } *lv;
static int nlv;

static int lv_find(const char *name) {
    for (int i = 0; i < nlv; i++) if (!strcmp(lv[i].name, name)) return i;
    return -1;
}
static void lv_del(int i) { free(lv[i].name); free(lv[i].val); lv[i] = lv[--nlv]; }

const char *var_get(const char *name) {
    int i = lv_find(name);
    return i >= 0 ? lv[i].val : getenv(name);
}
int var_exported(const char *name) { return getenv(name) != NULL; }
void var_set(const char *name, const char *val) {
    if (getenv(name)) { setenv(name, val, 1); return; }
    int i = lv_find(name);
    if (i >= 0) { free(lv[i].val); lv[i].val = strdup(val); return; }
    lv = realloc(lv, (nlv + 1) * sizeof *lv);
    lv[nlv].name = strdup(name); lv[nlv].val = strdup(val);
    nlv++;
}
void var_export(const char *name, const char *val) {
    int i = lv_find(name);
    char *keep = NULL;
    if (!val) {
        if (i >= 0) val = keep = strdup(lv[i].val);
        else if (getenv(name)) return;
        else val = "";
    }
    if (i >= 0) lv_del(i);
    setenv(name, val, 1);
    free(keep);
}
void var_unset(const char *name) {
    int i = lv_find(name);
    if (i >= 0) lv_del(i);
    unsetenv(name);
}
char **var_envp(struct vec *prefix, int with_locals) {
    int ne = 0, np = prefix ? prefix->n : 0;
    while (environ && environ[ne]) ne++;
    char **e = malloc((np + ne + (with_locals ? nlv : 0) + 1) * sizeof *e);
    int k = 0;
    for (int i = 0; i < np; i++) e[k++] = prefix->v[i];          /* prefix assignments first: getenv finds the first match */
    for (int i = 0; i < ne; i++) e[k++] = environ[i];
    if (with_locals)
        for (int i = 0; i < nlv; i++) {
            char *s = malloc(strlen(lv[i].name) + strlen(lv[i].val) + 2);
            sprintf(s, "%s=%s", lv[i].name, lv[i].val);
            e[k++] = s;                                             /* ponytail: leaked per child shell, tiny */
        }
    e[k] = 0;
    return e;
}
void var_list(int (*fn)(const char *name, const char *val, int exported)) {
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *nm = strndup(*e, eq - *e);
        fn(nm, eq + 1, 1);
        free(nm);
    }
    for (int i = 0; i < nlv; i++) fn(lv[i].name, lv[i].val, 0);
}

void set_pos(int argc, char **argv) {
    char **nv = NULL;
    if (argc > 0) {                      /* copy first: argv may point into the old vector */
        nv = malloc((argc + 1) * sizeof *nv);
        for (int i = 0; i < argc; i++) nv[i] = strdup(argv[i]);
        nv[argc] = 0;
    }
    for (int i = 0; i < g_sargc; i++) free(g_sargv[i]);
    free(g_sargv);
    g_sargv = nv;
    g_sargc = argc > 0 ? argc : 0;
}

static const char *ifs(void) { const char *v = var_get("IFS"); return v ? v : " \t\n"; }

/* value of a parameter (malloc'd), NULL if unset */
static char *lookup(const char *name) {
    char tmp[24];
    if (isdigit((unsigned char)name[0])) {
        int i = atoi(name);
        if (i == 0) return strdup(g_sargc ? g_sargv[0] : "sh");
        return i < g_sargc ? strdup(g_sargv[i]) : NULL;
    }
    if (!name[1]) switch (name[0]) {
    case '?': snprintf(tmp, sizeof tmp, "%d", g_status); return strdup(tmp);
    case '$': snprintf(tmp, sizeof tmp, "%d", getpid()); return strdup(tmp);
    case '!': snprintf(tmp, sizeof tmp, "%d", g_lastbg); return strdup(tmp);
    case '#': snprintf(tmp, sizeof tmp, "%d", g_sargc > 1 ? g_sargc - 1 : 0); return strdup(tmp);
    case '-': snprintf(tmp, sizeof tmp, "%s%s", g_errexit ? "e" : "", g_xtrace ? "x" : ""); return strdup(tmp);
    case '@': case '*': {
        struct sb b = { 0 };
        for (int i = 1; i < g_sargc; i++) { if (i > 1) sb_add(&b, ' '); sb_adds(&b, g_sargv[i]); }
        return sb_take(&b);
    }
    }
    if (!strcmp(name, "RANDOM")) { snprintf(tmp, sizeof tmp, "%d", rand() & 32767); return strdup(tmp); }
    const char *v = var_get(name);
    return v ? strdup(v) : NULL;
}

/* ---- arithmetic ---- */
static const char *ap;
static int aerr;
static void aws(void) { while (*ap == ' ' || *ap == '\t' || *ap == '\n') ap++; }
static long abin(int minprec);
static long aprim(void) {
    aws();
    char c = *ap;
    if (c == '(') { ap++; long v = abin(1); aws(); if (*ap == ')') ap++; else aerr = 1; return v; }
    if (c == '-') { ap++; return -aprim(); }
    if (c == '+') { ap++; return aprim(); }
    if (c == '!') { ap++; return !aprim(); }
    if (c == '~') { ap++; return ~aprim(); }
    if (isdigit((unsigned char)c)) {
        char *e;
        long v = strtol(ap, &e, ap[0] == '0' && (ap[1] == 'x' || ap[1] == 'X') ? 16 : 10);
        ap = e;
        return v;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        char nm[64];
        int n = 0;
        while ((isalnum((unsigned char)*ap) || *ap == '_') && n < 63) nm[n++] = *ap++;
        nm[n] = 0;
        char *v = lookup(nm);
        long r = v ? strtol(v, NULL, 10) : 0;
        free(v);
        return r;
    }
    aerr = 1;
    return 0;
}
static long abin(int minprec) {
    static const struct { const char *s; int prec; } ops[] = {
        { "||", 1 }, { "&&", 2 }, { "==", 6 }, { "!=", 6 }, { "<=", 7 }, { ">=", 7 }, { "<<", 8 }, { ">>", 8 },
        { "|", 3 }, { "^", 4 }, { "&", 5 }, { "<", 7 }, { ">", 7 }, { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
    };
    long l = aprim();
    for (;;) {
        aws();
        int k = -1;
        for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++)
            if (!strncmp(ap, ops[i].s, strlen(ops[i].s))) { k = i; break; }
        if (k < 0 || ops[k].prec < minprec) return l;
        const char *o = ops[k].s;
        ap += strlen(o);
        long r = abin(ops[k].prec + 1);
        if (!strcmp(o, "||")) l = l || r;
        else if (!strcmp(o, "&&")) l = l && r;
        else if (!strcmp(o, "==")) l = l == r;
        else if (!strcmp(o, "!=")) l = l != r;
        else if (!strcmp(o, "<=")) l = l <= r;
        else if (!strcmp(o, ">=")) l = l >= r;
        else if (!strcmp(o, "<<")) l <<= r & 31;
        else if (!strcmp(o, ">>")) l >>= r & 31;
        else switch (o[0]) {
        case '|': l |= r; break;
        case '^': l ^= r; break;
        case '&': l &= r; break;
        case '<': l = l < r; break;
        case '>': l = l > r; break;
        case '+': l += r; break;
        case '-': l -= r; break;
        case '*': l *= r; break;
        default:
            if (!r) { fprintf(stderr, "sh: division by zero\n"); aerr = 1; return 0; }
            l = o[0] == '/' ? l / r : l % r;
        }
    }
}

long arith(const char *expr) {
    char *e = expand_one(expr);
    const char *save = ap;
    int se = aerr;
    ap = e; aerr = 0;
    long v = abin(1);
    aws();
    if (*ap) aerr = 1;
    if (aerr) { fprintf(stderr, "sh: arithmetic syntax error: `%s'\n", expr); v = 0; }
    ap = save; aerr = se;
    free(e);
    return v;
}

/* ---- expansion ---- */
struct fld { struct sb lit, pat; int glob, hasq; };
struct ctx { struct fld f; struct vec *out; int mode, hq; };

static void addc(struct fld *f, int c, int quoted) {
    sb_add(&f->lit, c);
    if (quoted && strchr("*?[]\\", c)) sb_add(&f->pat, '\\');
    else if (!quoted && strchr("*?[", c)) f->glob = 1;
    sb_add(&f->pat, c);
}

static void push(struct fld *f, struct vec *out, int mode) {
    if (!f->lit.n && !f->hasq) return;
    char *pat = sb_take(&f->pat), *lit = sb_take(&f->lit);
    if (mode == M_ARGV && f->glob && glob_expand(pat, out)) free(lit);
    else vec_add(out, lit);
    free(pat);
    f->glob = f->hasq = 0;
}

static void put_str(struct ctx *x, const char *v, int quoted) {
    const char *sp = ifs();
    for (const char *q = v; *q; q++) {
        if (!quoted && x->mode == M_ARGV && strchr(sp, *q)) push(&x->f, x->out, M_ARGV);
        else addc(&x->f, *q, quoted || x->mode != M_ARGV);
    }
}

/* ${name op word} ; inner = text between the braces */
static char *param(const char *inner) {
    const char *p = inner;
    int lenop = 0;
    if (*p == '#' && p[1]) { lenop = 1; p++; }
    const char *ns = p;
    if (isdigit((unsigned char)*p)) while (isdigit((unsigned char)*p)) p++;
    else if (*p && strchr("?$#@*!-", *p)) p++;
    else while (isalnum((unsigned char)*p) || *p == '_') p++;
    char *name = strndup(ns, p - ns), *v = lookup(name), *res = NULL;
    if (lenop) {
        char t[16];
        snprintf(t, sizeof t, "%d", v ? (int)strlen(v) : 0);
        res = strdup(t);
    } else if (!*p) res = strdup(v ? v : "");
    else {
        int colon = *p == ':';
        if (colon) p++;
        char op = *p++;
        int null = !v || (colon && !*v);
        switch (op) {
        case '-': res = null ? expand_one(p) : strdup(v); break;
        case '=': if (null) { res = expand_one(p); var_set(name, res); } else res = strdup(v); break;
        case '+': res = null ? strdup("") : expand_one(p); break;
        case '?':
            if (null) {
                char *w = *p ? expand_one(p) : strdup("parameter null or not set");
                fprintf(stderr, "sh: %s: %s\n", name, w);
                free(w);
                if (!g_interactive) { fflush(stdout); exit(1); }
                g_status = 1;
                res = strdup("");
            } else res = strdup(v);
            break;
        case '#': case '%': {
            int longest = *p == op;
            if (longest) p++;
            char *pat = expand_pat(p), *val = strdup(v ? v : "");
            int len = strlen(val);
            res = strdup(val);
            if (op == '#') {
                for (int k = longest ? len : 0; longest ? k >= 0 : k <= len; k += longest ? -1 : 1) {
                    char *pre = strndup(val, k);
                    int m = gmatch(pat, pre);
                    free(pre);
                    if (m) { free(res); res = strdup(val + k); break; }
                }
            } else {
                for (int k = longest ? 0 : len; longest ? k <= len : k >= 0; k += longest ? 1 : -1)
                    if (gmatch(pat, val + k)) { free(res); res = strndup(val, k); break; }
            }
            free(pat); free(val);
            break;
        }
        default: res = strdup(v ? v : "");
        }
    }
    free(name); free(v);
    return res;
}

/* p[0] == '$': expand the parameter / command / arithmetic substitution; returns chars consumed */
static int dollar(struct ctx *x, const char *p, int quoted) {
    const char *q = p + 1;
    char *v = NULL;
    int adv;
    if (*q == '(') {
        int e = skip_paren(p, 2);
        if (e < 0) { addc(&x->f, '$', quoted); return 1; }
        if (q[1] == '(' && e >= 6 && p[e - 2] == ')') {
            char *ex = strndup(p + 3, e - 5), t[24];
            snprintf(t, sizeof t, "%ld", arith(ex));
            free(ex);
            v = strdup(t);
        } else {
            char *t = strndup(p + 2, e - 3);
            v = cmdsub(t);
            free(t);
        }
        adv = e;
    } else if (*q == '{') {
        int e = skip_brace(p, 2);
        if (e < 0) { addc(&x->f, '$', quoted); return 1; }
        char *t = strndup(p + 2, e - 3);
        v = param(t);
        free(t);
        adv = e;
    } else {
        char name[64];
        int n = 0;
        if (isdigit((unsigned char)*q)) name[n++] = *q++;
        else if (*q && strchr("?$#@*!-", *q)) name[n++] = *q++;
        else if (isalpha((unsigned char)*q) || *q == '_') while ((isalnum((unsigned char)*q) || *q == '_') && n < 63) name[n++] = *q++;
        else { addc(&x->f, '$', quoted); return 1; }
        name[n] = 0;
        adv = q - p;
        if (!strcmp(name, "@") && quoted && x->mode == M_ARGV) {      /* "$@": one field per parameter */
            if (g_sargc <= 1 && !x->f.lit.n) x->f.hasq = x->hq;
            for (int i = 1; i < g_sargc; i++) {
                if (i > 1) push(&x->f, x->out, M_ARGV);
                put_str(x, g_sargv[i], 1);
            }
            return adv;
        }
        v = lookup(name);
        if (!v) v = strdup("");
    }
    put_str(x, v, quoted);
    free(v);
    return adv;
}

static int backtick(struct ctx *x, const char *p, int quoted) {
    int e = skip_bq(p, 1);
    if (e < 0) { addc(&x->f, '`', quoted); return 1; }
    struct sb t = { 0 };
    for (int i = 1; i < e - 1; i++) {
        if (p[i] == '\\' && i + 1 < e - 1 && strchr("$`\\", p[i + 1])) i++;
        sb_add(&t, p[i]);
    }
    char *v = cmdsub(t.p ? t.p : "");
    free(t.p);
    put_str(x, v, quoted);
    free(v);
    return e;
}

/* inside "...": here = a here-doc body (runs to the end, quotes are literal) */
static const char *dq(struct ctx *x, const char *p, int here) {
    struct fld *f = &x->f;
    while (*p && (here || *p != '"')) {
        if (*p == '\\' && p[1] == '\n') p += 2;
        else if (*p == '\\' && p[1] && strchr("$\"\\`", p[1]) && !(here && p[1] == '"')) { addc(f, p[1], 1); p += 2; }
        else if (*p == '$') p += dollar(x, p, 1);
        else if (*p == '`') p += backtick(x, p, 1);
        else addc(f, *p++, 1);
    }
    return *p ? p + 1 : p;
}

static void finish(struct ctx *x) {
    if (x->mode == M_ARGV) push(&x->f, x->out, M_ARGV);
    else vec_add(x->out, sb_take(x->mode == M_PAT ? &x->f.pat : &x->f.lit));
    free(x->f.lit.p); free(x->f.pat.p);
}

void expand_word(const char *raw, struct vec *out, int mode) {
    struct ctx x = { .out = out, .mode = mode };
    struct fld *f = &x.f;
    for (const char *p = raw; *p;) {
        char c = *p;
        if (c == '\'') {
            f->hasq = 1;
            for (p++; *p && *p != '\''; p++) addc(f, *p, 1);
            if (*p) p++;
        } else if (c == '"') {
            x.hq = f->hasq;
            f->hasq = 1;
            p = dq(&x, p + 1, 0);
        } else if (c == '\\') {
            if (p[1] == '\n') p += 2;
            else if (p[1]) { addc(f, p[1], 1); p += 2; }
            else p++;
        } else if (c == '$') p += dollar(&x, p, 0);
        else if (c == '`') p += backtick(&x, p, 0);
        else if (c == '~' && p == raw && (p[1] == 0 || p[1] == '/')) {
            const char *h = var_get("HOME");
            for (h = h ? h : "/"; *h; h++) addc(f, *h, 1);
            p++;
        } else { addc(f, c, 0); p++; }
    }
    finish(&x);
}

static char *one(const char *raw, int mode) {
    struct vec v = { 0 };
    expand_word(raw, &v, mode);
    char *r = v.v[0];
    free(v.v);
    return r;
}
char *expand_one(const char *raw) { return one(raw, M_STR); }
char *expand_pat(const char *raw) { return one(raw, M_PAT); }

char *expand_here(const char *raw) {
    struct vec v = { 0 };
    struct ctx x = { .out = &v, .mode = M_STR };
    dq(&x, raw, 1);
    finish(&x);
    char *r = v.v[0];
    free(v.v);
    return r;
}
