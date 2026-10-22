/* sort [-rnufbs] [-t SEP] [-k F[.C][,F[.C]][nrbf]]... [file...]   keys, -t separator; -u keeps the first of equal keys */
#include <vutil.h>
#include <ctype.h>

struct key { int f1, c1, f2, c2, n, r, b, f; };
static struct key keys[16], glob;
static int nkeys, sep = -1, uniq, stable;
struct ln { char *s; size_t idx; };

static int isb(int c) { return c == ' ' || c == '\t'; }

static const char *field(const char *s, int f) {            /* start of field f (1-based) */
    if (sep >= 0) {
        while (--f > 0) { while (*s && *s != sep) s++; if (*s) s++; }
        return s;
    }
    while (--f > 0) { while (isb(*s)) s++; while (*s && !isb(*s)) s++; }
    return s;
}
static const char *fend(const char *s) {                   /* end of the field starting at s */
    if (sep >= 0) { while (*s && *s != sep) s++; return s; }
    while (isb(*s)) s++;
    while (*s && !isb(*s)) s++;
    return s;
}
static void span(const char *line, const struct key *k, const char **b, const char **e) {
    const char *s = field(line, k->f1);
    if (k->b) while (isb(*s)) s++;
    for (int c = k->c1 > 1 ? k->c1 - 1 : 0; c && *s; c--) s++;
    const char *t = line + strlen(line);
    if (k->f2) {
        const char *fs = field(line, k->f2);
        t = k->c2 ? fs : fend(fs);
        if (k->c2) { const char *x = fs; if (k->b) while (isb(*x)) x++; for (int c = k->c2; c && *x; c--) x++; t = x; }
    }
    if (t < s) t = s;
    *b = s; *e = t;
}
static double num(const char *s, const char *e) {
    while (s < e && isb(*s)) s++;
    int neg = 0;
    if (s < e && (*s == '-' || *s == '+')) neg = *s++ == '-';
    double v = 0, sc = 1;
    for (; s < e && isdigit((unsigned char)*s); s++) v = v * 10 + (*s - '0');
    if (s < e && *s == '.') for (s++; s < e && isdigit((unsigned char)*s); s++) { sc /= 10; v += (*s - '0') * sc; }
    return neg ? -v : v;
}
static int cmpkey(const char *a, const char *b, const struct key *k) {
    const char *as, *ae, *bs, *be;
    span(a, k, &as, &ae); span(b, k, &bs, &be);
    int r;
    if (k->n) { double x = num(as, ae), y = num(bs, be); r = x < y ? -1 : x > y; }
    else {
        for (r = 0; r == 0 && (as < ae || bs < be); as++, bs++) {
            if (as >= ae) { r = -1; break; }
            if (bs >= be) { r = 1; break; }
            int x = (unsigned char)*as, y = (unsigned char)*bs;
            if (k->f) { x = toupper(x); y = toupper(y); }
            r = x < y ? -1 : x > y;
        }
    }
    return k->r ? -r : r;
}
static int cmp(const void *pa, const void *pb) {
    const struct ln *a = pa, *b = pb;
    int r = 0;
    for (int i = 0; i < nkeys && !r; i++) r = cmpkey(a->s, b->s, &keys[i]);
    if (r) return r;
    if (uniq || stable) return a->idx < b->idx ? -1 : a->idx > b->idx;
    r = strcmp(a->s, b->s);                                  /* last resort: the whole line, reversed by -r */
    return glob.r ? -r : r;
}

static int parse_key(const char *s, struct key *k) {
    *k = glob;
    k->f1 = (int)strtol(s, (char **)&s, 10);
    if (k->f1 < 1) return 0;
    if (*s == '.') k->c1 = (int)strtol(s + 1, (char **)&s, 10);
    if (*s == ',') {
        k->f2 = (int)strtol(s + 1, (char **)&s, 10);
        if (*s == '.') k->c2 = (int)strtol(s + 1, (char **)&s, 10);
    }
    int mod = 0;
    for (; *s; s++) {
        if (!mod) { k->n = k->r = k->b = k->f = 0; mod = 1; }  /* ordering letters on a key replace the global ones */
        if (*s == 'n') k->n = 1; else if (*s == 'r') k->r = 1; else if (*s == 'b') k->b = 1; else if (*s == 'f') k->f = 1; else return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r') glob.r = 1;
            else if (*f == 'n') glob.n = 1;
            else if (*f == 'f') glob.f = 1;
            else if (*f == 'b') glob.b = 1;
            else if (*f == 'u') uniq = 1;
            else if (*f == 's') stable = 1;
            else if (*f == 't' || *f == 'k') {
                const char *v = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!v || (*f == 't' && strlen(v) != 1) || (*f == 'k' && (nkeys == 16 || !parse_key(v, &keys[nkeys++]))))
                    { fprintf(stderr, "sort: invalid argument for -%c\n", *f); return 2; }
                if (*f == 't') sep = (unsigned char)*v;
                break;
            } else { fprintf(stderr, "usage: sort [-rnufbs] [-t SEP] [-k KEY]... [file...]\n"); return 2; }
        }
    }
    for (int k = 0; k < nkeys; k++) if (!keys[k].n && !keys[k].r && !keys[k].b && !keys[k].f) { /* inherit globals set after the -k */
        keys[k].n = glob.n; keys[k].r = glob.r; keys[k].b = glob.b; keys[k].f = glob.f; }
    if (!nkeys) { keys[0] = glob; keys[0].f1 = 1; nkeys = 1; }
    struct ln *v = NULL;
    size_t n = 0;
    for (int k = i < argc ? i : argc; ; k++) {
        const char *name = i < argc ? argv[k] : "-";
        if (i < argc && k >= argc) break;
        size_t len;
        char *b = slurp("sort", name, &len);
        if (!b) rc = 2;
        else {
            for (char *p = b; p < b + len; ) {
                char *e = strchr(p, '\n');
                if (e) *e = 0;
                v = realloc(v, (n + 1) * sizeof *v);
                v[n].s = p; v[n].idx = n; n++;
                p = e ? e + 1 : b + len;
            }
        }
        if (i >= argc) break;
    }
    qsort(v, n, sizeof *v, cmp);
    for (size_t k = 0; k < n; k++) {
        if (uniq && k) {
            int r = 0;
            for (int j = 0; j < nkeys && !r; j++) r = cmpkey(v[k - 1].s, v[k].s, &keys[j]);
            if (!r) continue;
        }
        puts(v[k].s);
    }
    return rc;
}
