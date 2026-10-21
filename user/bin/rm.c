/* rm [-r] [-f] file... */
#include <vutil.h>
#include <dirent.h>

static int opt_r, opt_f;

static int rm(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (opt_f && errno == ENOENT) return 0;
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) < 0) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
        return 0;
    }
    if (!opt_r) { fprintf(stderr, "rm: %s: Is a directory\n", path); return 1; }
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
    char **names = NULL;                         /* collect first: unlinking while iterating is unsafe */
    int n = 0, rc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        names = realloc(names, (n + 1) * sizeof *names);
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", path, names[i]);
        rc |= rm(full);
        free(names[i]);
    }
    free(names);
    if (rc) return 1;
    if (rmdir(path) < 0) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'r' || *f == 'R') opt_r = 1;
            else if (*f == 'f') opt_f = 1;
            else { fprintf(stderr, "usage: rm [-rf] file...\n"); return 2; }
    }
    if (i >= argc) { if (opt_f) return 0; fprintf(stderr, "rm: missing operand\n"); return 1; }
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "/") || !strcmp(argv[i], ".") || !strcmp(argv[i], "..")) {
            fprintf(stderr, "rm: refusing to remove '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        rc |= rm(argv[i]);
    }
    return rc;
}
