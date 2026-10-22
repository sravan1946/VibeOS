/* paste [-s] [-d LIST] file...   merge corresponding lines (or -s: each file onto one line); "-" is stdin */
#include <vutil.h>

int main(int argc, char **argv) {
    int ser = 0, i = 1, rc = 0;
    const char *dl = "\t";
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 's') ser = 1;
            else if (*f == 'd') { dl = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL; if (!dl) goto usage; break; }
            else goto usage;
        }
    }
    int nf = argc - i, nd = strlen(dl);
    if (!nd) goto usage;
    if (nf < 1) { argv[--i] = "-"; nf = 1; }              /* no files: stdin */
    char **buf = calloc(nf, sizeof *buf);
    size_t *len = calloc(nf, sizeof *len), *pos = calloc(nf, sizeof *pos);
    for (int k = 0; k < nf; k++) { buf[k] = slurp("paste", argv[i + k], &len[k]); if (!buf[k]) { rc = 1; buf[k] = strdup(""); } }
    /* "\0" in a delimiter list means empty; keep it simple: only literal characters and \t \n \\ */
    char dd[64];
    int m = 0;
    for (int k = 0; dl[k] && m < 63; k++) {
        if (dl[k] == '\\' && dl[k + 1]) { k++; dd[m++] = dl[k] == 't' ? '\t' : dl[k] == 'n' ? '\n' : dl[k] == '0' ? 0 : dl[k]; }
        else dd[m++] = dl[k];
    }
    for (int k = 0; k < nf; k++) {
        if (!ser) break;
        int j = 0;
        for (size_t p = 0; p < len[k];) {
            char *e = memchr(buf[k] + p, '\n', len[k] - p);
            size_t ll = e ? (size_t)(e - (buf[k] + p)) : len[k] - p;
            if (j) { if (dd[(j - 1) % m]) putchar(dd[(j - 1) % m]); }
            fwrite(buf[k] + p, 1, ll, stdout);
            j++; p += ll + 1;
        }
        putchar('\n');
    }
    for (int more = !ser; more;) {
        more = 0;
        for (int k = 0; k < nf; k++) if (pos[k] < len[k]) more = 1;
        if (!more) break;
        for (int k = 0; k < nf; k++) {
            if (pos[k] < len[k]) {
                char *e = memchr(buf[k] + pos[k], '\n', len[k] - pos[k]);
                size_t ll = e ? (size_t)(e - (buf[k] + pos[k])) : len[k] - pos[k];
                fwrite(buf[k] + pos[k], 1, ll, stdout);
                pos[k] += ll + 1;
            }
            if (k < nf - 1) { if (dd[k % m]) putchar(dd[k % m]); }
        }
        putchar('\n');
    }
    return rc;
usage:
    fputs("usage: paste [-s] [-d LIST] file...\n", stderr);
    return 1;
}
