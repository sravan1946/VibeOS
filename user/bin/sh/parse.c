/* parse.c - lexer, word expansion (quotes, $vars, ~, field splitting) and globbing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "sh.h"

void sb_add(struct sb *b, int c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->p = realloc(b->p, b->cap); }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}
char *sb_take(struct sb *b) {
    char *r = b->p ? b->p : strdup("");
    b->p = 0; b->n = b->cap = 0;
    return r;
}
void vec_add(struct vec *v, char *s) {
    if (v->n + 2 > v->cap) { v->cap = v->cap ? v->cap * 2 : 8; v->v = realloc(v->v, v->cap * sizeof *v->v); }
    v->v[v->n++] = s;
    v->v[v->n] = 0;
}

/* ---- lexer ---- */
static void tadd(struct toks *t, int type, char *s, int fd, int arg) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->v = realloc(t->v, t->cap * sizeof *t->v); }
    t->v[t->n++] = (struct tok){ type, s, fd, arg };
}

void toks_free(struct toks *t) {
    for (int i = 0; i < t->n; i++) free(t->v[i].s);
    free(t->v);
    t->v = 0; t->n = t->cap = 0;
}

int lex(const char *l, struct toks *t) {
    int i = 0;
    for (;;) {
        while (l[i] == ' ' || l[i] == '\t' || l[i] == '\n' || l[i] == '\r') i++;
        char c = l[i];
        if (!c || c == '#') break;
        if (c == '|') { if (l[i + 1] == '|') { tadd(t, T_OR, 0, 0, 0); i += 2; } else { tadd(t, T_PIPE, 0, 0, 0); i++; } continue; }
        if (c == '&') { if (l[i + 1] == '&') { tadd(t, T_AND, 0, 0, 0); i += 2; } else { tadd(t, T_BG, 0, 0, 0); i++; } continue; }
        if (c == ';') { tadd(t, T_SEMI, 0, 0, 0); i++; continue; }
        int fd = -1;
        if ((c == '0' || c == '1' || c == '2') && (l[i + 1] == '<' || l[i + 1] == '>')) { fd = c - '0'; c = l[++i]; }
        if (c == '<') { tadd(t, T_RIN, 0, fd < 0 ? 0 : fd, 0); i++; continue; }
        if (c == '>') {
            int f = fd < 0 ? 1 : fd;
            i++;
            if (l[i] == '>') { tadd(t, T_RAPP, 0, f, 0); i++; }
            else if (l[i] == '&' && isdigit((unsigned char)l[i + 1])) { tadd(t, T_RDUP, 0, f, l[i + 1] - '0'); i += 2; }
            else tadd(t, T_ROUT, 0, f, 0);
            continue;
        }
        int st = i;
        while ((c = l[i]) && !strchr(" \t\n\r|&;<>", c)) {
            if (c == '\\') { if (!l[i + 1]) return 2; i += 2; continue; }
            if (c == '\'' || c == '"') {
                char q = c;
                for (i++; l[i] && l[i] != q; i++)
                    if (q == '"' && l[i] == '\\' && l[i + 1]) i++;
                if (!l[i]) return 1;
                i++;
                continue;
            }
            i++;
        }
        tadd(t, T_WORD, strndup(l + st, i - st), 0, 0);
    }
    if (t->n && (t->v[t->n - 1].type == T_PIPE || t->v[t->n - 1].type == T_AND || t->v[t->n - 1].type == T_OR)) return 1;
    return 0;
}

/* ---- globbing ---- */
static int gmatch(const char *p, const char *s) {
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

/* ---- expansion ---- */
/* s[0] == '$'; returns malloc'd value and sets *adv to the number of chars consumed */
static char *dollar(const char *s, int *adv) {
    const char *p = s + 1;
    char name[64] = "";
    if (*p == '{') {
        const char *e = strchr(p, '}');
        if (!e || e - p - 1 >= (int)sizeof name) { *adv = 1; return strdup("$"); }
        memcpy(name, p + 1, e - p - 1); name[e - p - 1] = 0;
        p = e + 1;
    } else if (*p && strchr("?$#@*", *p)) { name[0] = *p++; }
    else if (isdigit((unsigned char)*p)) { name[0] = *p++; }
    else if (isalpha((unsigned char)*p) || *p == '_') {
        int n = 0;
        while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
    } else { *adv = 1; return strdup("$"); }
    *adv = p - s;
    char tmp[24];
    if (!strcmp(name, "?")) { snprintf(tmp, sizeof tmp, "%d", g_status); return strdup(tmp); }
    if (!strcmp(name, "$")) { snprintf(tmp, sizeof tmp, "%d", getpid()); return strdup(tmp); }
    if (!strcmp(name, "#")) { snprintf(tmp, sizeof tmp, "%d", g_sargc > 1 ? g_sargc - 1 : 0); return strdup(tmp); }
    if (!strcmp(name, "@") || !strcmp(name, "*")) {      /* ponytail: "$@" joins like "$*" */
        struct sb b = { 0 };
        for (int i = 1; i < g_sargc; i++) { if (i > 1) sb_add(&b, ' '); for (char *q = g_sargv[i]; *q; q++) sb_add(&b, *q); }
        return sb_take(&b);
    }
    if (isdigit((unsigned char)name[0]) && !name[1]) {
        int i = name[0] - '0';
        return strdup(i < g_sargc ? g_sargv[i] : i == 0 ? "sh" : "");
    }
    const char *v = getenv(name);
    return strdup(v ? v : "");
}

struct fld { struct sb lit, pat; int glob, hasq; };

static void addc(struct fld *f, int c, int quoted) {
    sb_add(&f->lit, c);
    if (quoted && strchr("*?[]\\", c)) sb_add(&f->pat, '\\');
    else if (!quoted && strchr("*?[", c)) f->glob = 1;
    sb_add(&f->pat, c);
}

static void push(struct fld *f, struct vec *out, int doglob) {
    if (!f->lit.n && !f->hasq) return;
    char *pat = sb_take(&f->pat), *lit = sb_take(&f->lit);
    if (doglob && f->glob && glob_expand(pat, out)) free(lit);
    else vec_add(out, lit);
    free(pat);
    f->glob = f->hasq = 0;
}

/* split_glob: 1 = argv word (field splitting + globbing), 0 = single string (redirect targets, assignments) */
void expand_word(const char *raw, struct vec *out, int split_glob) {
    struct fld f = { 0 };
    for (const char *p = raw; *p;) {
        char c = *p;
        if (c == '\'') {
            f.hasq = 1;
            for (p++; *p && *p != '\''; p++) addc(&f, *p, 1);
            if (*p) p++;
        } else if (c == '"') {
            f.hasq = 1;
            for (p++; *p && *p != '"';) {
                if (*p == '\\' && p[1] && strchr("$\"\\`", p[1])) { addc(&f, p[1], 1); p += 2; }
                else if (*p == '$') {
                    int adv; char *v = dollar(p, &adv);
                    for (char *q = v; *q; q++) addc(&f, *q, 1);
                    free(v); p += adv;
                } else addc(&f, *p++, 1);
            }
            if (*p) p++;
        } else if (c == '\\') {
            if (p[1]) { addc(&f, p[1], 1); p += 2; } else p++;
        } else if (c == '$') {
            int adv; char *v = dollar(p, &adv);
            for (char *q = v; *q; q++) {
                if (split_glob && isspace((unsigned char)*q)) push(&f, out, 1);
                else addc(&f, *q, !split_glob);
            }
            free(v); p += adv;
        } else if (c == '~' && p == raw && (p[1] == 0 || p[1] == '/')) {
            const char *h = getenv("HOME");
            for (h = h ? h : "/"; *h; h++) addc(&f, *h, 1);
            p++;
        } else { addc(&f, c, 0); p++; }
    }
    push(&f, out, split_glob);
    free(f.lit.p); free(f.pat.p);
}

char *expand_one(const char *raw) {
    struct vec v = { 0 };
    expand_word(raw, &v, 0);
    char *r = v.n ? v.v[0] : strdup("");
    free(v.v);
    return r;
}
