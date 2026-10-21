/* sort [-r] [-n] [-u] [file...] */
#include <vutil.h>

static int rev, numeric, uniq;

static int cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int r;
    if (numeric) {
        long p = strtol(x, NULL, 10), q = strtol(y, NULL, 10);
        r = p < q ? -1 : p > q;
        if (!r && !uniq) r = strcmp(x, y);
    } else r = strcmp(x, y);
    return rev ? -r : r;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'r') rev = 1;
            else if (*f == 'n') numeric = 1;
            else if (*f == 'u') uniq = 1;
            else { fprintf(stderr, "usage: sort [-rnu] [file...]\n"); return 2; }
    }
    char **v = NULL;
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
                v[n++] = p;
                p = e ? e + 1 : b + len;
            }
        }
        if (i >= argc) break;
    }
    qsort(v, n, sizeof *v, cmp);
    for (size_t k = 0; k < n; k++) {
        if (uniq && k && cmp(&v[k - 1], &v[k]) == 0) continue;
        puts(v[k]);
    }
    return rc;
}
