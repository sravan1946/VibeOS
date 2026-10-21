/* head [-n N | -N] [-c N] [file...] */
#include <vutil.h>

static long nl = 10, nc = -1;

static int head(const char *name, int fd) {
    char buf[4096];
    ssize_t n;
    long lines = 0, bytes = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        ssize_t k = 0;
        if (nc >= 0) k = bytes + n > nc ? nc - bytes : n;
        else while (k < n && lines < nl) if (buf[k++] == '\n') lines++;
        write_all(1, buf, k);
        bytes += k;
        if (nc >= 0 ? bytes >= nc : lines >= nl) return 0;
    }
    if (n < 0) { WARN("head", name); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-n") && i + 1 < argc) nl = atol(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) nc = atol(argv[++i]);
        else if (argv[i][1] >= '0' && argv[i][1] <= '9') nl = atol(argv[i] + 1);
        else { fprintf(stderr, "usage: head [-n N] [-c N] [file...]\n"); return 2; }
    }
    if (i >= argc) return head("stdin", 0);
    for (int multi = argc - i > 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { fprintf(stderr, "head: cannot open '%s' for reading: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if (multi) printf("==> %s <==\n", argv[i]);
        fflush(stdout);
        rc |= head(argv[i], fd);
        if (fd) close(fd);
        if (multi && i < argc - 1) putchar('\n');
    }
    return rc;
}
