/* regex.c - POSIX regex: parser -> AST -> program. Matching is a Pike VM (leftmost-longest overall match,
 * captures by thread priority, linear time); patterns with back-references use a step-bounded backtracker.
 * Self-contained (own ASCII ctype) so the host differential test can build it against glibc. */
#include <regex.h>
#include <stdlib.h>
#include <string.h>

enum { N_EMPTY, N_CHAR, N_ANY, N_SET, N_CAT, N_ALT, N_REP, N_GRP, N_BOL, N_EOL, N_BREF, N_WB, N_NWB, N_WBEG, N_WEND };
enum { I_CHAR, I_ANY, I_SET, I_SPLIT, I_JMP, I_SAVE, I_MATCH, I_BOL, I_EOL, I_BREF, I_WB, I_NWB, I_WBEG, I_WEND };
typedef struct { int t, a, b, min, max; } Node;
typedef struct { int op, x, y; } Inst;
typedef unsigned char uc;
typedef struct { Inst *pr; int n; uc (*sets)[32]; int nsub, icase, nl, hasbref; } Prog;

#define MAXPROG 32768
#define DUPMAX 255
#define STEPS 2000000L

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int lo(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int isw(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int clsn(const char *nm, int n, int c) {
    static const char *names[] = { "alpha", "digit", "alnum", "upper", "lower", "space", "blank", "punct", "print", "graph", "cntrl", "xdigit" };
    int k = 0, al = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'), dg = c >= '0' && c <= '9';
    for (; k < 12; k++) if (strlen(names[k]) == (size_t)n && !memcmp(names[k], nm, n)) break;
    switch (k) {
    case 0: return al;
    case 1: return dg;
    case 2: return al || dg;
    case 3: return c >= 'A' && c <= 'Z';
    case 4: return c >= 'a' && c <= 'z';
    case 5: return c == ' ' || (c >= 9 && c <= 13);
    case 6: return c == ' ' || c == '\t';
    case 7: return c > 32 && c < 127 && !al && !dg;
    case 8: return c >= 32 && c < 127;
    case 9: return c > 32 && c < 127;
    case 10: return c < 32 || c == 127;
    case 11: return dg || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return -1;
}

typedef struct {
    const char *p; int ere, icase, nl, depth, err, nsub, closed;
    Node *nd; int nn, ncap;
    uc (*sets)[32]; int nsets, setcap;
} P;

static int mk(P *p, int t, int a, int b) {
    if (p->nn == p->ncap) {
        Node *x = realloc(p->nd, (p->ncap = p->ncap ? p->ncap * 2 : 64) * sizeof *x);
        if (!x) { p->err = REG_ESPACE; return 0; }
        p->nd = x;
    }
    Node *n = &p->nd[p->nn];
    n->t = t; n->a = a; n->b = b; n->min = n->max = 0;
    return p->nn++;
}
static uc *newset(P *p, int *idx) {
    if (p->nsets == p->setcap) {
        void *x = realloc(p->sets, (p->setcap = p->setcap ? p->setcap * 2 : 8) * 32);
        if (!x) { p->err = REG_ESPACE; *idx = 0; return NULL; }
        p->sets = x;
    }
    *idx = p->nsets;
    memset(p->sets[p->nsets], 0, 32);
    return p->sets[p->nsets++];
}
#define SETB(s, c) ((s)[(c) >> 3] |= 1 << ((c) & 7))
#define HASB(s, c) ((s)[(c) >> 3] >> ((c) & 7) & 1)

static int parse_alt(P *p);
static int at_alt(P *p) { return p->ere ? *p->p == '|' : (p->p[0] == '\\' && p->p[1] == '|'); }
static int at_close(P *p) { return p->ere ? (*p->p == ')' && p->depth > 0) : (p->p[0] == '\\' && p->p[1] == ')'); }

/* [..] after the '[' */
static int bracket(P *p) {
    int idx, neg = 0, first = 1;
    uc *s = newset(p, &idx);
    if (!s) return 0;
    const char *q = p->p;
    if (*q == '^') { neg = 1; q++; }
    for (;; first = 0) {
        if (!*q) { p->err = REG_EBRACK; return 0; }
        if (*q == ']' && !first) { q++; break; }
        int c, lo_ = -1;
        if (q[0] == '[' && (q[1] == ':' || q[1] == '=' || q[1] == '.')) {
            char d = q[1];
            const char *e = q + 2;
            while (*e && !(e[0] == d && e[1] == ']')) e++;
            if (!*e) { p->err = REG_EBRACK; return 0; }
            int n = e - (q + 2);
            if (d == ':') {
                int ok = 0;
                for (int ch = 0; ch < 256; ch++) { int r = clsn(q + 2, n, ch); if (r < 0) { p->err = REG_ECTYPE; return 0; } if (r) { SETB(s, ch); ok = 1; } }
                (void)ok; q = e + 2; continue;
            }
            if (n != 1) { p->err = REG_ECOLLATE; return 0; }
            lo_ = (uc)q[2]; q = e + 2;
            if (d == '=') { SETB(s, lo_); continue; }
        } else lo_ = (uc)*q++;
        c = lo_;
        if (q[0] == '-' && q[1] && q[1] != ']') {
            int hi;
            q++;
            if (q[0] == '[' && q[1] == '.') {
                const char *e = q + 2;
                while (*e && !(e[0] == '.' && e[1] == ']')) e++;
                if (!*e) { p->err = REG_EBRACK; return 0; }
                if (e - (q + 2) != 1) { p->err = REG_ECOLLATE; return 0; }
                hi = (uc)q[2]; q = e + 2;
            } else hi = (uc)*q++;
            if (hi < c) { p->err = REG_ERANGE; return 0; }
            for (int ch = c; ch <= hi; ch++) SETB(s, ch);
        } else SETB(s, c);
    }
    p->p = q;
    if (p->icase) for (int ch = 0; ch < 256; ch++) if (HASB(s, ch)) { SETB(s, lo(ch)); SETB(s, up(ch)); }
    if (neg) { for (int i = 0; i < 32; i++) s[i] = ~s[i]; if (p->nl) s[1] &= ~(1 << 2); }
    return mk(p, N_SET, idx, 0);
}

/* "{m,n}" body after the brace; returns 1 and advances if valid; -1 = syntax error */
static int interval(P *p, int *mn, int *mx) {
    const char *q = p->p;
    int m = -1, n = -1, comma = 0;
    if (*q >= '0' && *q <= '9') { m = 0; while (*q >= '0' && *q <= '9') { m = m * 10 + *q++ - '0'; if (m > 100000) m = 100000; } }
    if (*q == ',') { comma = 1; q++; if (*q >= '0' && *q <= '9') { n = 0; while (*q >= '0' && *q <= '9') { n = n * 10 + *q++ - '0'; if (n > 100000) n = 100000; } } }
    if (p->ere ? *q == '}' : (q[0] == '\\' && q[1] == '}')) q += p->ere ? 1 : 2;
    else { if (!p->ere || m >= 0 || comma) p->err = *q ? REG_BADBR : REG_EBRACE; return -1; }
    if (m < 0 && !comma) return -1;
    if (m < 0) m = 0;
    if (!comma) n = m;
    if ((n >= 0 && m > n) || m > DUPMAX || n > DUPMAX) { p->err = REG_BADBR; return -1; }
    *mn = m; *mx = n; p->p = q;
    return 1;
}

static int wset(P *p, int neg, int sp) {
    int idx;
    uc *s = newset(p, &idx);
    if (!s) return 0;
    for (int c = 0; c < 256; c++) {
        int in = sp ? (c == ' ' || (c >= 9 && c <= 13)) : isw(c);
        if (in != neg) SETB(s, c);
    }
    return mk(p, N_SET, idx, 0);
}

static int atom(P *p, int start) {
    int c = (uc)*p->p;
    if (p->ere) {
        switch (c) {
        case '(': {
            int n = ++p->nsub, in;
            p->p++; p->depth++;
            in = parse_alt(p);
            if (p->err) return 0;
            if (*p->p != ')') { p->err = REG_EPAREN; return 0; }
            p->p++; p->depth--;
            if (n <= 9) p->closed |= 1 << n;
            return mk(p, N_GRP, in, n);
        }
        case '[': p->p++; return bracket(p);
        case '.': p->p++; return mk(p, N_ANY, 0, 0);
        case '^': p->p++; return mk(p, N_BOL, 0, 0);
        case '$': p->p++; return mk(p, N_EOL, 0, 0);
        case '*': case '+': case '?': if (start) { p->err = REG_BADRPT; return 0; } break;
        case '{': { const char *sv = p->p; int a, b; p->p++; if (start && interval(p, &a, &b) > 0) { p->err = REG_BADRPT; return 0; } if (p->err) return 0; p->p = sv; break; }
        }
    } else {
        if (c == '*' && start) { p->p++; return mk(p, N_CHAR, '*', 0); }
        if (c == '[') { p->p++; return bracket(p); }
        if (c == '.') { p->p++; return mk(p, N_ANY, 0, 0); }
        if (c == '^' && start) { p->p++; return mk(p, N_BOL, 0, 0); }
        if (c == '$') {
            p->p++;
            if (!*p->p || at_alt(p) || at_close(p)) return mk(p, N_EOL, 0, 0);
            return mk(p, N_CHAR, '$', 0);
        }
        if (c == '\\' && p->p[1] == '(') {
            int n = ++p->nsub, in;
            p->p += 2; p->depth++;
            in = parse_alt(p);
            if (p->err) return 0;
            if (!(p->p[0] == '\\' && p->p[1] == ')')) { p->err = REG_EPAREN; return 0; }
            p->p += 2; p->depth--;
            if (n <= 9) p->closed |= 1 << n;
            return mk(p, N_GRP, in, n);
        }
        if (c == '\\' && p->p[1] == '{' && start) { p->err = REG_BADRPT; return 0; }
    }
    p->p++;
    if (c == '\\') {
        int e = (uc)*p->p;
        if (!e) { p->err = REG_EESCAPE; return 0; }
        p->p++;
        if (e >= '1' && e <= '9') {
            if (!(p->closed >> (e - '0') & 1)) { p->err = REG_ESUBREG; return 0; }
            return mk(p, N_BREF, e - '0', 0);
        }
        switch (e) {
        case 'w': return wset(p, 0, 0);
        case 'W': return wset(p, 1, 0);
        case 's': return wset(p, 0, 1);
        case 'S': return wset(p, 1, 1);
        case 'b': return mk(p, N_WB, 0, 0);
        case 'B': return mk(p, N_NWB, 0, 0);
        case '<': return mk(p, N_WBEG, 0, 0);
        case '>': return mk(p, N_WEND, 0, 0);
        }
        c = e;
    }
    return mk(p, N_CHAR, c, 0);
}

static int piece(P *p, int start) {
    int a = atom(p, start);
    if (p->err) return 0;
    if (!p->ere && p->nd[a].t == N_BOL) return a;
    for (int nq = 0;; nq++) {
        int mn, mx;
        const char *q = p->p;
        if (*q == '*') { mn = 0; mx = -1; p->p++; }
        else if (p->ere && (*q == '+' || *q == '?')) { mn = *q == '+'; mx = *q == '+' ? -1 : 1; p->p++; }
        else if (!p->ere && q[0] == '\\' && (q[1] == '+' || q[1] == '?')) { mn = q[1] == '+'; mx = q[1] == '+' ? -1 : 1; p->p += 2; }
        else if (p->ere && *q == '{') {
            p->p++;
            if (interval(p, &mn, &mx) < 0) { if (p->err) return 0; p->p = q; break; }
        } else if (!p->ere && q[0] == '\\' && q[1] == '{') {
            p->p += 2;
            if (interval(p, &mn, &mx) < 0) { if (!p->err) p->err = *p->p ? REG_BADBR : REG_EBRACE; return 0; }
        } else break;
        if ((!p->ere && nq) || (p->ere && p->nd[a].t == N_BOL)) { p->err = REG_BADRPT; return 0; }   /* glibc: a** and ^* are errors */
        a = mk(p, N_REP, a, 0);
        if (p->err) return 0;
        p->nd[a].min = mn; p->nd[a].max = mx;
    }
    return a;
}

static int parse_cat(P *p) {
    int l = 0, start = 1;
    while (!p->err && *p->p && !at_alt(p) && !at_close(p)) {
        int a = piece(p, start);
        if (p->err) return 0;
        start = !p->ere && p->nd[a].t == N_BOL;
        l = l ? mk(p, N_CAT, l, a) : a;
    }
    return l;
}

static int parse_alt(P *p) {
    int l = parse_cat(p);
    while (!p->err && at_alt(p)) {
        p->p += p->ere ? 1 : 2;
        int r = parse_cat(p);
        l = mk(p, N_ALT, l, r);
    }
    return l;
}

typedef struct { Inst *pr; int n, cap, err, icase, bref; Node *nd; } G;
static int emit(G *g, int op, int x, int y) {
    if (g->n == g->cap) {
        if (g->n >= MAXPROG) { g->err = REG_ESPACE; return 0; }
        Inst *t = realloc(g->pr, (g->cap = g->cap ? g->cap * 2 : 64) * sizeof *t);
        if (!t) { g->err = REG_ESPACE; return 0; }
        g->pr = t;
    }
    g->pr[g->n].op = op; g->pr[g->n].x = x; g->pr[g->n].y = y;
    return g->n++;
}
static void gen(G *g, int n) {
    if (g->err) return;
    Node *nn = &g->nd[n];
    switch (nn->t) {
    case N_EMPTY: break;
    case N_CHAR: emit(g, I_CHAR, g->icase ? lo(nn->a) : nn->a, 0); break;
    case N_ANY: emit(g, I_ANY, 0, 0); break;
    case N_SET: emit(g, I_SET, nn->a, 0); break;
    case N_CAT: gen(g, nn->a); gen(g, nn->b); break;
    case N_ALT: {
        int s = emit(g, I_SPLIT, 0, 0), j;
        if (g->err) return;
        g->pr[s].x = s + 1;
        gen(g, nn->a);
        j = emit(g, I_JMP, 0, 0);
        if (g->err) return;
        g->pr[s].y = g->n;
        gen(g, nn->b);
        g->pr[j].x = g->n;
        break;
    }
    case N_GRP: emit(g, I_SAVE, 2 * nn->b, 0); gen(g, nn->a); emit(g, I_SAVE, 2 * nn->b + 1, 0); break;
    case N_BOL: emit(g, I_BOL, 0, 0); break;
    case N_EOL: emit(g, I_EOL, 0, 0); break;
    case N_BREF: g->bref = 1; emit(g, I_BREF, nn->a, 0); break;
    case N_WB: emit(g, I_WB, 0, 0); break;
    case N_NWB: emit(g, I_NWB, 0, 0); break;
    case N_WBEG: emit(g, I_WBEG, 0, 0); break;
    case N_WEND: emit(g, I_WEND, 0, 0); break;
    case N_REP: {
        int mn = nn->min, mx = nn->max, a = nn->a, i;
        if (mx < 0 && mn >= 1) {                 /* x{m,} = x^(m-1) x+ */
            for (i = 1; i < mn; i++) gen(g, a);
            int top = g->n, s;
            gen(g, a);
            s = emit(g, I_SPLIT, top, 0);
            if (!g->err) g->pr[s].y = g->n;
        } else {
            for (i = 0; i < mn; i++) gen(g, a);
            if (mx < 0) {                        /* x* = (x+)?, so an empty iteration still sets captures */
                int s = emit(g, I_SPLIT, 0, 0), e;
                if (g->err) return;
                g->pr[s].x = s + 1;
                gen(g, a);
                e = emit(g, I_SPLIT, s + 1, 0);
                if (!g->err) g->pr[s].y = g->pr[e].y = g->n;
            } else {
                int fix[DUPMAX + 1], k = mx - mn;
                for (i = 0; i < k; i++) {
                    fix[i] = emit(g, I_SPLIT, 0, 0);
                    if (g->err) return;
                    g->pr[fix[i]].x = fix[i] + 1;
                    gen(g, a);
                }
                for (i = 0; i < k && !g->err; i++) g->pr[fix[i]].y = g->n;
            }
        }
        break;
    }
    }
}

int regcomp(regex_t *re, const char *pat, int cflags) {
    P p;
    memset(&p, 0, sizeof p);
    p.p = pat; p.ere = cflags & REG_EXTENDED; p.icase = cflags & REG_ICASE; p.nl = cflags & REG_NEWLINE;
    mk(&p, N_EMPTY, 0, 0);
    int root = parse_alt(&p), err = p.err;
    if (!err && *p.p) err = REG_EPAREN;       /* leftover is an unmatched \) */
    G g = { 0 };
    g.icase = p.icase; g.nd = p.nd;
    if (!err) {
        emit(&g, I_SAVE, 0, 0);
        gen(&g, root);
        emit(&g, I_SAVE, 1, 0);
        emit(&g, I_MATCH, 0, 0);
        err = g.err;
    }
    Prog *pg = err ? NULL : malloc(sizeof *pg);
    if (!err && !pg) err = REG_ESPACE;
    free(p.nd);
    if (err) { free(p.sets); free(g.pr); return err; }
    pg->pr = g.pr; pg->n = g.n; pg->sets = p.sets; pg->nsub = p.nsub;
    pg->icase = p.icase != 0; pg->nl = p.nl != 0; pg->hasbref = g.bref;
    re->re_nsub = p.nsub; re->re_prog = pg;
    return 0;
}

void regfree(regex_t *re) {
    Prog *pg = re->re_prog;
    if (pg) { free(pg->pr); free(pg->sets); free(pg); }
    re->re_prog = NULL;
}

size_t regerror(int err, const regex_t *re, char *buf, size_t size) {
    static const char *msg[] = { "Success", "No match", "Invalid regular expression", "Invalid collation character",
        "Invalid character class name", "Trailing backslash", "Invalid back reference", "Unmatched [, [^, [:, [., or [=",
        "Unmatched ( or \\(", "Unmatched \\{", "Invalid content of \\{\\}", "Invalid range end", "Memory exhausted",
        "Invalid preceding regular expression" };
    const char *m = err >= 0 && err < (int)(sizeof msg / sizeof *msg) ? msg[err] : "Unknown error";
    size_t n = strlen(m) + 1;
    if (size) { size_t k = n < size ? n : size - 1; memcpy(buf, m, k); buf[k] = 0; }
    return n;
}

typedef struct {
    const Prog *pg; const char *s; int start, end, ncap, notbol, noteol;
    int *mark;
} M;

static int m_bol(const M *m, int sp) {
    if (sp == 0) return !m->notbol;
    return m->pg->nl && m->s[sp - 1] == '\n';
}
static int m_eol(const M *m, int sp) {
    if (sp == m->end) return !m->noteol;
    return m->pg->nl && m->s[sp] == '\n';
}
/* zero-width assertion test; 1 = holds */
static int m_assert(const M *m, int op, int sp) {
    int b, a;
    switch (op) {
    case I_BOL: return m_bol(m, sp);
    case I_EOL: return m_eol(m, sp);
    }
    b = sp > 0 && isw((uc)m->s[sp - 1]);
    a = sp < m->end && isw((uc)m->s[sp]);
    switch (op) {
    case I_WB: return b != a;
    case I_NWB: return b == a;
    case I_WBEG: return !b && a;
    default: return b && !a;
    }
}
static int m_chr(const M *m, const Inst *i, int c) {
    switch (i->op) {
    case I_CHAR: return (m->pg->icase ? lo(c) : c) == i->x;
    case I_ANY: return !(m->pg->nl && c == '\n');
    default: return HASB(m->pg->sets[i->x], c);
    }
}

typedef struct { int n; int *pc, *cap; } L;

static void add(M *m, L *l, int pc, int sp, const int *cap, int gen) {
    for (;;) {
        if (m->mark[pc] == gen) return;
        m->mark[pc] = gen;
        const Inst *i = &m->pg->pr[pc];
        switch (i->op) {
        case I_JMP: pc = i->x; continue;
        case I_SPLIT: add(m, l, i->x, sp, cap, gen); pc = i->y; continue;
        case I_SAVE:
            if (i->x < m->ncap) {
                int c2[m->ncap];
                memcpy(c2, cap, sizeof c2);
                c2[i->x] = sp;
                add(m, l, pc + 1, sp, c2, gen);
                return;
            }
            pc++; continue;
        case I_BOL: case I_EOL: case I_WB: case I_NWB: case I_WBEG: case I_WEND:
            if (!m_assert(m, i->op, sp)) return;
            pc++; continue;
        default:
            l->pc[l->n] = pc;
            memcpy(l->cap + l->n * m->ncap, cap, m->ncap * sizeof(int));
            l->n++;
            return;
        }
    }
}

/* Pike VM; fills best[0..ncap) with the leftmost-longest match; 1 = matched */
static int pike(M *m, int *best, int boolmode) {
    int np = m->pg->n, nc = m->ncap, bs = -1, be = -1, sp, i;
    int *blk = malloc((2 * np * (1 + nc) + np) * sizeof(int));
    if (!blk) return -1;
    L cl = { 0, blk, blk + np }, nl = { 0, blk + np * (1 + nc), blk + np * (2 + nc) };
    m->mark = blk + 2 * np * (1 + nc);
    for (i = 0; i < np; i++) m->mark[i] = 0;
    int init[nc];
    for (i = 0; i < nc; i++) init[i] = -1;
    for (sp = m->start;; sp++) {
        if (bs < 0) add(m, &cl, 0, sp, init, sp + 1);
        if (!cl.n) { if (bs >= 0 || sp >= m->end) break; continue; }
        nl.n = 0;
        for (i = 0; i < cl.n; i++) {
            int *cap = cl.cap + i * nc;
            const Inst *in = &m->pg->pr[cl.pc[i]];
            if (bs >= 0 && cap[0] > bs) continue;
            if (in->op == I_MATCH) {
                if (bs < 0 || cap[0] < bs || (cap[0] == bs && sp > be)) { bs = cap[0]; be = sp; memcpy(best, cap, nc * sizeof(int)); }
                if (boolmode) goto done;
            } else if (sp < m->end && m_chr(m, in, (uc)m->s[sp])) add(m, &nl, cl.pc[i] + 1, sp + 1, cap, sp + 2);
        }
        L t = cl; cl = nl; nl = t;
        if (sp >= m->end) break;
    }
done:
    free(blk);
    return bs >= 0;
}

typedef struct { M *m; int *caps, *best; int be, ncap; long steps; } B;
static void bt(B *b, int pc, int sp, int depth) {
    M *m = b->m;
    if (depth > 3000) return;
    for (;;) {
        if (++b->steps > STEPS) return;
        const Inst *i = &m->pg->pr[pc];
        switch (i->op) {
        case I_CHAR: case I_ANY: case I_SET:
            if (sp < m->end && m_chr(m, i, (uc)m->s[sp])) { pc++; sp++; continue; }
            return;
        case I_JMP: pc = i->x; continue;
        case I_SPLIT: bt(b, i->x, sp, depth + 1); pc = i->y; continue;
        case I_SAVE: {
            int old = b->caps[i->x];
            b->caps[i->x] = sp;
            bt(b, pc + 1, sp, depth + 1);
            b->caps[i->x] = old;
            return;
        }
        case I_BREF: {
            int so = b->caps[2 * i->x], eo = b->caps[2 * i->x + 1], n = eo - so, k;
            if (so < 0 || eo < 0 || sp + n > m->end) return;
            for (k = 0; k < n; k++) {
                int x = (uc)m->s[so + k], y = (uc)m->s[sp + k];
                if (m->pg->icase ? lo(x) != lo(y) : x != y) return;
            }
            sp += n; pc++; continue;
        }
        case I_MATCH:
            if (sp > b->be) { b->be = sp; memcpy(b->best, b->caps, b->ncap * sizeof(int)); }
            return;
        default:
            if (!m_assert(m, i->op, sp)) return;
            pc++; continue;
        }
    }
}

static int backtrack(M *m, int *best) {
    int nc = m->ncap, caps[nc], st, k;
    B b = { m, caps, best, -1, nc, 0 };
    for (st = m->start; st <= m->end && b.steps <= STEPS; st++) {
        for (k = 0; k < nc; k++) caps[k] = -1;
        bt(&b, 0, st, 0);
        if (b.be >= 0) return 1;
    }
    return 0;
}

int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t pm[], int eflags) {
    const Prog *pg = re->re_prog;
    M m = { pg, s, 0, 0, 0, eflags & REG_NOTBOL, eflags & REG_NOTEOL, NULL };
    int nsub = pg->nsub;
    if (eflags & REG_STARTEND) { m.start = pm[0].rm_so; m.end = pm[0].rm_eo; } else m.end = strlen(s);
    int boolmode = nmatch == 0 || pm == NULL;
    m.ncap = pg->hasbref ? 2 * (nsub + 1) : 2 * (1 + (nmatch > 0 && (int)nmatch - 1 < nsub ? (int)nmatch - 1 : nsub));
    if (boolmode && !pg->hasbref) m.ncap = 2;
    int best[m.ncap], r;
    r = pg->hasbref ? backtrack(&m, best) : pike(&m, best, boolmode);
    if (r < 0) return REG_ESPACE;
    if (!r) return REG_NOMATCH;
    for (size_t i = 0; i < nmatch && pm; i++)
        if ((int)i * 2 + 1 < m.ncap) { pm[i].rm_so = best[2 * i]; pm[i].rm_eo = best[2 * i + 1]; if (pm[i].rm_eo < 0) pm[i].rm_so = -1; }
        else pm[i].rm_so = pm[i].rm_eo = -1;
    return 0;
}
