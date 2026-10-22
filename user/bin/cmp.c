/* cmp [-s] [-l] file1 file2: first differing byte (exit 1), or with -l every difference (byte number, octal values) */
#include <vutil.h>

int main(int argc, char **argv) {
    int s = 0, l = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 's') s = 1; else if (*f == 'l') l = 1; else goto usage;
    if (argc - i != 2) goto usage;
    size_t n1, n2;
    char *a = slurp("cmp", argv[i], &n1), *b = slurp("cmp", argv[i + 1], &n2);
    if (!a || !b) return 2;
    size_t m = n1 < n2 ? n1 : n2;
    int diff = 0, w = 1;
    for (size_t t = m; t >= 10; t /= 10) w++;
    long line = 1;
    for (size_t k = 0; k < m; k++) {
        if (a[k] != b[k]) {
            diff = 1;
            if (s) return 1;
            if (!l) { printf("%s %s differ: byte %lu, line %ld\n", argv[i], argv[i + 1], (unsigned long)k + 1, line); return 1; }
            printf("%*lu %3o %3o\n", w, (unsigned long)k + 1, (unsigned char)a[k], (unsigned char)b[k]);
        }
        if (a[k] == '\n') line++;
    }
    if (n1 != n2) {
        if (!s) { fflush(stdout); {
            int eol = m && a[m - 1] == '\n';       /* GNU: "line N" after a whole line, "in line N" mid-line */
            fprintf(stderr, "cmp: EOF on %s after byte %lu, %sline %ld\n", n1 < n2 ? argv[i] : argv[i + 1], (unsigned long)m, eol ? "" : "in ", eol ? line - 1 : line);
        } }
        return 1;
    }
    return diff;
usage:
    fputs("usage: cmp [-s] [-l] file1 file2\n", stderr);
    return 2;
}
