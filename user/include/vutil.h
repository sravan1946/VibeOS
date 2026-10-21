/* vutil.h - tiny helpers shared by the coreutils (all static inline, include what you use) */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

/* "prog: name: strerror(errno)" */
#define WARN(prog, name) fprintf(stderr, "%s: %s: %s\n", prog, name, strerror(errno))

/* read a whole fd into a malloc'd, NUL-terminated buffer; NULL on error */
static inline char *readall(int fd, size_t *len) {
    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    if (!b) return NULL;
    for (;;) {
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { free(b); return NULL; }
        if (r == 0) break;
        n += r;
        if (n == cap) {
            char *t = realloc(b, cap * 2 + 1);
            if (!t) { free(b); return NULL; }
            b = t; cap *= 2;
        }
    }
    b[n] = 0;
    if (len) *len = n;
    return b;
}

/* open path ("-" = stdin) and slurp it; prints "prog: path: err" on failure */
static inline char *slurp(const char *prog, const char *path, size_t *len) {
    int fd = strcmp(path, "-") ? open(path, O_RDONLY) : 0;
    if (fd < 0) { WARN(prog, path); return NULL; }
    char *b = readall(fd, len);
    if (!b) WARN(prog, path);
    if (fd) close(fd);
    return b;
}

static inline int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static inline const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* full write; returns 0 or -1 */
static inline int write_all(int fd, const char *b, size_t n) {
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return -1;
        b += w; n -= w;
    }
    return 0;
}
