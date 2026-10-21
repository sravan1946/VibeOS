/* mkdir [-p] dir... */
#include <vutil.h>

static int mk(const char *path, int parents) {
    if (!parents) {
        if (mkdir(path, 0755) < 0) { fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", path, strerror(errno)); return 1; }
        return 0;
    }
    char buf[PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; ; p++) {
        if (*p && *p != '/') continue;
        char c = *p;
        *p = 0;
        if (mkdir(buf, 0755) < 0 && errno != EEXIST) { fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", buf, strerror(errno)); return 1; }
        if (errno == EEXIST && !is_dir(buf)) { fprintf(stderr, "mkdir: cannot create directory '%s': Not a directory\n", buf); return 1; }
        if (!c) return 0;
        *p = c;
    }
}

int main(int argc, char **argv) {
    int i = 1, p = 0, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'p') p = 1;
            else { fprintf(stderr, "usage: mkdir [-p] dir...\n"); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "mkdir: missing operand\n"); return 1; }
    for (; i < argc; i++) rc |= mk(argv[i], p);
    return rc;
}
