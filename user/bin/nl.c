/* nl [-b a|t|n] [-n ln|rn|rz] [-w N] [-s SEP] [-v N] [-i N] [file...]   number lines (default: non-empty only) */
#include <vutil.h>

int main(int argc, char **argv) {
    int body = 't', width = 6, i = 1, rc = 0;
    const char *fmt = "rn", *sep = "\t";
    long n = 1, inc = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        const char *f = argv[i] + 1, *a = f[1] ? f + 1 : i + 1 < argc ? argv[i + 1] : NULL;
        if (!strchr("bnwsvi", *f) || !a) goto usage;
        if (!f[1]) i++;
        switch (*f) {
        case 'b': body = *a; if (!strchr("atn", body)) goto usage; break;
        case 'n': fmt = a; if (strcmp(a, "ln") && strcmp(a, "rn") && strcmp(a, "rz")) goto usage; break;
        case 'w': width = atoi(a); break;
        case 's': sep = a; break;
        case 'v': n = atol(a); break;
        case 'i': inc = atol(a); break;
        }
    }
    for (int k = i < argc ? i : argc; ; k++) {
        const char *name = i < argc ? argv[k] : "-";
        if (i < argc && k >= argc) break;
        size_t len;
        char *b = slurp("nl", name, &len);
        if (!b) rc = 1;
        else {
            for (size_t pos = 0; pos < len;) {
                char *e = memchr(b + pos, '\n', len - pos);
                size_t ll = e ? (size_t)(e - (b + pos)) : len - pos;
                if (body == 'a' || (body == 't' && ll)) {
                    if (fmt[0] == 'l') printf("%-*ld%s", width, n, sep);
                    else if (fmt[1] == 'z') printf("%0*ld%s", width, n, sep);
                    else printf("%*ld%s", width, n, sep);
                    n += inc;
                } else printf("%*s", width + (int)strlen(sep), "");
                fwrite(b + pos, 1, ll, stdout);
                putchar('\n');
                pos += ll + 1;
            }
            free(b);
        }
        if (i >= argc) break;
    }
    return rc;
usage:
    fputs("usage: nl [-b a|t|n] [-n ln|rn|rz] [-w N] [-s SEP] [-v N] [-i N] [file...]\n", stderr);
    return 1;
}
