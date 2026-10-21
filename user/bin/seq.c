/* seq [-s SEP] [-w] [FIRST [INCR]] LAST   (integers) */
#include <vutil.h>

int main(int argc, char **argv) {
    const char *sep = "\n";
    int w = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && (argv[i][1] < '0' || argv[i][1] > '9'); i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) sep = argv[++i];
        else if (!strcmp(argv[i], "-w")) w = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "usage: seq [-s SEP] [-w] [FIRST [INCR]] LAST\n"); return 1; }
    }
    int n = argc - i;
    if (n < 1 || n > 3) { fprintf(stderr, n ? "seq: extra operand '%s'\n" : "seq: missing operand\n", argv[argc - 1]); return 1; }
    long v[3], first = 1, inc = 1, last;
    for (int k = 0; k < n; k++) {
        char *e;
        v[k] = strtol(argv[i + k], &e, 10);
        if (*e || e == argv[i + k]) { fprintf(stderr, "seq: invalid number '%s'\n", argv[i + k]); return 1; }
    }
    last = v[n - 1];
    if (n > 1) first = v[0];
    if (n == 3) inc = v[1];
    if (inc == 0) { fprintf(stderr, "seq: invalid zero increment value: '0'\n"); return 1; }
    int width = 0;
    if (w) { char b[16]; int a = snprintf(b, sizeof b, "%ld", first), c = snprintf(b, sizeof b, "%ld", last); width = a > c ? a : c; }
    for (long v = first; inc > 0 ? v <= last : v >= last; v += inc) {
        if (v != first) fputs(sep, stdout);
        printf("%0*ld", width, v);
    }
    if (first == last || (inc > 0 ? first <= last : first >= last)) putchar('\n');
    return 0;
}
