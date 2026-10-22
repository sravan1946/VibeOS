/* cut -b|-c LIST | -f LIST [-d DELIM] [-s] [file...]    LIST: N  N-M  N-  -M  comma separated */
#include <vutil.h>

static struct { long lo, hi; } rg[64];
static int nrg;

static int inlist(long i) {
    for (int k = 0; k < nrg; k++) if (i >= rg[k].lo && i <= rg[k].hi) return 1;
    return 0;
}

static int parse(const char *s) {
    while (*s) {
        if (nrg == 64) return -1;
        long lo = 1, hi = 1L << 30;
        char *e;
        if (*s != '-') { lo = strtol(s, &e, 10); if (e == s || lo < 1) return -1; s = e; hi = lo; }
        if (*s == '-') {
            s++;
            if (*s >= '0' && *s <= '9') { hi = strtol(s, &e, 10); s = e; } else hi = 1L << 30;
        } else if (s[-1] == '-') return -1;
        if (hi < lo) return -1;
        rg[nrg].lo = lo; rg[nrg++].hi = hi;
        if (*s == ',') s++; else if (*s) return -1;
    }
    return nrg ? 0 : -1;
}

int main(int argc, char **argv) {
    int mode = 0, delim = '\t', sflag = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 's') sflag = 1;
            else if (*f == 'd' || *f == 'f' || *f == 'c' || *f == 'b') {
                const char *a = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!a) goto usage;
                if (*f == 'd') delim = (unsigned char)a[0];
                else { mode = *f; if (parse(a)) { fprintf(stderr, "cut: invalid list '%s'\n", a); return 2; } }
                break;
            } else goto usage;
        }
    }
    if (!mode) goto usage;
    for (int k = i < argc ? i : argc; ; k++) {
        const char *name = i < argc ? argv[k] : "-";
        if (i < argc && k >= argc) break;
        size_t len;
        char *b = slurp("cut", name, &len);
        if (!b) rc = 1;
        else {
            for (size_t pos = 0; pos < len;) {
                char *e = memchr(b + pos, '\n', len - pos);
                size_t ll = e ? (size_t)(e - (b + pos)) : len - pos;
                char *ln = b + pos;
                if (mode == 'f') {
                    if (!memchr(ln, delim, ll)) { if (!sflag) { fwrite(ln, 1, ll, stdout); putchar('\n'); } }
                    else {
                        long fld = 1, first = 1;
                        size_t st = 0;
                        for (size_t q = 0; q <= ll; q++)
                            if (q == ll || ln[q] == delim) {
                                if (inlist(fld)) { if (!first) putchar(delim); fwrite(ln + st, 1, q - st, stdout); first = 0; }
                                fld++; st = q + 1;
                            }
                        putchar('\n');
                    }
                } else {
                    for (size_t q = 0; q < ll; q++) if (inlist(q + 1)) putchar((unsigned char)ln[q]);
                    putchar('\n');
                }
                pos += ll + 1;
            }
            free(b);
        }
        if (i >= argc) break;
    }
    return rc;
usage:
    fputs("usage: cut -b|-c LIST | -f LIST [-d DELIM] [-s] [file...]\n", stderr);
    return 2;
}
