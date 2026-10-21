/* cp [-r] src... dest */
#include <vutil.h>
#include <dirent.h>

static int opt_r;

static int copy(const char *src, const char *dst) {
    struct stat st, dt;
    if (stat(src, &st) < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }
    if (stat(dst, &dt) == 0 && st.st_ino == dt.st_ino && S_ISREG(st.st_mode)) {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!opt_r) { fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src); return 1; }
        if (mkdir(dst, 0755) < 0 && !(errno == EEXIST && is_dir(dst))) {
            fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
            return 1;
        }
        size_t sl = strlen(src);
        if (!strncmp(dst, src, sl) && (dst[sl] == '/' || !dst[sl])) {
            fprintf(stderr, "cp: cannot copy a directory, '%s', into itself, '%s'\n", src, dst);
            return 1;
        }
        DIR *d = opendir(src);
        if (!d) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }
        int rc = 0;
        struct dirent *de;
        char **names = NULL;
        int n = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            names = realloc(names, (n + 1) * sizeof *names);
            names[n++] = strdup(de->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char s[PATH_MAX], t[PATH_MAX];
            snprintf(s, sizeof s, "%s/%s", src, names[i]);
            snprintf(t, sizeof t, "%s/%s", dst, names[i]);
            rc |= copy(s, t);
            free(names[i]);
        }
        free(names);
        return rc;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) { fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno)); close(in); return 1; }
    char buf[4096];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write_all(out, buf, n) < 0) { fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno)); rc = 1; break; }
    if (n < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); rc = 1; }
    close(in); close(out);
    return rc;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'r' || *f == 'R') opt_r = 1;
            else { fprintf(stderr, "usage: cp [-r] src... dest\n"); return 2; }
    }
    int n = argc - i;
    if (n < 2) { fprintf(stderr, n ? "cp: missing destination file operand after '%s'\n" : "cp: missing file operand\n", argv[argc - 1]); return 1; }
    const char *dest = argv[argc - 1];
    int dd = is_dir(dest);
    if (n > 2 && !dd) { fprintf(stderr, "cp: target '%s' is not a directory\n", dest); return 1; }
    for (; i < argc - 1; i++) {
        char t[PATH_MAX];
        if (dd) snprintf(t, sizeof t, "%s/%s", dest, base_name(argv[i]));
        else strlcpy(t, dest, sizeof t);
        rc |= copy(argv[i], t);
    }
    return rc;
}
