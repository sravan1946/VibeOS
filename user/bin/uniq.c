/* uniq [-c] [-d] [-u] [file] : collapse adjacent duplicate lines */
#include <vutil.h>

static int count, dups, only;

static void emit(const char *line, int n) {
    if ((dups && n < 2) || (only && n > 1)) return;
    if (count) printf("%7d ", n);
    puts(line);
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'c') count = 1;
            else if (*f == 'd') dups = 1;
            else if (*f == 'u') only = 1;
            else { fprintf(stderr, "usage: uniq [-cdu] [file]\n"); return 2; }
    size_t len;
    char *b = slurp("uniq", i < argc ? argv[i] : "-", &len);
    if (!b) return 1;
    char *prev = NULL;
    int n = 0;
    for (char *p = b; p < b + len; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (prev && !strcmp(prev, p)) n++;
        else { if (prev) emit(prev, n); prev = p; n = 1; }
        p = e ? e + 1 : b + len;
    }
    if (prev) emit(prev, n);
    return 0;
}
