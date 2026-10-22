/* tr [-c] [-d] [-s] SET1 [SET2]   sets: a-z  [:alpha:]  \n \t \\ \NNN  (SET2 is padded with its last char) */
#include <vutil.h>

static int set[2][512], len[2];

static int cls(const char *nm, int n, int c) {
    static const char *names[] = { "alpha", "digit", "alnum", "upper", "lower", "space", "blank", "punct", "print", "graph", "cntrl", "xdigit" };
    int k = 0;
    for (; k < 12; k++) if (strlen(names[k]) == (size_t)n && !memcmp(names[k], nm, n)) break;
    int al = (c | 32) >= 'a' && (c | 32) <= 'z', dg = c >= '0' && c <= '9';
    switch (k) {
    case 0: return al;
    case 1: return dg;
    case 2: return al || dg;
    case 3: return c >= 'A' && c <= 'Z';
    case 4: return c >= 'a' && c <= 'z';
    case 5: return c == ' ' || (c >= 9 && c <= 13);
    case 6: return c == ' ' || c == '\t';
    case 7: return c > 32 && c < 127 && !al && !dg;
    case 8: return c >= 32 && c < 127;
    case 9: return c > 32 && c < 127;
    case 10: return c < 32 || c == 127;
    case 11: return dg || ((c | 32) >= 'a' && (c | 32) <= 'f');
    }
    return -1;
}

/* one (possibly escaped) char; advances *p */
static int chr(const char **p) {
    const char *s = *p;
    if (*s != '\\' || !s[1]) { *p = s + 1; return (unsigned char)*s; }
    s++;
    int c = *s++;
    if (c >= '0' && c <= '7') {
        int v = c - '0', n = 1;
        while (n < 3 && *s >= '0' && *s <= '7') { v = v * 8 + *s++ - '0'; n++; }
        c = v & 255;
    } else if (c == 'n') c = '\n';
    else if (c == 't') c = '\t';
    else if (c == 'r') c = '\r';
    else if (c == 'a') c = 7;
    else if (c == 'b') c = 8;
    else if (c == 'f') c = 12;
    else if (c == 'v') c = 11;
    *p = s;
    return c;
}

static void expand(int k, const char *s) {
    int n = 0;
    while (*s) {
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (e) {
                for (int c = 0; c < 256; c++) { int r = cls(s + 2, e - (s + 2), c); if (r < 0) { fprintf(stderr, "tr: invalid character class\n"); exit(2); } if (r) set[k][n++] = c; }
                s = e + 2;
                continue;
            }
        }
        int a = chr(&s);
        if (*s == '-' && s[1]) {
            s++;
            int b = chr(&s);
            if (b < a) { fprintf(stderr, "tr: range-endpoints of '%c-%c' are in reverse collating sequence order\n", a, b); exit(2); }
            for (int c = a; c <= b; c++) set[k][n++] = c;
        } else set[k][n++] = a;
        if (n > 500) { fprintf(stderr, "tr: set too long\n"); exit(2); }
    }
    len[k] = n;
}

int main(int argc, char **argv) {
    int comp = 0, del = 0, sq = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'c' || *f == 'C') comp = 1;
            else if (*f == 'd') del = 1;
            else if (*f == 's') sq = 1;
            else goto usage;
    }
    int ns = argc - i;
    if (ns < 1 || ns > 2 || (!del && !sq && ns < 2) || (del && !sq && ns > 1)) goto usage;
    expand(0, argv[i]);
    if (ns > 1) expand(1, argv[i + 1]);
    if (comp) {                                  /* SET1 := every byte not in it, ascending */
        char in[256] = { 0 };
        for (int k = 0; k < len[0]; k++) in[set[0][k]] = 1;
        len[0] = 0;
        for (int c = 0; c < 256; c++) if (!in[c]) set[0][len[0]++] = c;
    }
    unsigned char map[256], in1[256] = { 0 }, insq[256] = { 0 };
    for (int c = 0; c < 256; c++) map[c] = c;
    for (int k = 0; k < len[0]; k++) in1[set[0][k]] = 1;
    int xlate = ns > 1 && !del;
    if (xlate) {
        if (!len[1]) { fprintf(stderr, "tr: empty SET2\n"); return 2; }
        for (int k = 0; k < len[0]; k++) map[set[0][k]] = set[1][k < len[1] ? k : len[1] - 1];
    }
    if (sq) {
        int s = ns > 1 ? 1 : 0;
        if (s && comp && del) s = 1;
        for (int k = 0; k < len[s]; k++) insq[set[s][k]] = 1;
    }
    unsigned char ib[4096], ob[4096];
    int on = 0, prev = -1;
    ssize_t n;
    while ((n = read(0, ib, sizeof ib)) > 0) {
        for (ssize_t k = 0; k < n; k++) {
            int c = ib[k];
            if (del && in1[c]) continue;
            if (xlate) c = map[c];
            if (sq && insq[c] && c == prev) continue;
            prev = c;
            ob[on++] = c;
            if (on == sizeof ob) { write_all(1, (char *)ob, on); on = 0; }
        }
    }
    if (on) write_all(1, (char *)ob, on);
    return n < 0;
usage:
    fputs("usage: tr [-cds] SET1 [SET2]\n", stderr);
    return 2;
}
