/* tail [-n [+]N | -N] [-c [+]N] [-f] [file...]   (+N: start at line/byte N; reads the whole input; -f follows one file) */
#include <vutil.h>

static long nl = 10, nc = -1;
static int follow, plus_l, plus_c;

static int tail(const char *name, int fd) {
    size_t len;
    char *b = readall(fd, &len);
    if (!b) { WARN("tail", name); return 1; }
    size_t start;
    if (plus_c) start = nc > 1 ? ((size_t)nc - 1 < len ? (size_t)nc - 1 : len) : 0;
    else if (plus_l) {
        start = 0;
        for (long k = 1; k < nl && start < len; k++) { char *e = memchr(b + start, '\n', len - start); start = e ? (size_t)(e - b) + 1 : len; }
    } else if (nc >= 0) start = (size_t)nc < len ? len - nc : 0;
    else {
        start = len;
        long seen = 0;
        if (len && b[len - 1] == '\n') start--;       /* a trailing newline ends the last line */
        while (start > 0 && seen < nl) { if (b[--start] == '\n') seen++; }
        if (seen == nl && nl > 0) start++;
        if (nl == 0) start = len;
    }
    write_all(1, b + start, len - start);
    free(b);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-f")) follow = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) { plus_l = argv[i + 1][0] == '+'; nl = atol(argv[++i]); }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) { plus_c = argv[i + 1][0] == '+'; nc = atol(argv[++i]); }
        else if (argv[i][1] >= '0' && argv[i][1] <= '9') nl = atol(argv[i] + 1);
        else { fprintf(stderr, "usage: tail [-n N] [-c N] [-f] [file...]\n"); return 2; }
    }
    if (i >= argc) return tail("stdin", 0);
    int first = i;
    for (int multi = argc - i > 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { fprintf(stderr, "tail: cannot open '%s' for reading: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if (multi) printf("==> %s <==\n", argv[i]);
        fflush(stdout);
        rc |= tail(argv[i], fd);
        if (fd) close(fd);
        if (multi && i < argc - 1) putchar('\n');
    }
    if (follow && first < argc) {
        int fd = open(argv[first], O_RDONLY);
        if (fd < 0) { WARN("tail", argv[first]); return 1; }
        lseek(fd, 0, SEEK_END);
        for (;;) {                                   /* ^C ends it */
            char buf[1024];
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) write_all(1, buf, n);
            else usleep(250000);
        }
    }
    return rc;
}
