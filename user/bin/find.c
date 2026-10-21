/* find [path...] [-name GLOB] [-type f|d] [-maxdepth N] */
#include <vutil.h>
#include <dirent.h>

static const char *name_pat;
static char type;
static int maxdepth = 1 << 30, rc;

static int glob_match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            for (;; s++) { if (glob_match(p + 1, s)) return 1; if (!*s) return 0; }
        }
        if (!*s) return 0;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1;
            int neg = *q == '!' || *q == '^', ok = 0;
            if (neg) q++;
            for (int first = 1; *q && (*q != ']' || first); q++, first = 0) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if (*s >= q[0] && *s <= q[2]) ok = 1; q += 2; }
                else if (*q == *s) ok = 1;
            }
            if (!*q) return 0;
            if (ok == neg) return 0;
            p = q;
            continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return 0;
    }
    return !*s;
}

static void walk(const char *path, int depth) {
    struct stat st;
    if (stat(path, &st) < 0) { fprintf(stderr, "find: '%s': %s\n", path, strerror(errno)); rc = 1; return; }
    int dir = S_ISDIR(st.st_mode);
    const char *b = base_name(path);
    if (!*b) b = path;
    if ((!name_pat || glob_match(name_pat, b)) && (!type || (type == 'd') == dir)) puts(path);
    if (!dir || depth >= maxdepth) return;
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "find: '%s': %s\n", path, strerror(errno)); rc = 1; return; }
    char **names = NULL;
    int n = 0;
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
        walk(full, depth + 1);
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv) {
    int i = 1, np = 0;
    const char *paths[32];
    for (; i < argc && argv[i][0] != '-' && np < 32; i++) paths[np++] = argv[i];
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-name") && i + 1 < argc) name_pat = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) {
            type = argv[++i][0];
            if (type != 'f' && type != 'd') { fprintf(stderr, "find: Unknown argument to -type: %c\n", type); return 1; }
        } else if (!strcmp(argv[i], "-maxdepth") && i + 1 < argc) maxdepth = atoi(argv[++i]);
        else { fprintf(stderr, "find: unknown predicate '%s'\nusage: find [path...] [-name GLOB] [-type f|d] [-maxdepth N]\n", argv[i]); return 1; }
    }
    if (!np) paths[np++] = ".";
    for (i = 0; i < np; i++) walk(paths[i], 0);
    return rc;
}
