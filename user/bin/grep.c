/* grep [-EFGivncqlLrRowxsHh] [-A N] [-B N] [-C N] [-e PAT]... PATTERN [file...]
 * patterns are POSIX BRE (default), ERE (-E) or fixed (-F), via libc regex.h */
#include <vutil.h>
#include <regex.h>
#include <dirent.h>

static int ci, inv, num, cnt, rec, listf, quiet, only, word, whole, silent, ere, fixed, with_name = -1;
static int after, before, anyout;
static regex_t *res;
static int nre;

static int isw(int c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* leftmost-longest match of any pattern in ln[from..len); 1 if found */
static int find(const char *ln, int len, int from, int *so, int *eo) {
    int bs = -1, be = -1;
    for (int i = 0; i < nre; i++)
        for (int st = from; st <= len;) {
            regmatch_t m[1] = { { st, len } };
            if (regexec(&res[i], ln, 1, m, REG_STARTEND)) break;
            if (word && !((m[0].rm_so == 0 || !isw((unsigned char)ln[m[0].rm_so - 1])) &&
                          (m[0].rm_eo == len || !isw((unsigned char)ln[m[0].rm_eo])))) { st = m[0].rm_so + 1; continue; }
            if (whole && !(m[0].rm_so == 0 && m[0].rm_eo == len)) { st = m[0].rm_so + 1; if (m[0].rm_so) break; continue; }
            if (bs < 0 || m[0].rm_so < bs || (m[0].rm_so == bs && m[0].rm_eo > be)) { bs = m[0].rm_so; be = m[0].rm_eo; }
            break;
        }
    *so = bs; *eo = be;
    return bs >= 0;
}

static void prefix(const char *name, int ln, char sep) {
    if (with_name) printf("%s%c", name, sep);
    if (num) printf("%d%c", ln, sep);
}

static int grep_buf(const char *name, char *b, size_t len) {
    int hits = 0, nl = 0, cap = 0, last = -1, aft = 0;
    char **ls = NULL;
    for (size_t pos = 0; pos < len;) {          /* split in place: every line becomes a C string */
        char *e = memchr(b + pos, '\n', len - pos);
        if (nl == cap) ls = realloc(ls, (cap = cap ? cap * 2 : 256) * sizeof *ls);
        ls[nl++] = b + pos;
        if (e) { *e = 0; pos = e - b + 1; } else { b[len] = 0; pos = len + 1; }
    }
    for (int i = 0; i < nl; i++) {
        int so, eo, l = strlen(ls[i]);
        if ((find(ls[i], l, 0, &so, &eo)) != inv) {
            hits++;
            if (quiet) break;
            if (listf) break;
            if (cnt) continue;
            int from = i - before < last + 1 ? last + 1 : i - before;
            if ((before || after) && anyout && from > last + 1) puts("--");
            else if ((before || after) && anyout && last < 0) puts("--");
            for (int k = from; k < i; k++) { prefix(name, k + 1, '-'); puts(ls[k]); }
            if (only) {
                if (!inv) for (int st = 0; st <= l && find(ls[i], l, st, &so, &eo); st = eo > so ? eo : so + 1)
                    if (eo > so) { prefix(name, i + 1, ':'); fwrite(ls[i] + so, 1, eo - so, stdout); putchar('\n'); }
            } else { prefix(name, i + 1, ':'); puts(ls[i]); }
            anyout = 1; last = i; aft = after;
        } else if (aft > 0 && !cnt && !listf && !quiet) {
            aft--; prefix(name, i + 1, '-'); puts(ls[i]); last = i;
        }
    }
    free(ls);
    if (quiet) return hits;
    if (listf == 1 && hits) puts(name);
    if (listf == 2 && !hits) puts(name);
    if (cnt && !listf) { if (with_name) printf("%s:", name); printf("%d\n", hits); }
    return hits;
}

static int do_path(const char *path, int top, int *err) {
    struct stat st;
    if (stat(path, &st) < 0) { if (!silent) WARN("grep", path); *err = 1; return 0; }
    if (S_ISDIR(st.st_mode)) {
        if (!rec) { if (!silent) fprintf(stderr, "grep: %s: Is a directory\n", path); *err = 1; return 0; }
        DIR *d = opendir(path);
        if (!d) { if (!silent) WARN("grep", path); *err = 1; return 0; }
        char **names = NULL;
        int n = 0, hits = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            names = realloc(names, (n + 1) * sizeof *names);
            names[n++] = strdup(de->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char full[PATH_MAX];
            snprintf(full, sizeof full, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", names[i]);
            hits += do_path(full, 0, err);
            free(names[i]);
        }
        free(names);
        return hits;
    }
    if (S_ISCHR(st.st_mode) && !top) return 0;
    size_t len;
    char *b = slurp("grep", path, &len);
    if (!b) { *err = 1; return 0; }
    int h = grep_buf(path, b, len);
    free(b);
    return h;
}

/* add every newline-separated pattern in p; -F patterns are escaped into a BRE */
static int addpat(const char *p) {
    for (;;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *q = malloc(n * 2 + 1), *o = q;
        for (size_t i = 0; i < n; i++) {
            if (fixed && strchr("\\.[]*^$", p[i])) *o++ = '\\';
            *o++ = p[i];
        }
        *o = 0;
        res = realloc(res, (nre + 1) * sizeof *res);
        int r = regcomp(&res[nre], q, (ere && !fixed ? REG_EXTENDED : 0) | (ci ? REG_ICASE : 0));
        if (r) { char m[128]; regerror(r, &res[nre], m, sizeof m); fprintf(stderr, "grep: %s: %s\n", q, m); return -1; }
        nre++;
        free(q);
        if (!e) return 0;
        p = e + 1;
    }
}

static const char USAGE[] = "usage: grep [-EFivncqlLrowxsHh] [-A N] [-B N] [-C N] [-e PAT] PATTERN [file...]\n";

int main(int argc, char **argv) {
    int i = 1, ne = 0, nop = 0, endopt = 0;
    const char *pats[64];
    char **op = malloc((argc + 1) * sizeof *op);          /* options may follow the operands, like GNU grep */
    for (; i < argc; i++) {
        if (endopt || argv[i][0] != '-' || !argv[i][1]) { op[nop++] = argv[i]; continue; }
        if (!strcmp(argv[i], "--")) { endopt = 1; continue; }
        for (const char *f = argv[i] + 1; *f; f++)
            switch (*f) {
            case 'i': case 'y': ci = 1; break;
            case 'v': inv = 1; break;
            case 'n': num = 1; break;
            case 'c': cnt = 1; break;
            case 'r': case 'R': rec = 1; break;
            case 'l': listf = 1; break;
            case 'L': listf = 2; break;
            case 'q': quiet = 1; break;
            case 'o': only = 1; break;
            case 'w': word = 1; break;
            case 'x': whole = 1; break;
            case 's': silent = 1; break;
            case 'H': with_name = 1; break;
            case 'h': with_name = 0; break;
            case 'E': ere = 1; break;
            case 'F': fixed = 1; break;
            case 'G': ere = fixed = 0; break;
            case 'e': case 'A': case 'B': case 'C': {
                const char *a = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
                if (!a) { fputs(USAGE, stderr); return 2; }
                if (*f == 'e') { if (ne < 64) pats[ne++] = a; }
                else if (*f == 'A') after = atoi(a);
                else if (*f == 'B') before = atoi(a);
                else after = before = atoi(a);
                f = " ";
                break;
            }
            default:
                if (*f >= '0' && *f <= '9') { after = before = atoi(f); f = " "; break; }
                fputs(USAGE, stderr); return 2;
            }
    }
    i = 0;
    if (!ne) {
        if (!nop) { fputs(USAGE, stderr); return 2; }
        pats[ne++] = op[i++];
    }
    for (int k = 0; k < ne; k++) if (addpat(pats[k])) return 2;
    int err = 0, hits = 0, nfiles = nop - i;
    if (with_name < 0) with_name = nfiles > 1 || rec;
    if (nfiles == 0 && !rec) {
        size_t len;
        char *b = readall(0, &len);
        if (!b) { WARN("grep", "stdin"); return 2; }
        hits = grep_buf("(standard input)", b, len);
    } else if (nfiles == 0) hits = do_path(".", 1, &err);
    else for (; i < nop; i++) { hits += do_path(op[i], 1, &err); if (quiet && hits) break; }
    return quiet && hits ? 0 : err ? 2 : hits ? 0 : 1;
}
