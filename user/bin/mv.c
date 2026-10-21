/* mv src... dest   (rename only; one filesystem) */
#include <vutil.h>

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "--")) i++;
    int n = argc - i;
    if (n < 2) { fprintf(stderr, n ? "mv: missing destination file operand after '%s'\n" : "mv: missing file operand\n", argv[argc - 1]); return 1; }
    const char *dest = argv[argc - 1];
    int dd = is_dir(dest);
    if (n > 2 && !dd) { fprintf(stderr, "mv: target '%s' is not a directory\n", dest); return 1; }
    for (; i < argc - 1; i++) {
        char t[PATH_MAX];
        struct stat st;
        if (stat(argv[i], &st) < 0) { fprintf(stderr, "mv: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if (dd) snprintf(t, sizeof t, "%s/%s", dest, base_name(argv[i]));
        else strlcpy(t, dest, sizeof t);
        if (rename(argv[i], t) < 0) { fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", argv[i], t, strerror(errno)); rc = 1; }
    }
    return rc;
}
