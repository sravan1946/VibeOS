/* parse.c - lexer + recursive-descent parser: text -> AST (arena allocated).
 * P_INCOMPLETE means "feed me another line" (open quote, unclosed compound command, pending here-doc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sh.h"

char g_perr[160];

void sb_add(struct sb *b, int c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->p = realloc(b->p, b->cap); }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}
void sb_adds(struct sb *b, const char *s) { while (*s) sb_add(b, *s++); }
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

/* ---- arena ---- */
struct arena { struct arena *next; size_t used, cap; char d[]; };

struct hd { struct redir *r; char *delim; int strip; };
enum { TK_EOF, TK_NL, TK_WORD, TK_PIPE, TK_OR, TK_AND, TK_BG, TK_SEMI, TK_DSEMI, TK_LP, TK_RP,
       TK_RIN, TK_ROUT, TK_RAPP, TK_HERE, TK_DUP };
struct P {
    char *s;                    /* text being parsed (private copy; aliases splice into it) */
    int i;                      /* lexer position */
    struct arena *ar;
    int have, tt, ts, te, tfd, targ, tstrip;   /* one token of lookahead */
    char *tw;
    int last_end;               /* end offset of the last consumed token */
    struct hd hd[16];
    int nhd, inc, err, noalias, nalias;
};

static void *aalloc(struct P *P, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!P->ar || P->ar->used + n > P->ar->cap) {
        size_t cap = n > 4000 ? n : 4000;
        struct arena *a = malloc(sizeof *a + cap);
        a->next = P->ar; a->used = 0; a->cap = cap;
        P->ar = a;
    }
    void *r = P->ar->d + P->ar->used;
    P->ar->used += n;
    memset(r, 0, n);
    return r;
}
static char *adup(struct P *P, const char *s, size_t n) {
    char *r = aalloc(P, n + 1);
    memcpy(r, s, n);
    return r;
}
static char **vcopy(struct P *P, struct vec *v) {
    char **r = aalloc(P, (v->n + 1) * sizeof *r);
    for (int i = 0; i < v->n; i++) r[i] = v->v[i];
    free(v->v);
    return r;
}
void prog_free(struct prog *p) {
    for (struct arena *a = p->ar, *n; a; a = n) { n = a->next; free(a); }
    p->ar = 0; p->root = 0;
}
static struct node *mk(struct P *P, int type) {
    struct node *n = aalloc(P, sizeof *n);
    n->type = type;
    return n;
}

/* ---- skipping over quoted / nested constructs (also used by the expander) ---- */
int skip_bq(const char *s, int j) {
    for (; s[j]; j++) {
        if (s[j] == '\\' && s[j + 1]) j++;
        else if (s[j] == '`') return j + 1;
    }
    return -1;
}
static int skip_dq(const char *s, int j);
int skip_paren(const char *s, int j) {
    for (int depth = 1; s[j];) {
        char c = s[j];
        if (c == '\\' && s[j + 1]) j += 2;
        else if (c == '\'') { const char *e = strchr(s + j + 1, '\''); if (!e) return -1; j = e - s + 1; }
        else if (c == '"') { if ((j = skip_dq(s, j + 1)) < 0) return -1; }
        else if (c == '`') { if ((j = skip_bq(s, j + 1)) < 0) return -1; }
        else if (c == '$' && s[j + 1] == '{') { if ((j = skip_brace(s, j + 2)) < 0) return -1; }
        else if (c == '(') { depth++; j++; }
        else if (c == ')') { j++; if (!--depth) return j; }
        else j++;
    }
    return -1;
}
int skip_brace(const char *s, int j) {
    for (int depth = 1; s[j];) {
        char c = s[j];
        if (c == '\\' && s[j + 1]) j += 2;
        else if (c == '\'') { const char *e = strchr(s + j + 1, '\''); if (!e) return -1; j = e - s + 1; }
        else if (c == '"') { if ((j = skip_dq(s, j + 1)) < 0) return -1; }
        else if (c == '`') { if ((j = skip_bq(s, j + 1)) < 0) return -1; }
        else if (c == '$' && s[j + 1] == '(') { if ((j = skip_paren(s, j + 2)) < 0) return -1; }
        else if (c == '{') { depth++; j++; }
        else if (c == '}') { j++; if (!--depth) return j; }
        else j++;
    }
    return -1;
}
static int skip_dq(const char *s, int j) {         /* j just after the opening quote */
    while (s[j]) {
        char c = s[j];
        if (c == '\\' && s[j + 1]) j += 2;
        else if (c == '"') return j + 1;
        else if (c == '`') { if ((j = skip_bq(s, j + 1)) < 0) return -1; }
        else if (c == '$' && s[j + 1] == '(') { if ((j = skip_paren(s, j + 2)) < 0) return -1; }
        else if (c == '$' && s[j + 1] == '{') { if ((j = skip_brace(s, j + 2)) < 0) return -1; }
        else j++;
    }
    return -1;
}

/* ---- lexer ---- */
static void read_heredocs(struct P *P, int *pi) {
    const char *s = P->s;
    int i = *pi;
    for (int k = 0; k < P->nhd; k++) {
        struct hd *h = &P->hd[k];
        size_t dl = strlen(h->delim);
        struct sb body = { 0 };
        for (;;) {
            if (!s[i]) { P->inc = 1; break; }
            int e = i;
            while (s[e] && s[e] != '\n') e++;
            int st = i;
            if (h->strip) while (st < e && s[st] == '\t') st++;
            int next = s[e] ? e + 1 : e;
            if ((size_t)(e - st) == dl && !strncmp(s + st, h->delim, dl)) { i = next; break; }
            for (int q = st; q < e; q++) sb_add(&body, s[q]);
            sb_add(&body, '\n');
            i = next;
        }
        h->r->word = adup(P, body.p ? body.p : "", body.n);
        free(body.p);
    }
    P->nhd = 0;
    *pi = i;
}

static void lex(struct P *P) {
    const char *s = P->s;
    int i = P->i, t;
    for (;;) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') i++;
        else if (s[i] == '\\' && s[i + 1] == '\n') i += 2;
        else if (s[i] == '#') while (s[i] && s[i] != '\n') i++;
        else break;
    }
    P->ts = i; P->tfd = -1; P->tw = 0; P->targ = 0; P->tstrip = 0;
    char c = s[i];
    if (!c) { t = TK_EOF; if (P->nhd) P->inc = 1; }
    else if (c == '\n') { i++; t = TK_NL; read_heredocs(P, &i); }
    else if (c == '|') { if (s[i + 1] == '|') { t = TK_OR; i += 2; } else { t = TK_PIPE; i++; } }
    else if (c == '&') { if (s[i + 1] == '&') { t = TK_AND; i += 2; } else { t = TK_BG; i++; } }
    else if (c == ';') { if (s[i + 1] == ';') { t = TK_DSEMI; i += 2; } else { t = TK_SEMI; i++; } }
    else if (c == '(') { t = TK_LP; i++; }
    else if (c == ')') { t = TK_RP; i++; }
    else {
        int fd = -1, j = i;
        while (isdigit((unsigned char)s[j])) j++;
        if (j > i && (s[j] == '<' || s[j] == '>')) { fd = j - i == 1 ? s[i] - '0' : 99; i = j; c = s[i]; }
        if (c == '<') {
            if (s[i + 1] == '<') { i += 2; t = TK_HERE; if (s[i] == '-') { P->tstrip = 1; i++; } }
            else if (s[i + 1] == '&' && isdigit((unsigned char)s[i + 2])) { t = TK_DUP; P->targ = s[i + 2] - '0'; i += 3; }
            else { t = TK_RIN; i++; }
            P->tfd = fd < 0 ? 0 : fd;
        } else if (c == '>') {
            if (s[i + 1] == '>') { t = TK_RAPP; i += 2; }
            else if (s[i + 1] == '&' && isdigit((unsigned char)s[i + 2])) { t = TK_DUP; P->targ = s[i + 2] - '0'; i += 3; }
            else { t = TK_ROUT; i++; if (s[i] == '|') i++; }
            P->tfd = fd < 0 ? 1 : fd;
        } else {
            int st = i;
            t = TK_WORD;
            while ((c = s[i]) && !strchr(" \t\n\r|&;()<>", c)) {
                if (c == '\\') { if (!s[i + 1]) { P->inc = 1; break; } i += 2; }
                else if (c == '\'') { const char *e = strchr(s + i + 1, '\''); if (!e) { P->inc = 1; i = strlen(s); break; } i = e - s + 1; }
                else if (c == '"') { if ((i = skip_dq(s, i + 1)) < 0) { P->inc = 1; i = strlen(s); break; } }
                else if (c == '`') { if ((i = skip_bq(s, i + 1)) < 0) { P->inc = 1; i = strlen(s); break; } }
                else if (c == '$' && s[i + 1] == '(') { if ((i = skip_paren(s, i + 2)) < 0) { P->inc = 1; i = strlen(s); break; } }
                else if (c == '$' && s[i + 1] == '{') { if ((i = skip_brace(s, i + 2)) < 0) { P->inc = 1; i = strlen(s); break; } }
                else i++;
            }
            P->tw = adup(P, s + st, i - st);
        }
    }
    P->te = P->i = i;
    P->tt = t;
    P->have = 1;
}

static int peek(struct P *P) { if (!P->have) lex(P); return P->tt; }
static void take(struct P *P) { peek(P); P->have = 0; P->last_end = P->te; }
static int is_kw(struct P *P, const char *kw) { return peek(P) == TK_WORD && !strcmp(P->tw, kw); }

static void bad(struct P *P) {
    if (peek(P) == TK_EOF) { P->inc = 1; return; }
    if (P->err) return;
    P->err = 1;
    if (P->tt == TK_NL) snprintf(g_perr, sizeof g_perr, "syntax error near unexpected token `newline'");
    else snprintf(g_perr, sizeof g_perr, "syntax error near unexpected token `%.*s'", P->te - P->ts > 20 ? 20 : P->te - P->ts, P->s + P->ts);
}

static int expect_kw(struct P *P, const char *kw) {
    if (is_kw(P, kw)) { take(P); return 1; }
    bad(P);
    return 0;
}
static void skip_nl(struct P *P) { while (peek(P) == TK_NL) take(P); }

int is_name(const char *s, int len) {
    if (len <= 0 || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (int i = 1; i < len; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}

/* ---- parser ---- */
static struct node *parse_list(struct P *P);
static struct node *parse_command(struct P *P);

static int is_term(struct P *P) {
    static const char *const kw[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}" };
    int t = peek(P);
    if (t == TK_EOF || t == TK_RP || t == TK_DSEMI) return 1;
    if (t == TK_WORD) for (unsigned i = 0; i < sizeof kw / sizeof *kw; i++) if (!strcmp(P->tw, kw[i])) return 1;
    return 0;
}

static int is_redir(int t) { return t >= TK_RIN && t <= TK_DUP; }

static int parse_redir(struct P *P, struct redir ***rt) {
    int t = peek(P);
    struct redir *r = aalloc(P, sizeof *r);
    r->type = t == TK_RIN ? R_IN : t == TK_ROUT ? R_OUT : t == TK_RAPP ? R_APP : t == TK_DUP ? R_DUP : R_HERE;
    r->fd = P->tfd; r->arg = P->targ;
    int strip = P->tstrip;
    take(P);
    if (t != TK_DUP) {
        if (peek(P) != TK_WORD) { bad(P); return 0; }
        r->word = P->tw;
        take(P);
        if (t == TK_HERE) {
            if (P->nhd >= 16) { P->err = 1; snprintf(g_perr, sizeof g_perr, "too many here-documents"); return 0; }
            char *d = aalloc(P, strlen(r->word) + 1);
            int k = 0;
            for (char *q = r->word; *q; q++) {
                if (*q == '\'' || *q == '"' || *q == '\\') r->quoted = 1; else d[k++] = *q;
            }
            P->hd[P->nhd++] = (struct hd){ r, d, strip };
            r->word = 0;
        }
    }
    **rt = r;
    *rt = &r->next;
    return 1;
}

static struct node *parse_simple(struct P *P) {
    struct node *n = mk(P, N_CMD);
    struct vec w = { 0 };
    struct redir **rt = &n->rd;
    for (;;) {
        int t = peek(P);
        if (t == TK_WORD) {
            char *raw = P->tw;
            take(P);
            if (!w.n && is_name(raw, strlen(raw)) && peek(P) == TK_LP) {       /* name() body */
                take(P);
                if (peek(P) != TK_RP) { bad(P); free(w.v); return NULL; }
                take(P);
                skip_nl(P);
                struct node *body = parse_command(P);
                if (!body) { free(w.v); return NULL; }
                struct node *f = mk(P, N_FUNC);
                f->s = raw; f->a = body; f->src = body->src;
                return f;
            }
            vec_add(&w, raw);
        } else if (is_redir(t)) {
            if (!parse_redir(P, &rt)) { free(w.v); return NULL; }
        } else break;
    }
    if (!w.n && !n->rd) { free(w.v); bad(P); return NULL; }
    n->nw = w.n;
    n->w = vcopy(P, &w);
    return n;
}

static struct node *parse_if(struct P *P) {         /* "if" or "elif" already consumed */
    struct node *n = mk(P, N_IF);
    if (!(n->a = parse_list(P)) || !expect_kw(P, "then") || !(n->b = parse_list(P))) return NULL;
    if (is_kw(P, "elif")) { take(P); return (n->c = parse_if(P)) ? n : NULL; }
    if (is_kw(P, "else")) { take(P); if (!(n->c = parse_list(P))) return NULL; }
    return expect_kw(P, "fi") ? n : NULL;
}

static struct node *parse_for(struct P *P) {
    struct node *n = mk(P, N_FOR);
    if (peek(P) != TK_WORD || !is_name(P->tw, strlen(P->tw))) { bad(P); return NULL; }
    n->s = P->tw;
    take(P);
    skip_nl(P);
    if (is_kw(P, "in")) {
        struct vec w = { 0 };
        take(P);
        n->hasin = 1;
        while (peek(P) == TK_WORD) { vec_add(&w, P->tw); take(P); }
        n->nw = w.n;
        n->w = vcopy(P, &w);
    }
    while (peek(P) == TK_SEMI || P->tt == TK_NL) take(P);
    if (!expect_kw(P, "do") || !(n->a = parse_list(P)) || !expect_kw(P, "done")) return NULL;
    return n;
}

static struct node *parse_case(struct P *P) {
    struct node *n = mk(P, N_CASE);
    struct citem **it = &n->items;
    if (peek(P) != TK_WORD) { bad(P); return NULL; }
    n->s = P->tw;
    take(P);
    skip_nl(P);
    if (!expect_kw(P, "in")) return NULL;
    for (;;) {
        skip_nl(P);
        if (is_kw(P, "esac")) { take(P); return n; }
        if (peek(P) == TK_EOF) { bad(P); return NULL; }
        if (P->tt == TK_LP) take(P);
        struct vec pv = { 0 };
        for (;;) {
            if (peek(P) != TK_WORD) { bad(P); free(pv.v); return NULL; }
            vec_add(&pv, P->tw);
            take(P);
            if (peek(P) == TK_PIPE) { take(P); continue; }
            break;
        }
        if (peek(P) != TK_RP) { bad(P); free(pv.v); return NULL; }
        take(P);
        struct citem *c = aalloc(P, sizeof *c);
        c->pats = vcopy(P, &pv);
        if (!(c->body = parse_list(P))) return NULL;
        *it = c; it = &c->next;
        if (peek(P) == TK_DSEMI) take(P);
        else if (!is_kw(P, "esac")) { bad(P); return NULL; }
    }
}

static struct node *parse_command(struct P *P) {
    struct node *n = NULL;
    int t = peek(P), start, end = -1;
again:
    t = peek(P);
    start = P->ts;
    if (t == TK_WORD) {
        const char *w = P->tw;
        if (!strcmp(w, "if")) { take(P); n = parse_if(P); }
        else if (!strcmp(w, "while") || !strcmp(w, "until")) {
            n = mk(P, w[0] == 'w' ? N_WHILE : N_UNTIL);
            take(P);
            if (!(n->a = parse_list(P)) || !expect_kw(P, "do") || !(n->b = parse_list(P)) || !expect_kw(P, "done")) n = NULL;
        }
        else if (!strcmp(w, "for")) { take(P); n = parse_for(P); }
        else if (!strcmp(w, "case")) { take(P); n = parse_case(P); }
        else if (!strcmp(w, "{")) {
            take(P);
            n = mk(P, N_GROUP);
            if (!(n->a = parse_list(P)) || !expect_kw(P, "}")) n = NULL;
        } else {
            const char *av = alias_get(w);
            if (av && P->noalias != start && P->nalias++ < 16) {          /* splice the alias text in and re-lex */
                size_t al = strlen(av), rest = strlen(P->s + P->te);
                char *ns = malloc(start + al + rest + 1);
                memcpy(ns, P->s, start);
                memcpy(ns + start, av, al);
                memcpy(ns + start + al, P->s + P->te, rest + 1);
                free(P->s);
                P->s = ns; P->i = start; P->have = 0; P->noalias = start;
                goto again;
            }
            n = parse_simple(P);
            if (n && n->type == N_CMD) n->src = adup(P, P->s + start, P->last_end - start);
            return n;
        }
        if (n) end = P->last_end;
    } else if (t == TK_LP) {
        take(P);
        int inner = P->last_end;
        n = mk(P, N_SUB);
        if (!(n->a = parse_list(P))) return NULL;
        if (peek(P) != TK_RP) { bad(P); return NULL; }
        n->s = adup(P, P->s + inner, P->ts - inner);
        take(P);
        end = P->last_end;
    } else {
        n = parse_simple(P);
        if (n && n->type == N_CMD) n->src = adup(P, P->s + start, P->last_end - start);
        return n;
    }
    if (!n) return NULL;
    n->src = adup(P, P->s + start, end - start);
    struct redir **rt = &n->rd;
    while (is_redir(peek(P))) if (!parse_redir(P, &rt)) return NULL;
    return n;
}

static struct node *parse_pipeline(struct P *P) {
    int neg = 0;
    if (is_kw(P, "!")) { take(P); neg = 1; }
    struct node *res = parse_command(P);
    if (!res) return NULL;
    if (peek(P) == TK_PIPE) {
        struct node *p = mk(P, N_PIPE), *tl = res;
        p->a = res;
        while (peek(P) == TK_PIPE) {
            take(P);
            skip_nl(P);
            struct node *c = parse_command(P);
            if (!c) return NULL;
            tl->next = c; tl = c;
        }
        res = p;
    }
    if (neg) { struct node *n = mk(P, N_NOT); n->a = res; res = n; }
    return res;
}

static struct node *parse_andor(struct P *P) {
    struct node *l = parse_pipeline(P);
    while (l && (peek(P) == TK_AND || P->tt == TK_OR)) {
        struct node *n = mk(P, P->tt == TK_AND ? N_AND : N_OR);
        take(P);
        skip_nl(P);
        n->a = l;
        if (!(n->b = parse_pipeline(P))) return NULL;
        l = n;
    }
    return l;
}

static struct node *parse_list(struct P *P) {
    struct node *L = mk(P, N_LIST), **tail = &L->a;
    for (;;) {
        skip_nl(P);
        if (is_term(P)) break;
        int st = P->ts;
        struct node *n = parse_andor(P);
        if (!n) return NULL;
        if (!n->src) n->src = adup(P, P->s + st, P->last_end - st);
        int t = peek(P);
        if (t == TK_BG) {
            struct node *b = mk(P, N_BG);
            take(P);
            b->a = n; b->src = n->src;
            n = b;
        } else if (t == TK_SEMI || t == TK_NL) take(P);
        else if (!is_term(P)) { bad(P); return NULL; }
        *tail = n; tail = &n->next;
    }
    return L;
}

int parse_text(const char *text, struct prog *out) {
    struct P P = { 0 };
    P.s = strdup(text);
    P.noalias = -1;
    g_perr[0] = 0;
    struct node *root = parse_list(&P);
    if (root && !P.inc && !P.err && peek(&P) != TK_EOF) { bad(&P); root = NULL; }
    int rc = P.err ? P_ERROR : P.inc ? P_INCOMPLETE : root ? P_OK : P_ERROR;
    if (rc == P_ERROR && !g_perr[0]) snprintf(g_perr, sizeof g_perr, "syntax error");
    free(P.s);
    out->ar = P.ar; out->root = root;
    if (rc != P_OK) prog_free(out);
    return rc;
}
