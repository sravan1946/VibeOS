/* cat [file...]  (no args or "-" = stdin) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int cat_fd(int fd, const char *name) {
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(1, buf + off, n - off);
            if (w <= 0) { fprintf(stderr, "cat: write error: %s\n", strerror(errno)); return 1; }
            off += w;
        }
    }
    if (n < 0) { fprintf(stderr, "cat: %s: %s\n", name, strerror(errno)); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) return cat_fd(0, "stdin");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) { rc |= cat_fd(0, "stdin"); continue; }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        rc |= cat_fd(fd, argv[i]);
        close(fd);
    }
    return rc;
}
