/* diff [-u|-U N] [-q] [-i] [-w] [-b] file1 file2     Myers O(ND) line diff; normal output, or unified with -u.
 * A directory operand means "the file of that name in it". Exit 0 same, 1 differ, 2 trouble. */
#include <vutil.h>
#include <ctype.h>

typedef struct { char *s; int len; unsigned h; char *key; int klen, noeol; } Line;
typedef struct { Line *l; int n, noeol; const char *name; char *buf; } File;

static int fi, fw, fb;

static void load(File *f, const char *name) {
    size_t len;
    f->name = name;
    f->buf = slurp("diff", name, &len);
    if (!f->buf) exit(2);
    int cap = 0;
    f->l = NULL; f->n = 0;
    f->noeol = len && f->buf[len - 1] != '\n';
    for (size_t p = 0; p < len;) {
        char *e = memchr(f->buf + p, '\n', len - p);
        int ll = e ? (int)(e - (f->buf + p)) : (int)(len - p);
        if (f->n == cap) f->l = realloc(f->l, (cap = cap ? cap * 2 : 256) * sizeof *f->l);
        Line *L = &f->l[f->n++];
        L->s = f->buf + p; L->len = ll;
        L->key = L->s; L->klen = ll;
        L->noeol = !e;                           /* a last line without newline differs from one with */
        if (fi || fw || fb) {                    /* comparison key: case / whitespace folded copy */
            char *k = malloc(ll + 1);
            int m = 0, sp = 0;
            for (int i = 0; i < ll; i++) {
                int c = (unsigned char)L->s[i];
                if (fw && isspace(c)) continue;
                if (fb && isspace(c)) { sp = 1; continue; }
                if (sp) { k[m++] = ' '; sp = 0; }
                k[m++] = fi ? tolower(c) : c;
            }
            k[m] = 0; L->key = k; L->klen = m;
        }
        unsigned h = 5381;
        for (int i = 0; i < L->klen; i++) h = h * 33 + (unsigned char)L->key[i];
        L->h = h;
        p += ll + 1;
    }
}

static int eq(const Line *a, const Line *b) {
    return a->h == b->h && a->noeol == b->noeol && a->klen == b->klen && !memcmp(a->key, b->key, a->klen);
}

/* edit script: ops[k] = 0 keep, 1 delete (from a), 2 insert (from b) */
static char *ops;
static int nops;

static void myers(File *a, File *b) {
    int n = a->n, m = b->n, max = n + m, off = max + 1, d, k, x, y;
    int **trace = malloc((max + 2) * sizeof *trace);
    int *v = calloc(2 * max + 3, sizeof *v);
    int found = -1;
    if (!max) { ops = malloc(1); nops = 0; return; }
    for (d = 0; d <= max && found < 0; d++) {
        trace[d] = malloc((2 * d + 3) * sizeof(int));
        for (k = -d; k <= d; k += 2) {
            if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) x = v[off + k + 1];
            else x = v[off + k - 1] + 1;
            y = x - k;
            while (x < n && y < m && eq(&a->l[x], &b->l[y])) x++, y++;
            v[off + k] = x;
            if (x >= n && y >= m) { found = d; break; }
        }
        /* snapshot of v for k in [-d-1, d+1] as seen at the start of the next round */
        for (k = -d - 1; k <= d + 1; k++) trace[d][k + d + 1] = v[off + k];
    }
    d = found;
    ops = malloc(n + m + 1);
    nops = 0;
    x = n; y = m;
    for (; d > 0; d--) {
        int *pv = trace[d - 1];                 /* v after round d-1, index k + (d-1) + 1 */
        k = x - y;
        int pk, base = d;                       /* pv[k + base] with base = (d-1)+1 */
        if (k == -d || (k != d && pv[k - 1 + base] < pv[k + 1 + base])) pk = k + 1; else pk = k - 1;
        int px = pv[pk + base], py = px - pk;
        while (x > px && y > py) { ops[nops++] = 0; x--; y--; }
        ops[nops++] = x == px ? 2 : 1;           /* moved down = insert, right = delete */
        x = px; y = py;
    }
    while (x > 0 && y > 0) { ops[nops++] = 0; x--; y--; }
    for (int i = 0; i < nops / 2; i++) { char t = ops[i]; ops[i] = ops[nops - 1 - i]; ops[nops - 1 - i] = t; }
}

static void pline(char pre, File *f, int i) {
    putchar(pre);
    if (pre == ' ' || pre == '+' || pre == '-') {} else putchar(' ');
    fwrite(f->l[i].s, 1, f->l[i].len, stdout);
    putchar('\n');
    if (f->noeol && i == f->n - 1) puts("\\ No newline at end of file");
}

static void range(int lo, int hi) { if (hi - lo <= 1 ? 1 : 0) printf("%d", lo); else printf("%d,%d", lo, hi - 1); }   /* lines lo..hi-1 */

static void normal(File *a, File *b) {
    int ia = 0, ib = 0;
    for (int k = 0; k < nops;) {
        if (!ops[k]) { ia++; ib++; k++; continue; }
        int a0 = ia, b0 = ib;
        while (k < nops && ops[k]) { if (ops[k] == 1) ia++; else ib++; k++; }
        if (a0 == ia) { printf("%da", a0); range(b0 + 1, ib + 1); }
        else if (b0 == ib) { range(a0 + 1, ia + 1); printf("d%d", b0); }
        else { range(a0 + 1, ia + 1); putchar('c'); range(b0 + 1, ib + 1); }
        putchar('\n');
        for (int i = a0; i < ia; i++) pline('<', a, i);
        if (a0 != ia && b0 != ib) puts("---");
        for (int i = b0; i < ib; i++) pline('>', b, i);
    }
}

static void uhdr(int s, int len) { if (len == 1) printf("%d", s); else printf("%d,%d", len ? s : s - 1, len); }

static void unified(File *a, File *b, int ctx) {
    /* positions of every op: pa[k], pb[k] = lines consumed before op k */
    int *pa = malloc((nops + 1) * sizeof *pa), *pb = malloc((nops + 1) * sizeof *pb);
    int x = 0, y = 0, hdr = 0;
    for (int k = 0; k < nops; k++) { pa[k] = x; pb[k] = y; if (ops[k] != 2) x++; if (ops[k] != 1) y++; }
    pa[nops] = x; pb[nops] = y;
    for (int k = 0; k < nops;) {
        if (!ops[k]) { k++; continue; }
        /* hunk starts ctx ops of equal lines before k and extends while the next change is within 2*ctx */
        int st = k, en;
        for (int back = 0; st > 0 && !ops[st - 1] && back < ctx; back++) st--;
        en = k;
        for (;;) {
            while (en < nops && ops[en]) en++;
            int j = en, eq_ = 0;
            while (j < nops && !ops[j]) { j++; eq_++; }
            if (j < nops && eq_ <= 2 * ctx) { en = j; continue; }
            en = en + (eq_ < ctx ? eq_ : ctx);
            break;
        }
        if (!hdr) { printf("--- %s\n+++ %s\n", a->name, b->name); hdr = 1; }
        printf("@@ -"); uhdr(pa[st] + 1, pa[en] - pa[st]);
        printf(" +"); uhdr(pb[st] + 1, pb[en] - pb[st]);
        puts(" @@");
        for (int q = st; q < en; q++) {
            if (!ops[q]) pline(' ', a, pa[q]);
            else if (ops[q] == 1) pline('-', a, pa[q]);
            else pline('+', b, pb[q]);
        }
        k = en;
    }
}

static char *joinpath(const char *dir, const char *f) {
    char *r = malloc(strlen(dir) + strlen(f) + 2);
    sprintf(r, "%s/%s", dir, f);
    return r;
}

int main(int argc, char **argv) {
    int uni = 0, ctx = 3, quiet = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            switch (*f) {
            case 'u': uni = 1; break;
            case 'U': uni = 1; ctx = atoi(f[1] ? f + 1 : i + 1 < argc ? argv[++i] : "3"); f = " "; break;
            case 'q': quiet = 1; break;
            case 'i': fi = 1; break;
            case 'w': fw = 1; break;
            case 'b': fb = 1; break;
            default: goto usage;
            }
    }
    if (argc - i != 2) goto usage;
    const char *p1 = argv[i], *p2 = argv[i + 1];
    if (is_dir(p1) && !is_dir(p2)) p1 = joinpath(p1, base_name(p2));
    else if (is_dir(p2) && !is_dir(p1)) p2 = joinpath(p2, base_name(p1));
    else if (is_dir(p1) && is_dir(p2)) { fprintf(stderr, "diff: %s and %s are directories\n", p1, p2); return 2; }
    File a, b;
    load(&a, p1); load(&b, p2);
    myers(&a, &b);
    int differ = 0;
    for (int k = 0; k < nops; k++) if (ops[k]) { differ = 1; break; }
    if (!differ) return 0;
    if (quiet) { printf("Files %s and %s differ\n", p1, p2); return 1; }
    if (uni) unified(&a, &b, ctx); else normal(&a, &b);
    return 1;
usage:
    fputs("usage: diff [-u|-U N] [-q] [-i] [-w] [-b] file1 file2\n", stderr);
    return 2;
}
