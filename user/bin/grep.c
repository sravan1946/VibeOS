/* grep [-i] [-v] [-n] [-c] [-r] [-l] [-H|-h] [-e PAT] PATTERN [file...]
 * regex subset: . * + ? ^ $ [set] [^set] [a-z] \x   (backtracking matcher, no groups/alternation) */
#include <vutil.h>
#include <ctype.h>
#include <dirent.h>

static int ci, inv, num, cnt, rec, listf, with_name = -1;
static const char *pat;

static int eq(int a, int b) { return ci ? tolower(a) == tolower(b) : a == b; }

static int alen(const char *re) {                  /* length of the atom at re */
    if (re[0] == '\\' && re[1]) return 2;
    if (re[0] == '[') {
        const char *p = re + 1;
        if (*p == '^') p++;
        if (*p == ']') p++;
        while (*p && *p != ']') p++;
        if (*p) return p - re + 1;
    }
    return 1;
}

static int amatch(const char *re, int n, int c) {
    if (re[0] == '.') return 1;
    if (re[0] == '\\' && n == 2) return eq(re[1], c);
    if (re[0] == '[' && n > 1) {
        const char *p = re + 1, *end = re + n - 1;
        int neg = *p == '^', ok = 0;
        if (neg) p++;
        for (; p < end; p++) {
            if (p + 2 < end && p[1] == '-') {
                if ((c >= p[0] && c <= p[2]) || (ci && ((tolower(c) >= p[0] && tolower(c) <= p[2]) ||
                                                        (toupper(c) >= p[0] && toupper(c) <= p[2])))) ok = 1;
                p += 2;
            } else if (eq(*p, c)) ok = 1;
        }
        return ok != neg;
    }
    return eq(re[0], c);
}

static int mhere(const char *re, const char *t) {
    if (!*re) return 1;
    if (re[0] == '$' && !re[1]) return !*t;
    int n = alen(re);
    char q = re[n];
    if (q == '*' || q == '+' || q == '?') {
        int min = q == '+', max = q == '?' ? 1 : 1 << 30, k = 0;
        while (k < max && t[k] && amatch(re, n, t[k])) k++;
        for (; k >= min; k--) if (mhere(re + n + 1, t + k)) return 1;
        return 0;
    }
    return *t && amatch(re, n, *t) && mhere(re + n, t + 1);
}

static int match(const char *re, const char *t) {
    if (*re == '^') return mhere(re + 1, t);
    do if (mhere(re, t)) return 1; while (*t++);
    return 0;
}

static int grep_buf(const char *name, char *b, size_t len) {
    int hits = 0, lineno = 0;
    for (size_t pos = 0; pos < len;) {
        char *e = memchr(b + pos, '\n', len - pos);
        size_t ll = e ? (size_t)(e - (b + pos)) : len - pos;
        char *line = b + pos, save = line[ll];
        line[ll] = 0;
        lineno++;
        if (match(pat, line) != inv) {
            hits++;
            if (listf) { line[ll] = save; break; }
            if (!cnt) {
                if (with_name) printf("%s:", name);
                if (num) printf("%d:", lineno);
                puts(line);
            }
        }
        line[ll] = save;
        pos += ll + 1;
    }
    if (listf && hits) puts(name);
    if (cnt && !listf) { if (with_name) printf("%s:", name); printf("%d\n", hits); }
    return hits;
}

static int do_path(const char *path, int top, int *err) {
    struct stat st;
    if (stat(path, &st) < 0) { WARN("grep", path); *err = 1; return 0; }
    if (S_ISDIR(st.st_mode)) {
        if (!rec) { fprintf(stderr, "grep: %s: Is a directory\n", path); *err = 1; return 0; }
        DIR *d = opendir(path);
        if (!d) { WARN("grep", path); *err = 1; return 0; }
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

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            switch (*f) {
            case 'i': ci = 1; break;
            case 'v': inv = 1; break;
            case 'n': num = 1; break;
            case 'c': cnt = 1; break;
            case 'r': case 'R': rec = 1; break;
            case 'l': listf = 1; break;
            case 'H': with_name = 1; break;
            case 'h': with_name = 0; break;
            case 'E': case 'G': break;
            case 'e': if (i + 1 < argc) pat = argv[++i]; break;
            default: fprintf(stderr, "usage: grep [-ivncrlHh] [-e PAT] PATTERN [file...]\n"); return 2;
            }
    }
    if (!pat) {
        if (i >= argc) { fprintf(stderr, "usage: grep [-ivncrlHh] [-e PAT] PATTERN [file...]\n"); return 2; }
        pat = argv[i++];
    }
    int err = 0, hits = 0, nfiles = argc - i;
    if (with_name < 0) with_name = nfiles > 1 || rec;
    if (nfiles == 0 && !rec) {
        size_t len;
        char *b = readall(0, &len);
        if (!b) { WARN("grep", "stdin"); return 2; }
        hits = grep_buf("(standard input)", b, len);
    } else if (nfiles == 0) hits = do_path(".", 1, &err);
    else for (; i < argc; i++) hits += do_path(argv[i], 1, &err);
    return err ? 2 : hits ? 0 : 1;
}
