/* awk [-F fs] [-v var=val]... 'program' | -f progfile [file | var=val ...]
 * A POSIX-awk subset: patterns (BEGIN END expr /re/ a,b), $0 $n NF NR FNR FS OFS ORS RS SUBSEP RSTART RLENGTH FILENAME
 * CONVFMT OFMT ENVIRON ARGV ARGC, print/printf (> >> redirection), if/while/do/for/for-in, break continue next exit
 * return delete, user functions (arrays by reference), assoc arrays, getline [var] [< file], regex via libc ERE, and
 * length substr index split sub gsub match sprintf sin cos atan2 exp log sqrt int rand srand tolower toupper system
 * close fflush; `cmd | getline` and `print | cmd` run `sh -c` through spawn (the whole command output is read at once).
 * Bytes, not UTF-8 characters. */
#include <vutil.h>
#include <regex.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>

/* ---------------------------------------------------------------- values */
enum { F_NUM = 1, F_STR = 2, F_STRNUM = 4 };
typedef struct { unsigned f; double n; char *s; } Val;
static char EMPTY[] = "";

typedef struct Arr Arr;
typedef struct Cell { int kind; Val v; Arr *arr; struct Cell *ref; int own; } Cell;   /* kind 0 uninit, 1 array, 2 scalar */
typedef struct Ent { char *key; Cell c; struct Ent *next, *prev, *chain; } Ent;
struct Arr { Ent **tab; int size, count; Ent *first, *last; };

static void fatal(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fputs("awk: ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

/* temp string pool, released in stack order at statement boundaries */
static char **pool;
static int ptop, pcap;
static char *tmp(char *s) {
    if (ptop == pcap) pool = realloc(pool, (pcap = pcap ? pcap * 2 : 256) * sizeof *pool);
    pool[ptop++] = s;
    return s;
}
static void release(int m) { while (ptop > m) free(pool[--ptop]); }

static char *dupn(const char *s, size_t n) { char *r = malloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }

static Val vnum(double n) { Val v = { F_NUM, n, NULL }; return v; }
static Val vstr(char *s) { Val v = { F_STR, 0, s }; return v; }
static Val vtmp(char *s) { return vstr(tmp(s)); }
static Val uninit(void) { Val v = { F_NUM | F_STR | F_STRNUM, 0, EMPTY }; return v; }

static char *convfmt_s = "%.6g", *ofmt_s = "%.6g";

static char *numstr(double n, const char *fmt) {
    char b[64];
    if (n == (double)(long long)n && fabs(n) < 1e16) snprintf(b, sizeof b, "%lld", (long long)n);
    else if (isnan(n)) strcpy(b, n < 0 ? "-nan" : "nan");
    else if (isinf(n)) strcpy(b, n < 0 ? "-inf" : "inf");
    else snprintf(b, sizeof b, fmt, n);
    return strdup(b);
}

static int looksnum(const char *s, double *out) {
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    const char *p = s + (*s == '+' || *s == '-');
    if (!(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1])))) return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return 0;
    double d = strtod(s, &e);
    while (*e == ' ' || *e == '\t' || *e == '\n') e++;
    if (*e) return 0;
    if (out) *out = d;
    return 1;
}
static double atonum(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    const char *p = s + (*s == '+' || *s == '-');
    if (!(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1])))) return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) return 0;
    return strtod(s, NULL);
}

static double tonum(Val v) { return v.f & F_NUM ? v.n : atonum(v.s); }
static char *tostr_f(Val v, const char *fmt) { return v.f & F_STR ? v.s : tmp(numstr(v.n, fmt)); }
static char *tostr(Val v) { return tostr_f(v, convfmt_s); }
static int truth(Val v) {
    if ((v.f & F_NUM) && !(v.f & F_STR)) return v.n != 0;
    if ((v.f & F_STRNUM) && (v.f & F_NUM)) return v.n != 0;          /* uninitialised */
    if (v.f & F_STRNUM) { double d; if (looksnum(v.s, &d)) return d != 0; }
    return *v.s != 0;
}
static int isnumeric(Val v) { return (v.f & F_NUM) || (v.f & F_STRNUM); }
static int compare(Val a, Val b) {
    if (isnumeric(a) && isnumeric(b)) {
        double x = (a.f & F_NUM) ? a.n : atonum(a.s), y = (b.f & F_NUM) ? b.n : atonum(b.s);
        return x < y ? -1 : x > y;
    }
    int r = strcmp(tostr(a), tostr(b));
    return r < 0 ? -1 : r > 0;
}

static void setcell(Cell *c, Val v) {
    if (c->kind == 1) fatal("can't assign to array name");
    char *old = (c->v.f & F_STR) && c->v.s != EMPTY ? c->v.s : NULL;
    if (v.f & F_STR) { if (v.s == EMPTY) c->v.s = EMPTY; else c->v.s = strdup(v.s); } else c->v.s = EMPTY;
    c->v.f = v.f; c->v.n = v.n;
    if (!(v.f & F_STR)) c->v.f = v.f & ~F_STR;
    free(old);
    c->kind = 2;
}
static Val cellval(Cell *c) {
    if (c->kind == 1) fatal("can't read value of array");
    if (c->kind == 0) return uninit();
    Val v = c->v;
    if (!(v.f & F_STR)) v.s = NULL;
    return v;
}

/* arrays */
static unsigned hashs(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
static Arr *newarr(void) { Arr *a = calloc(1, sizeof *a); a->size = 16; a->tab = calloc(a->size, sizeof *a->tab); return a; }
static Ent *arr_find(Arr *a, const char *k) {
    for (Ent *e = a->tab[hashs(k) % a->size]; e; e = e->chain) if (!strcmp(e->key, k)) return e;
    return NULL;
}
static Ent *arr_get(Arr *a, const char *k) {
    Ent *e = arr_find(a, k);
    if (e) return e;
    if (a->count >= a->size * 2) {
        int ns = a->size * 4;
        Ent **nt = calloc(ns, sizeof *nt);
        for (Ent *x = a->first; x; x = x->next) { unsigned h = hashs(x->key) % ns; x->chain = nt[h]; nt[h] = x; }
        free(a->tab); a->tab = nt; a->size = ns;
    }
    e = calloc(1, sizeof *e);
    e->key = strdup(k);
    unsigned h = hashs(k) % a->size;
    e->chain = a->tab[h]; a->tab[h] = e;
    e->prev = a->last; if (a->last) a->last->next = e; else a->first = e;
    a->last = e;
    a->count++;
    return e;
}
static void cell_clear(Cell *c) {
    if ((c->v.f & F_STR) && c->v.s != EMPTY && c->kind == 2) free(c->v.s);
    if (c->kind == 1 && c->arr && c->own) { /* freed by arr_free */ }
}
static void arr_free(Arr *a);
static void arr_del(Arr *a, const char *k) {
    unsigned h = hashs(k) % a->size;
    for (Ent **p = &a->tab[h]; *p; p = &(*p)->chain)
        if (!strcmp((*p)->key, k)) {
            Ent *e = *p;
            *p = e->chain;
            if (e->prev) e->prev->next = e->next; else a->first = e->next;
            if (e->next) e->next->prev = e->prev; else a->last = e->prev;
            cell_clear(&e->c); free(e->key); free(e);
            a->count--;
            return;
        }
}
static void arr_clear(Arr *a) { while (a->first) arr_del(a, a->first->key); }
static void arr_free(Arr *a) { arr_clear(a); free(a->tab); free(a); }

/* ---------------------------------------------------------------- lexer */
enum { T_EOF, T_NL, T_NUM, T_STR, T_ERE, T_NAME, T_FUNCNAME, T_BUILTIN,
       T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE, T_NEXT, T_EXIT, T_RETURN,
       T_DELETE, T_IN, T_GETLINE, T_PRINT, T_PRINTF,
       T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA,
       T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET, T_NOT, T_GT, T_LT, T_PIPE, T_QUESTION, T_COLON, T_TILDE, T_DOLLAR,
       T_ASSIGN, T_INCR, T_DECR, T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_MODEQ, T_POWEQ, T_EQ, T_LE, T_GE, T_NE, T_NOMATCH,
       T_AND, T_OR, T_APPEND };
typedef struct { int t; char *s; double n; int id; } Tok;
static Tok *toks;
static int ntok, ptok;

enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT,
       B_INT, B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH };
static const char *bnames[] = { "length", "substr", "index", "split", "sub", "gsub", "match", "sprintf", "sin", "cos", "atan2",
    "exp", "log", "sqrt", "int", "rand", "srand", "tolower", "toupper", "system", "close", "fflush" };
static const struct { const char *s; int t; } kws[] = { { "BEGIN", T_BEGIN }, { "END", T_END }, { "function", T_FUNCTION },
    { "func", T_FUNCTION }, { "if", T_IF }, { "else", T_ELSE }, { "while", T_WHILE }, { "for", T_FOR }, { "do", T_DO },
    { "break", T_BREAK }, { "continue", T_CONTINUE }, { "next", T_NEXT }, { "exit", T_EXIT }, { "return", T_RETURN },
    { "delete", T_DELETE }, { "in", T_IN }, { "getline", T_GETLINE }, { "print", T_PRINT }, { "printf", T_PRINTF } };

static void addtok(int t, char *s, double n, int id) {
    toks = realloc(toks, (ntok + 1) * sizeof *toks);
    toks[ntok].t = t; toks[ntok].s = s; toks[ntok].n = n; toks[ntok].id = id;
    ntok++;
}

static int regex_ok(int prev) {            /* can a '/' here start a regex? */
    switch (prev) {
    case T_NUM: case T_STR: case T_ERE: case T_NAME: case T_RPAREN: case T_RBRACKET: case T_DOLLAR: case T_INCR: case T_DECR: case T_BUILTIN:
        return 0;
    }
    return 1;
}

static int escape(const char **pp, char *out) {          /* after the backslash; 1 = produced *out */
    const char *p = *pp;
    int c = *p++;
    switch (c) {
    case 'n': *out = '\n'; break;
    case 't': *out = '\t'; break;
    case 'r': *out = '\r'; break;
    case '\\': *out = '\\'; break;
    case '"': *out = '"'; break;
    case '/': *out = '/'; break;
    case 'a': *out = 7; break;
    case 'b': *out = 8; break;
    case 'f': *out = 12; break;
    case 'v': *out = 11; break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = c - '0', n = 1;
        while (n < 3 && *p >= '0' && *p <= '7') { v = v * 8 + *p++ - '0'; n++; }
        *out = v;
        break;
    }
    default: *pp = p - 1; return 0;                      /* keep "\c" as is (regex escapes) */
    }
    *pp = p;
    return 1;
}

static void lex(const char *p) {
    int prev = T_NL;
    while (*p) {
        int t = -1;
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
        if (*p == '\\' && p[1] == '\r' && p[2] == '\n') { p += 3; continue; }
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (*p == '\n') { addtok(T_NL, NULL, 0, 0); prev = T_NL; p++; continue; }
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            char *e;
            double d = strtod(p, &e);
            if ((p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) { d = strtol(p, &e, 16); }
            addtok(T_NUM, NULL, d, 0);
            p = e; prev = T_NUM; continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            size_t n = p - s;
            int k, found = 0;
            for (k = 0; k < (int)(sizeof kws / sizeof *kws); k++)
                if (strlen(kws[k].s) == n && !memcmp(kws[k].s, s, n)) { addtok(kws[k].t, NULL, 0, 0); prev = kws[k].t; found = 1; break; }
            if (found) continue;
            for (k = 0; k < (int)(sizeof bnames / sizeof *bnames); k++)
                if (strlen(bnames[k]) == n && !memcmp(bnames[k], s, n)) { addtok(T_BUILTIN, NULL, 0, k); prev = T_BUILTIN; found = 1; break; }
            if (found) continue;
            addtok(*p == '(' ? T_FUNCNAME : T_NAME, dupn(s, n), 0, 0);
            prev = T_NAME;
            continue;
        }
        if (*p == '"') {
            size_t cap = 32, n = 0;
            char *b = malloc(cap);
            p++;
            while (*p && *p != '"') {
                if (n + 4 >= cap) b = realloc(b, cap *= 2);
                if (*p == '\\') {
                    p++;
                    if (*p == '\n') { p++; continue; }
                    char c;
                    if (escape(&p, &c)) b[n++] = c; else { b[n++] = '\\'; b[n++] = *p++; }
                } else if (*p == '\n') fatal("newline in string");
                else b[n++] = *p++;
            }
            if (!*p) fatal("non-terminated string");
            p++;
            b[n] = 0;
            addtok(T_STR, b, 0, 0);
            prev = T_STR; continue;
        }
        if (*p == '/' && regex_ok(prev)) {
            size_t cap = 32, n = 0;
            char *b = malloc(cap);
            int inb = 0;
            p++;
            while (*p && (*p != '/' || inb)) {
                if (n + 3 >= cap) b = realloc(b, cap *= 2);
                if (*p == '\n') fatal("newline in regex");
                if (*p == '\\' && p[1] == '/') { b[n++] = '/'; p += 2; continue; }
                if (*p == '\\' && p[1]) { b[n++] = *p++; }
                else if (*p == '[' && !inb) { inb = 1; b[n++] = *p++; if (*p == '^') b[n++] = *p++; if (*p == ']') b[n++] = *p++; continue; }
                else if (*p == ']' && inb) inb = 0;
                b[n++] = *p++;
            }
            if (!*p) fatal("non-terminated regular expression");
            p++;
            b[n] = 0;
            addtok(T_ERE, b, 0, 0);
            prev = T_ERE; continue;
        }
        static const struct { const char *s; int t; } ops[] = {
            { "&&", T_AND }, { "||", T_OR }, { "++", T_INCR }, { "--", T_DECR }, { "+=", T_ADDEQ }, { "-=", T_SUBEQ },
            { "*=", T_MULEQ }, { "/=", T_DIVEQ }, { "%=", T_MODEQ }, { "^=", T_POWEQ }, { "**=", T_POWEQ }, { "==", T_EQ },
            { "<=", T_LE }, { ">=", T_GE }, { "!=", T_NE }, { "!~", T_NOMATCH }, { ">>", T_APPEND }, { "**", T_CARET },
            { "{", T_LBRACE }, { "}", T_RBRACE }, { "(", T_LPAREN }, { ")", T_RPAREN }, { "[", T_LBRACKET }, { "]", T_RBRACKET },
            { ";", T_SEMI }, { ",", T_COMMA }, { "+", T_PLUS }, { "-", T_MINUS }, { "*", T_STAR }, { "/", T_SLASH },
            { "%", T_PERCENT }, { "^", T_CARET }, { "!", T_NOT }, { ">", T_GT }, { "<", T_LT }, { "|", T_PIPE },
            { "?", T_QUESTION }, { ":", T_COLON }, { "~", T_TILDE }, { "$", T_DOLLAR }, { "=", T_ASSIGN } };
        int best = -1;
        size_t bl = 0;
        for (int k = 0; k < (int)(sizeof ops / sizeof *ops); k++) {
            size_t l = strlen(ops[k].s);
            if (l > bl && !strncmp(p, ops[k].s, l)) { best = k; bl = l; }
        }
        if (best < 0) fatal("syntax error at '%c'", *p);
        t = ops[best].t;
        addtok(t, NULL, 0, 0);
        prev = t;
        p += bl;
        if (t == T_LBRACE || t == T_SEMI) {}
    }
    addtok(T_NL, NULL, 0, 0);
    addtok(T_EOF, NULL, 0, 0);
}

/* ---------------------------------------------------------------- AST */
enum { N_NUM, N_STR, N_ERE, N_VAR, N_LOCAL, N_FIELD, N_INDEX, N_ASSIGN, N_COND, N_AND, N_OR, N_NOT, N_NEG, N_PLUS, N_CMP, N_MATCH,
       N_CAT, N_ARITH, N_PREINC, N_POSTINC, N_IN, N_CALL, N_BUILTIN, N_GETLINE, N_GROUP,
       S_BLOCK, S_EXPR, S_PRINT, S_PRINTF, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BREAK, S_CONT, S_NEXT, S_EXIT, S_RETURN, S_DELETE };
typedef struct Node {
    int t, op, idx;
    struct Node *a, *b, *c, *d, *next;
    double num;
    char *str;
    regex_t *re;
} Node;

static Node *mk(int t, Node *a, Node *b) { Node *n = calloc(1, sizeof *n); n->t = t; n->a = a; n->b = b; return n; }

/* global variable table */
enum { V_NF, V_NR, V_FNR, V_FS, V_OFS, V_ORS, V_RS, V_SUBSEP, V_RSTART, V_RLENGTH, V_FILENAME, V_CONVFMT, V_OFMT, V_ENVIRON, V_ARGC, V_ARGV, V_NSPECIAL };
static const char *vspecial[] = { "NF", "NR", "FNR", "FS", "OFS", "ORS", "RS", "SUBSEP", "RSTART", "RLENGTH", "FILENAME", "CONVFMT", "OFMT", "ENVIRON", "ARGC", "ARGV" };
static char **gnames;
static Cell *globals;
static int nglob;

static int gvar(const char *name) {
    for (int i = 0; i < nglob; i++) if (!strcmp(gnames[i], name)) return i;
    gnames = realloc(gnames, (nglob + 1) * sizeof *gnames);
    globals = realloc(globals, (nglob + 1) * sizeof *globals);
    gnames[nglob] = strdup(name);
    memset(&globals[nglob], 0, sizeof *globals);
    return nglob++;
}

typedef struct { char *name; int nparams; char **params; Node *body; } Func;
static Func *funcs;
static int nfuncs;
static Func *curfunc;

typedef struct Item { Node *pat, *pat2; Node *action; int inrange; struct Item *next; } Item;
static Item *begins, *ends, *items, **itemtail = &items, **begtail = &begins, **endtail = &ends;

static Tok *tk(void) { return &toks[ptok]; }
static int tt(void) { return toks[ptok].t; }
static void adv(void) { if (toks[ptok].t != T_EOF) ptok++; }
static void syntax(const char *what) { fatal("syntax error: %s", what); }
static void expect(int t, const char *what) { if (tt() != t) syntax(what); adv(); }
static void skip_nl(void) { while (tt() == T_NL) adv(); }
static void skip_term(void) { while (tt() == T_NL || tt() == T_SEMI) adv(); }

static Node *expr(void);
static Node *ternary(void);
static Node *stmt(void);
static int noin, nogt;

static int findfunc(const char *name) {
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) return i;
    funcs = realloc(funcs, (nfuncs + 1) * sizeof *funcs);
    memset(&funcs[nfuncs], 0, sizeof *funcs);
    funcs[nfuncs].name = strdup(name);
    funcs[nfuncs].nparams = -1;
    return nfuncs++;
}

static Node *varref(const char *name) {
    if (curfunc)
        for (int i = 0; i < curfunc->nparams; i++)
            if (!strcmp(curfunc->params[i], name)) { Node *n = mk(N_LOCAL, NULL, NULL); n->idx = i; return n; }
    Node *n = mk(N_VAR, NULL, NULL);
    n->idx = gvar(name);
    return n;
}

static Node *exprlist(int closer) {           /* comma separated, linked by ->next */
    Node *head = NULL, **tail = &head;
    int sn = nogt, si = noin;
    nogt = 0; noin = 0;
    skip_nl();
    if (tt() != closer) for (;;) {
        *tail = expr();
        tail = &(*tail)->next;
        skip_nl();
        if (tt() != T_COMMA) break;
        adv(); skip_nl();
    }
    nogt = sn; noin = si;
    return head;
}

static int starts_primary(int t) {
    switch (t) {
    case T_NUM: case T_STR: case T_ERE: case T_NAME: case T_FUNCNAME: case T_BUILTIN: case T_DOLLAR: case T_NOT: case T_LPAREN:
    case T_MINUS: case T_PLUS: case T_INCR: case T_DECR:
        return 1;
    }
    return 0;
}

static Node *lvalue_check(Node *n) {
    if (n->t != N_VAR && n->t != N_LOCAL && n->t != N_FIELD && n->t != N_INDEX) syntax("not an lvalue");
    return n;
}

static Node *primary(void) {
    Tok *k = tk();
    Node *n;
    switch (k->t) {
    case T_NUM: n = mk(N_NUM, NULL, NULL); n->num = k->n; adv(); return n;
    case T_STR: n = mk(N_STR, NULL, NULL); n->str = k->s; adv(); return n;
    case T_ERE: n = mk(N_ERE, NULL, NULL); n->str = k->s; adv(); return n;
    case T_DOLLAR: {
        adv();
        if (tt() == T_INCR || tt() == T_DECR) { int d = tt() == T_INCR ? 1 : -1; adv(); Node *lv = lvalue_check(primary()); n = mk(N_PREINC, lv, NULL); n->op = d; return mk(N_FIELD, n, NULL); }
        if (tt() == T_MINUS) { adv(); n = mk(N_NEG, primary(), NULL); return mk(N_FIELD, n, NULL); }
        n = primary();
        return mk(N_FIELD, n, NULL);
    }
    case T_INCR: case T_DECR: {
        int d = k->t == T_INCR ? 1 : -1;
        adv();
        Node *lv = lvalue_check(primary());
        n = mk(N_PREINC, lv, NULL); n->op = d;
        return n;
    }
    case T_NOT: adv(); return mk(N_NOT, primary(), NULL);
    case T_MINUS: adv(); return mk(N_NEG, primary(), NULL);
    case T_PLUS: adv(); return mk(N_PLUS, primary(), NULL);
    case T_LPAREN: {
        adv();
        Node *list = exprlist(T_RPAREN);
        expect(T_RPAREN, "expected )");
        if (list && list->next) {                       /* (i,j) in arr */
            if (tt() != T_IN) syntax("expected 'in' after (list)");
            adv();
            if (tt() != T_NAME) syntax("array name expected");
            n = mk(N_IN, list, varref(tk()->s)); adv();
            return n;
        }
        if (!list) syntax("empty ()");
        return mk(N_GROUP, list, NULL);
    }
    case T_NAME: {
        char *name = k->s;
        adv();
        if (tt() == T_LBRACKET) {
            adv();
            Node *subs = exprlist(T_RBRACKET);
            expect(T_RBRACKET, "expected ]");
            if (!subs) syntax("empty subscript");
            return mk(N_INDEX, varref(name), subs);
        }
        return varref(name);
    }
    case T_FUNCNAME: {
        char *name = k->s;
        adv(); adv();                                     /* name, ( */
        Node *args = exprlist(T_RPAREN);
        expect(T_RPAREN, "expected ) after call");
        n = mk(N_CALL, args, NULL);
        n->idx = findfunc(name);
        return n;
    }
    case T_BUILTIN: {
        int id = k->id;
        adv();
        n = mk(N_BUILTIN, NULL, NULL);
        n->idx = id;
        if (tt() == T_LPAREN) {
            adv();
            n->a = exprlist(T_RPAREN);
            expect(T_RPAREN, "expected ) after builtin arguments");
        } else if (id != B_LENGTH) syntax("builtin needs arguments");
        return n;
    }
    case T_GETLINE: {
        adv();
        n = mk(N_GETLINE, NULL, NULL);
        if (tt() == T_NAME || tt() == T_DOLLAR) {
            Node *lv = primary();
            n->a = lvalue_check(lv->t == N_GROUP ? lv->a : lv);
        }
        if (tt() == T_LT) { adv(); n->b = primary(); }
        return n;
    }
    }
    syntax("unexpected token");
    return NULL;
}

static Node *postfix(void) {
    Node *n = primary();
    if ((n->t == N_VAR || n->t == N_LOCAL || n->t == N_FIELD || n->t == N_INDEX) && (tt() == T_INCR || tt() == T_DECR)) {
        Node *p = mk(N_POSTINC, n, NULL);
        p->op = tt() == T_INCR ? 1 : -1;
        adv();
        return p;
    }
    return n;
}

static Node *power(void) {
    Node *n = postfix();
    if (tt() == T_CARET) {
        adv();
        Node *r;
        if (tt() == T_MINUS) { adv(); r = mk(N_NEG, power(), NULL); } else if (tt() == T_PLUS) { adv(); r = power(); } else if (tt() == T_NOT) { adv(); r = mk(N_NOT, power(), NULL); } else r = power();
        Node *a = mk(N_ARITH, n, r);
        a->op = '^';
        return a;
    }
    return n;
}

static Node *unary(void) {
    if (tt() == T_NOT) { adv(); return mk(N_NOT, unary(), NULL); }
    if (tt() == T_MINUS) { adv(); return mk(N_NEG, unary(), NULL); }
    if (tt() == T_PLUS) { adv(); return mk(N_PLUS, unary(), NULL); }
    return power();
}

static Node *mult(void) {
    Node *n = unary();
    while (tt() == T_STAR || tt() == T_SLASH || tt() == T_PERCENT) {
        int op = tt() == T_STAR ? '*' : tt() == T_SLASH ? '/' : '%';
        adv();
        n = mk(N_ARITH, n, unary());
        n->op = op;
    }
    return n;
}

static Node *additive(void) {
    Node *n = mult();
    while (tt() == T_PLUS || tt() == T_MINUS) {
        int op = tt() == T_PLUS ? '+' : '-';
        adv();
        n = mk(N_ARITH, n, mult());
        n->op = op;
    }
    return n;
}

static Node *concat(void) {
    Node *n = additive();
    while (starts_primary(tt()) && tt() != T_MINUS && tt() != T_PLUS && tt() != T_NOT && !(noin && tt() == T_IN))
        n = mk(N_CAT, n, additive());
    return n;
}

static Node *comparison(void) {
    Node *n = concat();
    while (tt() == T_PIPE && toks[ptok + 1].t == T_GETLINE) {       /* cmd | getline [var] */
        adv(); adv();
        Node *g = mk(N_GETLINE, NULL, NULL);
        g->c = n;
        if (tt() == T_NAME || tt() == T_DOLLAR) { Node *lv = primary(); g->a = lvalue_check(lv); }
        n = g;
    }
    int t = tt();
    if (t == T_LT || t == T_LE || t == T_EQ || t == T_NE || (t == T_GT && !nogt) || t == T_GE) {
        adv();
        Node *c = mk(N_CMP, n, concat());
        c->op = t;
        return c;
    }
    return n;
}

static Node *matchexpr(void) {
    Node *n = comparison();
    while (tt() == T_TILDE || tt() == T_NOMATCH) {
        int neg = tt() == T_NOMATCH;
        adv();
        n = mk(N_MATCH, n, comparison());
        n->op = neg;
    }
    return n;
}

static Node *inexpr(void) {
    Node *n = matchexpr();
    while (tt() == T_IN && !noin) {
        adv();
        if (tt() != T_NAME) syntax("array name expected after 'in'");
        n = mk(N_IN, n, varref(tk()->s));
        adv();
    }
    return n;
}

static Node *andexpr(void) {
    Node *n = inexpr();
    while (tt() == T_AND) { adv(); skip_nl(); n = mk(N_AND, n, inexpr()); }
    return n;
}

static Node *orexpr(void) {
    Node *n = andexpr();
    while (tt() == T_OR) { adv(); skip_nl(); n = mk(N_OR, n, andexpr()); }
    return n;
}

static Node *ternary(void) {
    Node *c = orexpr();
    if (tt() == T_QUESTION) {
        adv(); skip_nl();
        Node *a = ternary();
        skip_nl();
        expect(T_COLON, "expected : in ?:");
        skip_nl();
        Node *b = ternary();
        Node *n = mk(N_COND, c, a);
        n->c = b;
        return n;
    }
    return c;
}

static Node *expr(void) {
    Node *l = ternary();
    int t = tt();
    if (t == T_ASSIGN || (t >= T_ADDEQ && t <= T_POWEQ)) {
        Node *base = l->t == N_GROUP ? NULL : l;
        if (!base || (l->t != N_VAR && l->t != N_LOCAL && l->t != N_FIELD && l->t != N_INDEX)) syntax("assignment to non-lvalue");
        adv(); skip_nl();
        Node *r = expr();
        Node *n = mk(N_ASSIGN, l, r);
        n->op = t == T_ASSIGN ? 0 : "+-*/%^"[t - T_ADDEQ];
        return n;
    }
    return l;
}

static void endstmt(void) {
    if (tt() == T_SEMI || tt() == T_NL) { adv(); return; }
    if (tt() == T_RBRACE || tt() == T_EOF) return;
    syntax("expected end of statement");
}

static Node *block(void) {
    expect(T_LBRACE, "expected {");
    Node *head = mk(S_BLOCK, NULL, NULL), **tail = &head->a;
    skip_term();
    while (tt() != T_RBRACE) {
        if (tt() == T_EOF) syntax("missing }");
        *tail = stmt();
        tail = &(*tail)->next;
        skip_term();
    }
    adv();
    return head;
}

static Node *simple_or_block(void) {
    skip_nl();
    return stmt();
}

static Node *simple(void) {
    Node *n;
    switch (tt()) {
    case T_PRINT: case T_PRINTF: {
        int isf = tt() == T_PRINTF;
        adv();
        n = mk(isf ? S_PRINTF : S_PRINT, NULL, NULL);
        nogt = 1;
        if (tt() == T_LPAREN) {                            /* print (a,b) > "f" */
            int d = 0, k = ptok;
            for (; toks[k].t != T_EOF; k++) {
                if (toks[k].t == T_LPAREN) d++;
                else if (toks[k].t == T_RPAREN && --d == 0) break;
            }
            int after = toks[k].t == T_EOF ? T_EOF : toks[k + 1].t;
            if (after == T_NL || after == T_SEMI || after == T_RBRACE || after == T_GT || after == T_APPEND || after == T_PIPE || after == T_EOF) {
                adv();
                n->a = exprlist(T_RPAREN);
                expect(T_RPAREN, "expected )");
                goto redirect;
            }
        }
        if (tt() != T_NL && tt() != T_SEMI && tt() != T_RBRACE && tt() != T_GT && tt() != T_APPEND && tt() != T_PIPE) {
            Node *head = NULL, **tail = &head;
            for (;;) {
                *tail = expr();
                tail = &(*tail)->next;
                if (tt() != T_COMMA) break;
                adv(); skip_nl();
            }
            n->a = head;
        }
    redirect:
        if (tt() == T_GT || tt() == T_APPEND) {
            n->op = tt() == T_GT ? 1 : 2;
            adv();
            Node *f = concat();
            n->b = f;
        } else if (tt() == T_PIPE) { n->op = 3; adv(); n->b = concat(); }
        nogt = 0;
        return n;
    }
    case T_DELETE: {
        adv();
        if (tt() != T_NAME) syntax("array name expected");
        Node *arr = varref(tk()->s);
        adv();
        Node *subs = NULL;
        if (tt() == T_LBRACKET) { adv(); subs = exprlist(T_RBRACKET); expect(T_RBRACKET, "expected ]"); }
        return mk(S_DELETE, arr, subs);
    }
    case T_NEXT: adv(); return mk(S_NEXT, NULL, NULL);
    case T_BREAK: adv(); return mk(S_BREAK, NULL, NULL);
    case T_CONTINUE: adv(); return mk(S_CONT, NULL, NULL);
    case T_EXIT: case T_RETURN: {
        int t = tt();
        adv();
        n = mk(t == T_EXIT ? S_EXIT : S_RETURN, NULL, NULL);
        if (tt() != T_NL && tt() != T_SEMI && tt() != T_RBRACE && tt() != T_EOF) n->a = expr();
        return n;
    }
    }
    return mk(S_EXPR, expr(), NULL);
}

static Node *stmt(void) {
    Node *n;
    switch (tt()) {
    case T_LBRACE: return block();
    case T_IF: {
        adv(); expect(T_LPAREN, "expected ( after if");
        Node *c = expr();
        expect(T_RPAREN, "expected ) after if condition");
        Node *th = simple_or_block();
        n = mk(S_IF, c, th);
        int save = ptok;
        skip_term();
        if (tt() == T_ELSE) { adv(); n->c = simple_or_block(); } else ptok = save;
        return n;
    }
    case T_WHILE: {
        adv(); expect(T_LPAREN, "expected ( after while");
        Node *c = expr();
        expect(T_RPAREN, "expected )");
        if (tt() == T_SEMI) { adv(); return mk(S_WHILE, c, NULL); }
        return mk(S_WHILE, c, simple_or_block());
    }
    case T_DO: {
        adv();
        Node *body = simple_or_block();
        skip_term();
        if (tt() != T_WHILE) syntax("expected while after do body");
        adv(); expect(T_LPAREN, "expected (");
        Node *c = expr();
        expect(T_RPAREN, "expected )");
        return mk(S_DO, c, body);
    }
    case T_FOR: {
        adv(); expect(T_LPAREN, "expected ( after for");
        if (tt() == T_NAME && toks[ptok + 1].t == T_IN && toks[ptok + 2].t == T_NAME && toks[ptok + 3].t == T_RPAREN) {
            Node *v = varref(tk()->s);
            adv(); adv();
            Node *arr = varref(tk()->s);
            adv(); adv();
            n = mk(S_FORIN, v, arr);
            n->c = simple_or_block();
            return n;
        }
        Node *init = NULL, *cond = NULL, *incr = NULL;
        if (tt() != T_SEMI) init = simple();
        expect(T_SEMI, "expected ; in for");
        skip_nl();
        if (tt() != T_SEMI) cond = expr();
        expect(T_SEMI, "expected ; in for");
        skip_nl();
        if (tt() != T_RPAREN) incr = simple();
        expect(T_RPAREN, "expected ) after for");
        n = mk(S_FOR, init, cond);
        n->c = incr;
        if (tt() == T_SEMI) { adv(); return n; }
        n->d = simple_or_block();
        return n;
    }
    case T_SEMI: adv(); return mk(S_BLOCK, NULL, NULL);
    }
    n = simple();
    endstmt();
    return n;
}

static void parse_program(void) {
    skip_term();
    while (tt() != T_EOF) {
        if (tt() == T_FUNCTION) {
            adv();
            if (tt() != T_NAME && tt() != T_FUNCNAME) syntax("function name expected");
            Func *f;
            int id = findfunc(tk()->s);
            adv();
            f = &funcs[id];
            expect(T_LPAREN, "expected (");
            f->nparams = 0; f->params = NULL;
            skip_nl();
            while (tt() == T_NAME) {
                f->params = realloc(f->params, (f->nparams + 1) * sizeof *f->params);
                f->params[f->nparams++] = tk()->s;
                adv(); skip_nl();
                if (tt() == T_COMMA) { adv(); skip_nl(); }
            }
            expect(T_RPAREN, "expected ) in function definition");
            skip_nl();
            curfunc = f;
            f->body = block();
            curfunc = NULL;
        } else if (tt() == T_BEGIN || tt() == T_END) {
            int b = tt() == T_BEGIN;
            adv();
            Item *it = calloc(1, sizeof *it);
            if (tt() != T_LBRACE) syntax("BEGIN/END need an action");
            it->action = block();
            if (b) { *begtail = it; begtail = &it->next; } else { *endtail = it; endtail = &it->next; }
        } else {
            Item *it = calloc(1, sizeof *it);
            if (tt() != T_LBRACE) {
                it->pat = expr();
                if (tt() == T_COMMA) { adv(); skip_nl(); it->pat2 = expr(); }
            }
            if (tt() == T_LBRACE) it->action = block();
            *itemtail = it; itemtail = &it->next;
        }
        skip_term();
    }
    for (int i = 0; i < nfuncs; i++) if (funcs[i].nparams < 0) fatal("calling undefined function %s", funcs[i].name);
}

/* ---------------------------------------------------------------- regex cache */
static struct { char *src; regex_t re; } rcache[64];
static int rnext;
static regex_t *getre(const char *src) {
    for (int i = 0; i < 64; i++) if (rcache[i].src && !strcmp(rcache[i].src, src)) return &rcache[i].re;
    int i = rnext++ & 63;
    if (rcache[i].src) { regfree(&rcache[i].re); free(rcache[i].src); }
    int e = regcomp(&rcache[i].re, src, REG_EXTENDED);
    if (e) { char m[100]; regerror(e, &rcache[i].re, m, sizeof m); rcache[i].src = NULL; fatal("regular expression /%s/: %s", src, m); }
    rcache[i].src = strdup(src);
    return &rcache[i].re;
}

/* ---------------------------------------------------------------- records and fields */
static char *rec = EMPTY;
static char **fld;
static int nf, fldcap;
static int fields_ok = 1, rec_ok = 1;
static char *rec_fs;
static Cell *gv(int i) { return &globals[i]; }
static const char *gstr(int i) { Cell *c = gv(i); return c->kind == 2 && (c->v.f & F_STR) ? c->v.s : c->kind == 2 ? tostr(c->v) : ""; }

static void setnf_cell(void) { Cell *c = gv(V_NF); Val v = vnum(nf); setcell(c, v); }

static void splitstr(const char *s, const char *fs, int paragraph, char ***out, int *n);

static void set_fields(char **v, int n) {
    for (int i = 1; i <= nf; i++) free(fld[i]);
    if (n + 2 > fldcap) fld = realloc(fld, (fldcap = n + 16) * sizeof *fld);
    for (int i = 1; i <= n; i++) fld[i] = v[i - 1];
    nf = n;
    setnf_cell();
}

static void split_record(void) {
    char **v; int n;
    splitstr(rec, rec_fs ? rec_fs : gstr(V_FS), !strcmp(gstr(V_RS), ""), &v, &n);
    set_fields(v, n);
    free(v);
    fields_ok = 1;
}

static void rebuild_rec(void) {
    const char *ofs = gstr(V_OFS);
    size_t n = 1, ol = strlen(ofs);
    for (int i = 1; i <= nf; i++) n += strlen(fld[i]) + ol;
    char *r = malloc(n), *w = r;
    for (int i = 1; i <= nf; i++) {
        if (i > 1) { memcpy(w, ofs, ol); w += ol; }
        size_t l = strlen(fld[i]);
        memcpy(w, fld[i], l); w += l;
    }
    *w = 0;
    if (rec != EMPTY) free(rec);
    rec = r;
    rec_ok = 1;
}

static void set_record(char *s) {          /* takes ownership */
    if (rec != EMPTY) free(rec);
    rec = s;
    free(rec_fs);
    rec_fs = strdup(gstr(V_FS));
    fields_ok = 0; rec_ok = 1;
}

static int fieldidx(Val v) {
    double d = tonum(v);
    if (d < 0 || d > 1e6) fatal("trying to access out of range field %.0f", d);
    return (int)d;
}

static Val getfield(int i) {
    if (i == 0) { if (!rec_ok) rebuild_rec(); Val v = { F_STR, 0, rec }; if (looksnum(rec, NULL)) v.f |= F_STRNUM; return v; }
    if (!fields_ok) split_record();
    if (i > nf) { Val v = { F_STR, 0, EMPTY }; return v; }
    Val v = { F_STR, 0, fld[i] };
    if (looksnum(fld[i], NULL)) v.f |= F_STRNUM;
    return v;
}

static void setfield(int i, const char *s) {
    if (i == 0) { set_record(strdup(s)); return; }
    if (!fields_ok) split_record();
    if (i > nf) {
        if (i + 2 > fldcap) fld = realloc(fld, (fldcap = i + 16) * sizeof *fld);
        for (int k = nf + 1; k <= i; k++) fld[k] = strdup("");
        nf = i;
        setnf_cell();
    }
    char *n = strdup(s);
    free(fld[i]);
    fld[i] = n;
    rec_ok = 0;
}

static void set_nf(int n) {
    if (!fields_ok) split_record();
    if (n < 0) n = 0;
    if (n > nf) {
        if (n + 2 > fldcap) fld = realloc(fld, (fldcap = n + 16) * sizeof *fld);
        for (int k = nf + 1; k <= n; k++) fld[k] = strdup("");
    } else for (int k = n + 1; k <= nf; k++) free(fld[k]);
    nf = n;
    setnf_cell();
    rec_ok = 0;
}

/* split s by fs into malloc'd strings */
static void splitstr(const char *s, const char *fs, int paragraph, char ***out, int *n) {
    char **v = NULL;
    int cnt = 0, cap = 0;
#define PUSH(p, l) do { if (cnt == cap) v = realloc(v, (cap = cap ? cap * 2 : 16) * sizeof *v); v[cnt++] = dupn(p, l); } while (0)
    if (!*fs) {                                   /* empty separator: one field per byte */
        for (; *s; s++) PUSH(s, 1);
    } else if (!strcmp(fs, " ")) {
        while (*s) {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            if (!*s) break;
            const char *e = s;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
            PUSH(s, e - s);
            s = e;
        }
    } else if (*s) {
        if (strlen(fs) == 1 && *fs != '\\' && !paragraph) {
            for (;;) {
                const char *e = strchr(s, *fs);
                if (!e) { PUSH(s, strlen(s)); break; }
                PUSH(s, e - s);
                s = e + 1;
            }
        } else {
            char *pat = (char *)fs;
            int heap = 0;
            if (paragraph) {
                heap = 1;
                pat = malloc(strlen(fs) + 16);
                if (strlen(fs) == 1 && *fs != '\\') sprintf(pat, "[%s\n]", *fs == '^' ? "\\^" : fs); else sprintf(pat, "(%s)|\n", fs);
            } else if (strlen(fs) == 1) pat = "\\\\";
            regex_t *re = getre(pat);
            if (heap) free(pat);
            size_t len = strlen(s), pos = 0, st = 0;
            for (;;) {
                regmatch_t m[1] = { { (regoff_t)pos, (regoff_t)len } };
                if (pos > len || regexec(re, s, 1, m, REG_STARTEND)) { PUSH(s + st, len - st); break; }
                if (m[0].rm_eo == m[0].rm_so) { pos = m[0].rm_so + 1; continue; }     /* empty matches don't split */
                PUSH(s + st, m[0].rm_so - st);
                st = pos = m[0].rm_eo;
            }
        }
    }
    *out = v; *n = cnt;
}

/* ---------------------------------------------------------------- input sources */
typedef struct { char *buf; size_t len, pos; char *name; int eof; } Src;

static int readrec(Src *s, char **out) {
    if (s->pos >= s->len) return 0;
    const char *rs = gstr(V_RS);
    char *b = s->buf + s->pos;
    size_t rem = s->len - s->pos, n, adv_;
    if (!*rs) {                                   /* paragraph mode */
        while (rem && *b == '\n') { b++; rem--; s->pos++; }
        if (!rem) return 0;
        char *e = b;
        while ((size_t)(e - b) < rem && !(e[0] == '\n' && ((size_t)(e - b) + 1 == rem || e[1] == '\n'))) e++;
        n = e - b;
        adv_ = n;
        while (adv_ < rem && b[adv_] == '\n') adv_++;
        if (n && b[n - 1] == '\n') n--;
        if (adv_ == rem && n == rem && b[n - 1] == '\n') n--;
    } else if (rs[1]) {                           /* regex RS */
        regex_t *re = getre(rs);
        regmatch_t m[1] = { { (regoff_t)s->pos, (regoff_t)s->len } };
        if (!regexec(re, s->buf, 1, m, REG_STARTEND) && m[0].rm_eo > m[0].rm_so) { n = m[0].rm_so - s->pos; adv_ = m[0].rm_eo - s->pos; }
        else { n = rem; adv_ = rem; if (n && b[n - 1] == '\n') n--; }
    } else {
        char *e = memchr(b, rs[0], rem);
        n = e ? (size_t)(e - b) : rem;
        adv_ = e ? n + 1 : n;
    }
    *out = dupn(b, n);
    s->pos += adv_;
    return 1;
}

static Src *opensrc(const char *name) {
    Src *s = calloc(1, sizeof *s);
    s->buf = slurp("awk", name, &s->len);
    if (!s->buf) { free(s); return NULL; }
    s->name = strdup(name);
    return s;
}

static Src main_src;
static int arg_i = 1, main_started, main_done;
static Arr *argv_arr(void) { return gv(V_ARGV)->arr; }

static void assign_cmdvar(const char *a);

/* next record from the operands; 0 at the end */
static int main_record(char **out) {
    for (;;) {
        if (main_src.buf && readrec(&main_src, out)) return 1;
        if (main_src.buf) { free(main_src.buf); main_src.buf = NULL; }
        if (main_done) return 0;
        int argc = (int)tonum(cellval(gv(V_ARGC)));
        for (;;) {
            if (arg_i >= argc) {
                if (main_started) { main_done = 1; break; }
                main_started = 1;
                main_done = 1;
                main_src.buf = slurp("awk", "-", &main_src.len);
                if (!main_src.buf) main_src.buf = dupn("", 0), main_src.len = 0;
                main_src.pos = 0;
                setcell(gv(V_FILENAME), vstr(EMPTY));
                break;
            }
            Ent *e = arr_find(argv_arr(), tmp(numstr(arg_i, "%d")));
            arg_i++;
            if (!e || e->c.kind != 2) continue;
            const char *a = tostr(cellval(&e->c));
            if (!*a) continue;
            if (strchr(a, '=') && (isalpha((unsigned char)*a) || *a == '_') && strcspn(a, "=") == strspn(a, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")) { assign_cmdvar(a); continue; }
            main_started = 1;
            char *b = !strcmp(a, "-") ? slurp("awk", "-", &main_src.len) : slurp("awk", a, &main_src.len);
            if (!b) { fflush(stdout); exit(2); }
            main_src.buf = b; main_src.pos = 0;
            setcell(gv(V_FILENAME), vstr((char *)a));
            setcell(gv(V_FNR), vnum(0));
            break;
        }
        if (!main_src.buf) return 0;
    }
}

/* ---------------------------------------------------------------- output streams */
static struct { char *name; FILE *f; int pid; } outs[32];
static FILE *getout(const char *name, int mode) {
    if (!strcmp(name, "/dev/stdout") || !strcmp(name, "-")) return stdout;
    if (!strcmp(name, "/dev/stderr")) return stderr;
    for (int i = 0; i < 32; i++) if (outs[i].name && !strcmp(outs[i].name, name)) return outs[i].f;
    for (int i = 0; i < 32; i++)
        if (!outs[i].name) {
            FILE *f;
            outs[i].pid = 0;
            if (mode == 3) {                              /* print | "cmd": sh -c cmd with a pipe on its stdin */
                int p[2];
                char *av[] = { "sh", "-c", (char *)name, NULL };
                struct spawn_opts o = { { p[0], -1, -1 }, NULL, 0 };
                fflush(NULL);
                if (pipe(p) < 0) fatal("can't create pipe");
                o.fd[0] = p[0];
                outs[i].pid = spawnve("/bin/sh", av, environ, &o);
                close(p[0]);
                if (outs[i].pid < 0) fatal("can't run %s", name);
                f = fdopen(p[1], "w");
            } else f = fopen(name, mode == 2 ? "a" : "w");
            if (!f) fatal("can't redirect to %s", name);
            outs[i].name = strdup(name); outs[i].f = f;
            return f;
        }
    fatal("too many open output files");
    return NULL;
}
static int closeout(const char *name) {
    for (int i = 0; i < 32; i++)
        if (outs[i].name && !strcmp(outs[i].name, name)) {
            int r = fclose(outs[i].f), st = 0;
            if (outs[i].pid > 0) { waitpid(outs[i].pid, &st, 0); r = st; }
            free(outs[i].name); outs[i].name = NULL;
            return r;
        }
    return -1;
}
static struct { char *name; Src *s; } ins[16];

static Src *opencmd(const char *cmd) {                    /* cmd | getline: run it to completion, keep its output */
    int p[2];
    char *av[] = { "sh", "-c", (char *)cmd, NULL };
    struct spawn_opts o = { { -1, -1, -1 }, NULL, 0 };
    fflush(NULL);
    if (pipe(p) < 0) return NULL;
    o.fd[1] = p[1];
    int pid = spawnve("/bin/sh", av, environ, &o);
    close(p[1]);
    if (pid < 0) { close(p[0]); return NULL; }
    Src *s = calloc(1, sizeof *s);
    s->buf = readall(p[0], &s->len);
    close(p[0]);
    waitpid(pid, NULL, 0);
    if (!s->buf) { free(s); return NULL; }
    s->name = strdup(cmd);
    return s;
}

/* ---------------------------------------------------------------- evaluation */
enum { RC_OK, RC_BREAK, RC_CONT, RC_NEXT, RC_EXIT, RC_RETURN };
static int exit_code, nest;
static Cell *frame;
static Val retval;
static char *retbuf;

static Val eval(Node *n);
static int exec(Node *n);
static jmp_buf next_jb, exit_jb;
static void awk_do_exit(void) { longjmp(exit_jb, 1); }
static void awk_do_next(void) { longjmp(next_jb, 1); }

static Cell *cellof(Node *n) { return n->t == N_VAR ? &globals[n->idx] : &frame[n->idx]; }

static Arr *getarr(Node *n) {
    Cell *c = cellof(n);
    if (c->kind == 1) return c->arr;
    if (c->kind == 2) fatal("can't use scalar %s as array", n->t == N_VAR ? gnames[n->idx] : "parameter");
    c->arr = newarr(); c->kind = 1;
    if (c->ref) { c->ref->arr = c->arr; c->ref->kind = 1; }
    else c->own = 1;
    return c->arr;
}

static char *subscript(Node *subs) {
    if (!subs->next) { Val v = eval(subs); return tostr(v); }
    size_t cap = 64, n = 0;
    char *b = malloc(cap);
    const char *sep = gstr(V_SUBSEP);
    for (Node *s = subs; s; s = s->next) {
        const char *p = tostr(eval(s));
        size_t l = strlen(p), sl = s->next ? strlen(sep) : 0;
        while (n + l + sl + 1 > cap) b = realloc(b, cap *= 2);
        memcpy(b + n, p, l); n += l;
        memcpy(b + n, sep, sl); n += sl;
    }
    b[n] = 0;
    return tmp(b);
}

static Cell *lvcell(Node *n) {
    switch (n->t) {
    case N_VAR: case N_LOCAL: return cellof(n);
    case N_INDEX: { Arr *a = getarr(n->a); char *k = subscript(n->b); return &arr_get(a, k)->c; }
    }
    return NULL;
}

static void special_assigned(int idx) {
    if (idx == V_NF) { set_nf((int)tonum(cellval(gv(V_NF)))); }
    else if (idx == V_CONVFMT) convfmt_s = (char *)gstr(V_CONVFMT);
    else if (idx == V_OFMT) ofmt_s = (char *)gstr(V_OFMT);
}

static void assign(Node *lv, Val v) {
    if (lv->t == N_FIELD) {
        Val iv = eval(lv->a);
        int i = fieldidx(iv);
        setfield(i, tostr(v));
        return;
    }
    Cell *c = lvcell(lv);
    setcell(c, v);
    if (lv->t == N_VAR && lv->idx < V_NSPECIAL) special_assigned(lv->idx);
}

static Val readlv(Node *lv) {
    if (lv->t == N_FIELD) return getfield(fieldidx(eval(lv->a)));
    Cell *c = lvcell(lv);
    if (lv->t == N_VAR && lv->idx == V_NF && !fields_ok) split_record();
    return cellval(c);
}

static Val arith(int op, double x, double y) {
    switch (op) {
    case '+': return vnum(x + y);
    case '-': return vnum(x - y);
    case '*': return vnum(x * y);
    case '/': if (y == 0) fatal("division by zero"); return vnum(x / y);
    case '%': if (y == 0) fatal("division by zero in %%"); return vnum(fmod(x, y));
    default: return vnum(pow(x, y));
    }
}

static regex_t *regex_of(Node *n) {
    if (n->t == N_ERE) { if (!n->re) n->re = getre(n->str); return n->re; }
    return getre(tostr(eval(n)));
}

static int nargs(Node *a) { int k = 0; for (; a; a = a->next) k++; return k; }

/* formatted output for printf/sprintf */
static char *format(Node *args) {
    if (!args) fatal("printf: no format");
    const char *f = tostr(eval(args));
    Node *arg = args->next;
    size_t cap = 128, n = 0;
    char *out = malloc(cap);
#define OUT(p, l) do { size_t _l = (l); while (n + _l + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + n, (p), _l); n += _l; } while (0)
    char *fcopy = strdup(f);
    for (const char *p = fcopy; *p;) {
        if (*p != '%') { const char *e = strchr(p, '%'); size_t l = e ? (size_t)(e - p) : strlen(p); OUT(p, l); p += l; continue; }
        if (p[1] == '%') { OUT("%", 1); p += 2; continue; }
        char spec[64];
        int sl = 0;
        spec[sl++] = *p++;
        while (*p && strchr("-+ #0", *p) && sl < 20) spec[sl++] = *p++;
        for (int part = 0; part < 2; part++) {
            if (*p == '*') {
                if (!arg) fatal("not enough arguments to printf");
                sl += snprintf(spec + sl, 20, "%d", (int)tonum(eval(arg)));
                arg = arg->next; p++;
            } else while (isdigit((unsigned char)*p) && sl < 40) spec[sl++] = *p++;
            if (part == 0 && *p == '.') spec[sl++] = *p++; else break;
        }
        while (*p == 'l' || *p == 'h' || *p == 'q' || *p == 'L') p++;
        int c = *p ? *p++ : 0;
        char tmpb[512];
        if (!c) { OUT(spec, sl); break; }
        if (!strchr("diouxXeEfFgGcs", c)) { OUT(spec, sl); OUT(&c, 1); continue; }
        if (!arg) fatal("not enough arguments to printf (%%%c)", c);
        Val v = eval(arg);
        arg = arg->next;
        switch (c) {
        case 'd': case 'i': {
            double d = tonum(v);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = 'd'; spec[sl] = 0;
            long long x = d >= 9.2e18 ? 0x7fffffffffffffffLL : d <= -9.2e18 ? (-0x7fffffffffffffffLL - 1) : (long long)d;
            if (isnan(d)) x = 0;
            int k = snprintf(tmpb, sizeof tmpb, spec, x);
            OUT(tmpb, k < (int)sizeof tmpb ? (size_t)k : sizeof tmpb - 1);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            double d = tonum(v);
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = c; spec[sl] = 0;
            unsigned long long x = d < 0 ? (unsigned long long)(long long)d : (unsigned long long)d;
            int k = snprintf(tmpb, sizeof tmpb, spec, x);
            OUT(tmpb, k < (int)sizeof tmpb ? (size_t)k : sizeof tmpb - 1);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            spec[sl++] = c; spec[sl] = 0;
            int k = snprintf(tmpb, sizeof tmpb, spec, tonum(v));
            OUT(tmpb, k < (int)sizeof tmpb ? (size_t)k : sizeof tmpb - 1);
            break;
        }
        case 'c': {
            int isnum = (v.f & F_NUM) && !(v.f & F_STR);
            char ch[2] = { 0, 0 };
            if (isnum) ch[0] = (char)(int)v.n; else ch[0] = tostr(v)[0];
            if (isnum && !ch[0]) { OUT(ch, 1); break; }
            spec[sl++] = 's'; spec[sl] = 0;
            int k = snprintf(tmpb, sizeof tmpb, spec, ch);
            OUT(tmpb, k);
            break;
        }
        case 's': {
            const char *s = tostr_f(v, convfmt_s);
            spec[sl++] = 's'; spec[sl] = 0;
            size_t need = strlen(s) + 64;
            char *bigb = need > sizeof tmpb ? malloc(need) : tmpb;
            int k = snprintf(bigb, need, spec, s);
            OUT(bigb, k);
            if (bigb != tmpb) free(bigb);
            break;
        }
        }
    }
    free(fcopy);
    out[n] = 0;
    return out;
}

/* sub/gsub: returns count; writes result via assign */
static Val do_sub(Node *n, int global) {
    Node *a = n->a;
    if (nargs(a) < 2) fatal("sub/gsub need 2 or 3 arguments");
    regex_t *re = regex_of(a);
    const char *repl = tostr(eval(a->next));
    Node *target = a->next->next;
    Val tv = target ? readlv(target->t == N_GROUP ? target->a : target) : getfield(0);
    const char *s = tostr(tv);
    char *src = tmp(strdup(s));
    size_t len = strlen(src), pos = 0, cap = len + 64, on = 0;
    char *out = malloc(cap);
    int count = 0, lasteo = -1;
#define APP(p, l) do { size_t _l = (l); while (on + _l + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + on, (p), _l); on += _l; } while (0)
    while (pos <= len) {
        regmatch_t m[1] = { { (regoff_t)pos, (regoff_t)len } };
        if (regexec(re, src, 1, m, REG_STARTEND)) break;
        size_t so = m[0].rm_so, eo = m[0].rm_eo;
        if (so == eo && (int)so == lasteo) {            /* empty match right after a match */
            if (so < len) APP(src + pos, so - pos + 1);
            pos = so + 1;
            continue;
        }
        APP(src + pos, so - pos);
        for (const char *r = repl; *r; r++) {
            if (*r == '\\' && (r[1] == '&' || r[1] == '\\')) { APP(r + 1, 1); r++; }
            else if (*r == '&') APP(src + so, eo - so);
            else APP(r, 1);
        }
        count++;
        lasteo = (int)eo;
        if (so == eo) { if (so < len) APP(src + so, 1); pos = so + 1; } else pos = eo;
        if (!global) break;
    }
    if (count) {
        if (pos < len) APP(src + pos, len - pos);
        out[on] = 0;
        if (target) assign(target->t == N_GROUP ? target->a : target, vtmp(out));
        else setfield(0, tmp(out));
    } else free(out);
    return vnum(count);
}

static Val builtin(Node *n) {
    Node *a = n->a;
    int na = nargs(a);
    switch (n->idx) {
    case B_LENGTH: {
        if (!a) return vnum(strlen(tostr(getfield(0))));
        Node *x = a->t == N_GROUP ? a->a : a;
        if ((x->t == N_VAR || x->t == N_LOCAL) && cellof(x)->kind == 1) return vnum(cellof(x)->arr->count);
        return vnum(strlen(tostr(eval(a))));
    }
    case B_SUBSTR: {
        if (na < 2) fatal("substr needs 2 or 3 arguments");
        const char *s = tostr(eval(a));
        size_t len = strlen(s);
        double m = tonum(eval(a->next));
        double st = floor(m + 0.5), en;
        if (isnan(m)) return vstr(EMPTY);
        if (na > 2) { double l = tonum(eval(a->next->next)); if (isnan(l)) return vstr(EMPTY); en = st + floor(l + 0.5); }
        else en = (double)len + 1;
        if (st < 1) st = 1;
        if (en > (double)len + 1) en = (double)len + 1;
        if (en <= st) return vstr(EMPTY);
        return vtmp(dupn(s + (size_t)st - 1, (size_t)(en - st)));
    }
    case B_INDEX: {
        if (na != 2) fatal("index needs 2 arguments");
        const char *s = tostr(eval(a)), *t = tostr(eval(a->next));
        const char *p = strstr(s, t);
        return vnum(p ? p - s + 1 : 0);
    }
    case B_SPLIT: {
        if (na < 2) fatal("split needs 2 or 3 arguments");
        char *s = tostr(eval(a));
        Node *an = a->next->t == N_GROUP ? a->next->a : a->next;
        if (an->t != N_VAR && an->t != N_LOCAL) fatal("split: second argument must be an array");
        char *src = tmp(strdup(s));
        const char *fs;
        char **v; int cnt;
        if (na > 2) {
            Node *fn = a->next->next;
            if (fn->t == N_ERE) { /* regex literal: pass as pattern */ fs = fn->str; if (strlen(fs) == 1 && *fs != ' ') fs = tmp(strdup(fs)); else if (!strcmp(fs, " ")) fs = "[ ]"; }
            else fs = tostr(eval(fn));
        } else fs = gstr(V_FS);
        splitstr(src, fs, 0, &v, &cnt);
        Arr *arr = getarr(an);
        arr_clear(arr);
        for (int i = 0; i < cnt; i++) {
            char k[24];
            snprintf(k, sizeof k, "%d", i + 1);
            Ent *e = arr_get(arr, k);
            Val x = { F_STR, 0, v[i] };
            if (looksnum(v[i], NULL)) x.f |= F_STRNUM;
            setcell(&e->c, x);
            free(v[i]);
        }
        free(v);
        return vnum(cnt);
    }
    case B_SUB: return do_sub(n, 0);
    case B_GSUB: return do_sub(n, 1);
    case B_MATCH: {
        if (na < 2) fatal("match needs 2 arguments");
        const char *s = tostr(eval(a));
        char *src = tmp(strdup(s));
        regex_t *re = regex_of(a->next);
        regmatch_t m[1];
        if (regexec(re, src, 1, m, 0)) { setcell(gv(V_RSTART), vnum(0)); setcell(gv(V_RLENGTH), vnum(-1)); return vnum(0); }
        setcell(gv(V_RSTART), vnum(m[0].rm_so + 1));
        setcell(gv(V_RLENGTH), vnum(m[0].rm_eo - m[0].rm_so));
        return vnum(m[0].rm_so + 1);
    }
    case B_SPRINTF: return vtmp(format(a));
    case B_SIN: return vnum(sin(tonum(eval(a))));
    case B_COS: return vnum(cos(tonum(eval(a))));
    case B_ATAN2: if (na != 2) fatal("atan2 needs 2 arguments"); return vnum(atan2(tonum(eval(a)), tonum(eval(a->next))));
    case B_EXP: return vnum(exp(tonum(eval(a))));
    case B_LOG: return vnum(log(tonum(eval(a))));
    case B_SQRT: return vnum(sqrt(tonum(eval(a))));
    case B_INT: return vnum(trunc(tonum(eval(a))));
    case B_RAND: return vnum(rand() / (RAND_MAX + 1.0));
    case B_SRAND: {
        static double prev;
        double old = prev;
        prev = a ? tonum(eval(a)) : (double)time(NULL);
        srand((unsigned)prev);
        return vnum(old);
    }
    case B_TOLOWER: case B_TOUPPER: {
        char *s = strdup(tostr(eval(a)));
        for (char *p = s; *p; p++) *p = n->idx == B_TOLOWER ? tolower((unsigned char)*p) : toupper((unsigned char)*p);
        return vtmp(s);
    }
    case B_SYSTEM: {
        const char *cmd = tostr(eval(a));
        fflush(NULL);
        int r = system(cmd);
        return vnum(r);
    }
    case B_CLOSE: {
        const char *name = tostr(eval(a));
        for (int i = 0; i < 16; i++) if (ins[i].name && !strcmp(ins[i].name, name)) { free(ins[i].s->buf); free(ins[i].s); free(ins[i].name); ins[i].name = NULL; return vnum(0); }
        return vnum(closeout(name));
    }
    case B_FFLUSH: fflush(NULL); return vnum(0);
    }
    return vnum(0);
}

static Val call(Node *n) {
    Func *f = &funcs[n->idx];
    int na = nargs(n->a);
    if (na > f->nparams) fatal("function %s called with %d args, accepts only %d", f->name, na, f->nparams);
    if (++nest > 800) fatal("function call nesting too deep");
    Cell *nf_ = calloc(f->nparams ? f->nparams : 1, sizeof *nf_);
    Node *arg = n->a;
    for (int i = 0; i < f->nparams; i++, arg = arg ? arg->next : NULL) {
        if (!arg) continue;
        Node *x = arg->t == N_GROUP ? arg->a : arg;
        if ((x->t == N_VAR || x->t == N_LOCAL)) {
            Cell *c = cellof(x);
            if (c->kind == 1) { nf_[i].kind = 1; nf_[i].arr = c->arr; nf_[i].ref = c; continue; }
            if (c->kind == 0) { nf_[i].ref = c; continue; }
        }
        Val v = eval(arg);
        setcell(&nf_[i], v);
    }
    Cell *saved = frame;
    frame = nf_;
    int mark = ptop;
    retval = uninit();
    int r = exec(f->body);
    char *rs = NULL;
    Val rv = retval;
    if (r == RC_RETURN && (rv.f & F_STR)) rs = strdup(rv.s);
    frame = saved;
    for (int i = 0; i < f->nparams; i++) {
        Cell *c = &nf_[i];
        if (c->kind == 1 && c->own && !c->ref) arr_free(c->arr);
        else if (c->kind == 2 && (c->v.f & F_STR) && c->v.s != EMPTY) free(c->v.s);
    }
    free(nf_);
    release(mark);
    nest--;
    if (r == RC_EXIT) awk_do_exit();
    if (r == RC_NEXT) awk_do_next();
    if (r != RC_RETURN) return uninit();
    if (rs) { rv.s = tmp(rs); }
    return rv;
}

static int getline_rec(Node *n, Val *res) {
    char *r = NULL;
    int ok;
    if (n->b || n->c) {
        const char *name = tostr(eval(n->b ? n->b : n->c));
        Src *s = NULL;
        for (int i = 0; i < 16; i++) if (ins[i].name && !strcmp(ins[i].name, name)) s = ins[i].s;
        if (!s) {
            s = n->c ? opencmd(name) : opensrc(name);
            if (!s) { *res = vnum(-1); return 0; }
            for (int i = 0; i < 16; i++) if (!ins[i].name) { ins[i].name = strdup(name); ins[i].s = s; break; }
        }
        ok = readrec(s, &r);
        if (!ok) { *res = vnum(0); return 0; }
        if (n->a) { Val v = { F_STR, 0, r }; if (looksnum(r, NULL)) v.f |= F_STRNUM; assign(n->a, v); free(r); }
        else set_record(r);
    } else {
        ok = main_record(&r);
        if (!ok) { *res = vnum(0); return 0; }
        setcell(gv(V_NR), vnum(tonum(cellval(gv(V_NR))) + 1));
        setcell(gv(V_FNR), vnum(tonum(cellval(gv(V_FNR))) + 1));
        if (n->a) { Val v = { F_STR, 0, r }; if (looksnum(r, NULL)) v.f |= F_STRNUM; assign(n->a, v); free(r); }
        else set_record(r);
    }
    *res = vnum(1);
    return 1;
}

static Val eval(Node *n) {
    switch (n->t) {
    case N_NUM: return vnum(n->num);
    case N_STR: return vstr(n->str);
    case N_ERE: { if (!n->re) n->re = getre(n->str); Val v = getfield(0); return vnum(regexec(n->re, v.s, 0, NULL, 0) == 0); }
    case N_GROUP: return eval(n->a);
    case N_VAR: case N_LOCAL: case N_INDEX: case N_FIELD: return readlv(n);
    case N_ASSIGN: {
        Val v;
        if (n->op) {
            double l = tonum(readlv(n->a)), r = tonum(eval(n->b));
            v = arith(n->op, l, r);
        } else {
            v = eval(n->b);
            if (n->b->t == N_VAR || n->b->t == N_LOCAL || n->b->t == N_INDEX || n->b->t == N_FIELD) {}   /* copied by setcell */
        }
        if (v.f & F_STR) v.s = tmp(strdup(v.s));        /* the value outlives a self-assignment */
        assign(n->a, v);
        return v;
    }
    case N_COND: return truth(eval(n->a)) ? eval(n->b) : eval(n->c);
    case N_AND: return vnum(truth(eval(n->a)) && truth(eval(n->b)));
    case N_OR: return vnum(truth(eval(n->a)) || truth(eval(n->b)));
    case N_NOT: return vnum(!truth(eval(n->a)));
    case N_NEG: return vnum(-tonum(eval(n->a)));
    case N_PLUS: return vnum(tonum(eval(n->a)));
    case N_CMP: {
        Val a = eval(n->a);
        char *as = NULL;
        if (a.f & F_STR) { as = strdup(a.s); a.s = tmp(as); }
        Val b = eval(n->b);
        int c = compare(a, b);
        switch (n->op) {
        case T_LT: return vnum(c < 0);
        case T_LE: return vnum(c <= 0);
        case T_EQ: return vnum(c == 0);
        case T_NE: return vnum(c != 0);
        case T_GT: return vnum(c > 0);
        default: return vnum(c >= 0);
        }
    }
    case N_MATCH: {
        Val a = eval(n->a);
        char *s = tmp(strdup(tostr(a)));
        regex_t *re = regex_of(n->b);
        return vnum((regexec(re, s, 0, NULL, 0) == 0) != n->op);
    }
    case N_CAT: {
        char *a = tmp(strdup(tostr(eval(n->a))));
        const char *b = tostr(eval(n->b));
        size_t la = strlen(a), lb = strlen(b);
        char *r = malloc(la + lb + 1);
        memcpy(r, a, la); memcpy(r + la, b, lb + 1);
        return vtmp(r);
    }
    case N_ARITH: {
        double x = tonum(eval(n->a)), y = tonum(eval(n->b));
        return arith(n->op, x, y);
    }
    case N_PREINC: case N_POSTINC: {
        double old = tonum(readlv(n->a));
        assign(n->a, vnum(old + n->op));
        return vnum(n->t == N_PREINC ? old + n->op : old);
    }
    case N_IN: {
        Arr *a = getarr(n->b);
        return vnum(arr_find(a, subscript(n->a)) != NULL);
    }
    case N_CALL: return call(n);
    case N_BUILTIN: return builtin(n);
    case N_GETLINE: { Val r; getline_rec(n, &r); return r; }
    }
    fatal("internal: bad node %d", n->t);
    return vnum(0);
}


static void do_print(Node *n) {
    FILE *f = stdout;
    if (n->b) { const char *name = tostr(eval(n->b)); f = getout(name, n->op); }
    if (n->t == S_PRINTF) {
        char *s = format(n->a);
        fputs(s, f);
        free(s);
    } else if (!n->a) {
        fputs(tostr(getfield(0)), f);
        fputs(gstr(V_ORS), f);
    } else {
        const char *ofs = gstr(V_OFS);
        for (Node *a = n->a; a; a = a->next) {
            Val v = eval(a);
            fputs(tostr_f(v, ofmt_s), f);
            if (a->next) fputs(ofs, f);
        }
        fputs(gstr(V_ORS), f);
    }
    if (f == stderr) fflush(f);
}

static int exec(Node *n) {
    int r;
    for (; n; n = n->next) {
        int mark = ptop;
        switch (n->t) {
        case S_BLOCK: r = exec(n->a); if (r) return r; break;
        case S_EXPR: eval(n->a); release(mark); break;
        case S_PRINT: case S_PRINTF: do_print(n); release(mark); break;
        case S_IF: {
            int c = truth(eval(n->a));
            release(mark);
            if (c) { r = exec(n->b); if (r) return r; }
            else if (n->c) { r = exec(n->c); if (r) return r; }
            break;
        }
        case S_WHILE:
            for (;;) {
                int c = truth(eval(n->a));
                release(mark);
                if (!c) break;
                r = exec(n->b);
                if (r == RC_BREAK) break;
                if (r && r != RC_CONT) return r;
            }
            break;
        case S_DO:
            for (;;) {
                r = exec(n->b);
                if (r == RC_BREAK) break;
                if (r && r != RC_CONT) return r;
                int c = truth(eval(n->a));
                release(mark);
                if (!c) break;
            }
            break;
        case S_FOR:
            if (n->a) { exec(n->a); }
            for (;;) {
                if (n->b) { int c = truth(eval(n->b)); release(mark); if (!c) break; }
                r = exec(n->d);
                if (r == RC_BREAK) break;
                if (r && r != RC_CONT) return r;
                if (n->c) exec(n->c);
            }
            break;
        case S_FORIN: {
            Arr *a = getarr(n->b);
            int cnt = a->count, i = 0;
            char **keys = malloc((cnt + 1) * sizeof *keys);
            for (Ent *e = a->first; e; e = e->next) keys[i++] = strdup(e->key);
            for (i = 0; i < cnt; i++) {
                Val kv = { F_STR, 0, keys[i] };
                if (looksnum(keys[i], NULL)) kv.f |= F_STRNUM;
                assign(n->a, kv);
                r = exec(n->c);
                if (r == RC_BREAK) break;
                if (r && r != RC_CONT) { for (int k = 0; k < cnt; k++) free(keys[k]); free(keys); return r; }
            }
            for (int k = 0; k < cnt; k++) free(keys[k]);
            free(keys);
            break;
        }
        case S_BREAK: return RC_BREAK;
        case S_CONT: return RC_CONT;
        case S_NEXT: return RC_NEXT;
        case S_EXIT:
            if (n->a) exit_code = (int)tonum(eval(n->a));
            release(mark);
            return RC_EXIT;
        case S_RETURN:
            if (n->a) { Val v = eval(n->a); free(retbuf); retbuf = NULL; if (v.f & F_STR) { retbuf = strdup(v.s); v.s = retbuf; } retval = v; }
            else retval = uninit();
            release(mark);
            return RC_RETURN;
        case S_DELETE: {
            Arr *a = getarr(n->a);
            if (n->b) arr_del(a, subscript(n->b)); else arr_clear(a);
            release(mark);
            break;
        }
        default: fatal("internal: bad statement %d", n->t);
        }
    }
    return RC_OK;
}

static int pattern_match(Item *it) {
    if (!it->pat) return 1;
    int mark = ptop, r;
    if (!it->pat2) r = truth(eval(it->pat));
    else if (it->inrange) { r = 1; if (truth(eval(it->pat2))) it->inrange = 0; }
    else if (truth(eval(it->pat))) { r = 1; if (!truth(eval(it->pat2))) it->inrange = 1; }
    else r = 0;
    release(mark);
    return r;
}

static void run_items(Item *list, int *status) {
    for (Item *it = list; it; it = it->next) {
        int r = exec(it->action);
        if (r == RC_EXIT) { *status = RC_EXIT; return; }
    }
}

/* ---------------------------------------------------------------- setup */
static char *unesc(const char *s) {
    char *o = malloc(strlen(s) + 1), *w = o;
    while (*s) {
        if (*s == '\\' && s[1]) { s++; char c; if (escape(&s, &c)) *w++ = c; else *w++ = *s++; }
        else *w++ = *s++;
    }
    *w = 0;
    return o;
}

static void assign_cmdvar(const char *a) {
    size_t n = strcspn(a, "=");
    char *name = dupn(a, n);
    int idx = gvar(name);
    free(name);
    char *v = unesc(a + n + 1);
    Val x = { F_STR, 0, v };
    if (looksnum(v, &x.n)) { x.f |= F_STRNUM; }
    if (x.f & F_STRNUM) x.f |= 0;
    setcell(&globals[idx], x);
    if (idx < V_NSPECIAL) special_assigned(idx);
    free(v);
}

int main(int argc, char **argv) {
    int i = 1;
    char *fsopt = NULL, *prog = NULL;
    char *vars[64];
    int nv = 0;
    for (int k = 0; k < V_NSPECIAL; k++) gvar(vspecial[k]);
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        const char *f = argv[i] + 1;
        if (*f == 'F' || *f == 'v' || *f == 'f') {
            const char *a = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
            if (!a) goto usage;
            if (*f == 'F') fsopt = (char *)a;
            else if (*f == 'v') { if (nv < 64) vars[nv++] = (char *)a; }
            else {
                size_t n;
                char *b = slurp("awk", a, &n);
                if (!b) return 2;
                if (prog) { prog = realloc(prog, strlen(prog) + n + 2); strcat(prog, "\n"); strcat(prog, b); free(b); }
                else prog = b;
            }
        } else if (!strcmp(argv[i], "--version")) { puts("VibeOS awk"); return 0; }
        else goto usage;
    }
    if (!prog) { if (i >= argc) goto usage; prog = argv[i++]; }
    lex(prog);
    parse_program();
    /* defaults */
    setcell(gv(V_FS), vstr(" ")); setcell(gv(V_OFS), vstr(" ")); setcell(gv(V_ORS), vstr("\n")); setcell(gv(V_RS), vstr("\n"));
    setcell(gv(V_SUBSEP), vstr("\034")); setcell(gv(V_CONVFMT), vstr("%.6g")); setcell(gv(V_OFMT), vstr("%.6g"));
    convfmt_s = (char *)gstr(V_CONVFMT); ofmt_s = (char *)gstr(V_OFMT);
    setcell(gv(V_NF), vnum(0)); setcell(gv(V_NR), vnum(0)); setcell(gv(V_FNR), vnum(0));
    setcell(gv(V_RSTART), vnum(0)); setcell(gv(V_RLENGTH), vnum(-1)); setcell(gv(V_FILENAME), vstr(EMPTY));
    gv(V_ENVIRON)->kind = 1; gv(V_ENVIRON)->arr = newarr();
    gv(V_ARGV)->kind = 1; gv(V_ARGV)->arr = newarr();
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *k = dupn(*e, eq - *e);
        Val v = { F_STR, 0, (char *)eq + 1 };
        if (looksnum(eq + 1, NULL)) v.f |= F_STRNUM;
        setcell(&arr_get(gv(V_ENVIRON)->arr, k)->c, v);
        free(k);
    }
    setcell(&arr_get(gv(V_ARGV)->arr, "0")->c, vstr("awk"));
    int ac = 1;
    for (; i < argc; i++) {
        char k[16];
        snprintf(k, sizeof k, "%d", ac++);
        setcell(&arr_get(gv(V_ARGV)->arr, k)->c, vstr(argv[i]));
    }
    setcell(gv(V_ARGC), vnum(ac));
    if (fsopt) {
        char *f = !strcmp(fsopt, "t") ? strdup("\t") : unesc(fsopt);
        setcell(gv(V_FS), vstr(f)); free(f);
    }
    for (int k = 0; k < nv; k++) assign_cmdvar(vars[k]);

    int status = RC_OK;
    if (!setjmp(exit_jb)) {
        run_items(begins, &status);
    } else status = RC_EXIT;
    if (status != RC_EXIT && (items || ends)) {
        if (!setjmp(exit_jb)) {
            char *r;
            while (main_record(&r)) {
                set_record(r);
                setcell(gv(V_NR), vnum(tonum(cellval(gv(V_NR))) + 1));
                setcell(gv(V_FNR), vnum(tonum(cellval(gv(V_FNR))) + 1));
                if (setjmp(next_jb)) { release(0); continue; }
                for (Item *it = items; it; it = it->next) {
                    if (!pattern_match(it)) continue;
                    if (!it->action) { fputs(tostr(getfield(0)), stdout); fputs(gstr(V_ORS), stdout); release(0); continue; }
                    int rr = exec(it->action);
                    release(0);
                    if (rr == RC_NEXT) break;
                    if (rr == RC_EXIT) { status = RC_EXIT; goto done; }
                }
            }
        } else status = RC_EXIT;
    }
done:
    if (!setjmp(exit_jb)) {
        int st = RC_OK;
        if (!(status == RC_EXIT && 0)) run_items(ends, &st);
    }
    fflush(stdout);
    for (int k = 0; k < 32; k++) if (outs[k].name) closeout(outs[k].name);
    return exit_code;
usage:
    fputs("usage: awk [-F fs] [-v var=val] 'program' | -f progfile [file | var=val ...]\n", stderr);
    return 2;
}
