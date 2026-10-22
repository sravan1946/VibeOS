/* join [-a N] [-v N] [-e STR] [-i] [-o LIST] [-t CHAR] [-1 F] [-2 F] file1 file2   both inputs sorted on the join field */
#include <vutil.h>
#include <ctype.h>

struct rec { char *key; char **f; int nf; };
struct file { struct rec *r; size_t n; };

static int sep = -1, icase, jf[2] = { 1, 1 }, show[2], only[2];
static const char *fill = "";
static struct { int file, field; } *ofmt;           /* -o: file 0 = the join field */
static int nofmt;

static void split(char *s, struct rec *r, int jfield) {
    r->f = NULL; r->nf = 0;
    for (;;) {
        char *e;
        if (sep >= 0) { e = s; while (*e && *e != sep) e++; }
        else { while (*s == ' ' || *s == '\t') s++; if (!*s) break; e = s; while (*e && *e != ' ' && *e != '\t') e++; }
        int last = !*e;
        r->f = realloc(r->f, (r->nf + 1) * sizeof *r->f);
        r->f[r->nf++] = s;
        if (!last) *e++ = 0;
        if (last) break;
        s = e;
    }
    r->key = jfield <= r->nf ? r->f[jfield - 1] : (char *)"";
}
static int load(const char *name, int jfield, struct file *fl) {
    size_t len;
    char *b = slurp("join", name, &len);
    if (!b) return 0;
    for (char *p = b; p < b + len; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        fl->r = realloc(fl->r, (fl->n + 1) * sizeof *fl->r);
        split(p, &fl->r[fl->n++], jfield);
        p = e ? e + 1 : b + len;
    }
    return 1;
}
static int kcmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (icase) { x = tolower(x); y = tolower(y); }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static void out(const char *s, int *first) {
    if (!*first) putchar(sep >= 0 ? sep : ' ');
    fputs(s, stdout);
    *first = 0;
}
static void emit(const struct rec *a, const struct rec *b) {     /* a or b may be NULL (unpairable) */
    int first = 1;
    const struct rec *x = a ? a : b;
    if (nofmt) {
        for (int i = 0; i < nofmt; i++) {
            const struct rec *r = ofmt[i].file == 1 ? a : ofmt[i].file == 2 ? b : x;
            int fld = ofmt[i].file ? ofmt[i].field : (a ? jf[0] : jf[1]);
            out(r && fld >= 1 && fld <= r->nf ? r->f[fld - 1] : fill, &first);
        }
    } else {
        out(x->key, &first);
        if (a) for (int i = 0; i < a->nf; i++) if (i != jf[0] - 1) out(a->f[i], &first);
        if (b) for (int i = 0; i < b->nf; i++) if (i != jf[1] - 1) out(b->f[i], &first);
    }
    putchar('\n');
}
static void parse_o(char *s) {
    for (char *t = strtok(s, ", \t"); t; t = strtok(NULL, ", \t")) {
        ofmt = realloc(ofmt, (nofmt + 1) * sizeof *ofmt);
        if (!strcmp(t, "0")) { ofmt[nofmt].file = 0; ofmt[nofmt++].field = 0; }
        else if ((t[0] == '1' || t[0] == '2') && t[1] == '.' && atoi(t + 2) > 0) { ofmt[nofmt].file = t[0] - '0'; ofmt[nofmt++].field = atoi(t + 2); }
        else { fprintf(stderr, "join: invalid field specifier: '%s'\n", t); exit(1); }
    }
}

int main(int argc, char **argv) {
    int i = 1;
    char *names[2];
    int nn = 0;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !a[1]) { if (nn < 2) names[nn++] = a; continue; }
        if (!strcmp(a, "--")) { for (i++; i < argc && nn < 2; i++) names[nn++] = argv[i]; break; }
        if (!strcmp(a, "-i")) { icase = 1; continue; }
        char o = a[1];
        const char *v = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : NULL;
        if (!v || !strchr("aveot12j", o)) { fprintf(stderr, "usage: join [-a N] [-v N] [-e STR] [-i] [-o LIST] [-t CHAR] [-1 F] [-2 F] file1 file2\n"); return 1; }
        if (o == 'a') show[atoi(v) == 2] = 1;
        else if (o == 'v') { show[atoi(v) == 2] = 1; only[atoi(v) == 2] = 1; }
        else if (o == 'e') fill = v;
        else if (o == 'o') parse_o(strdup(v));
        else if (o == 't') sep = (unsigned char)*v;
        else if (o == '1') jf[0] = atoi(v);
        else if (o == '2') jf[1] = atoi(v);
        else jf[0] = jf[1] = atoi(v);
        if (jf[0] < 1 || jf[1] < 1) { fprintf(stderr, "join: invalid field number: '%s'\n", v); return 1; }
    }
    if (nn != 2) { fprintf(stderr, "join: need two files\n"); return 1; }
    struct file f1 = {0}, f2 = {0};
    if (!load(names[0], jf[0], &f1) || !load(names[1], jf[1], &f2)) return 1;
    size_t p = 0, q = 0;
    int paired = !only[0] && !only[1];
    while (p < f1.n && q < f2.n) {
        int c = kcmp(f1.r[p].key, f2.r[q].key);
        if (c < 0) { if (show[0]) emit(&f1.r[p], NULL); p++; }
        else if (c > 0) { if (show[1]) emit(NULL, &f2.r[q]); q++; }
        else {
            size_t p2 = p, q2 = q;
            while (p2 < f1.n && !kcmp(f1.r[p2].key, f1.r[p].key)) p2++;
            while (q2 < f2.n && !kcmp(f2.r[q2].key, f2.r[q].key)) q2++;
            if (paired) for (size_t a = p; a < p2; a++) for (size_t b = q; b < q2; b++) emit(&f1.r[a], &f2.r[b]);
            p = p2; q = q2;
        }
    }
    for (; p < f1.n; p++) if (show[0]) emit(&f1.r[p], NULL);
    for (; q < f2.n; q++) if (show[1]) emit(NULL, &f2.r[q]);
    return 0;
}
