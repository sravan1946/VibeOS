/* gzip [-dckf] [FILE...]   gunzip = gzip -d, zcat = gzip -dc (chosen by argv[0], see gunzip.c / zcat.c)
 * Compress FILE to FILE.gz (removing FILE unless -k/-c); no FILE: stdin to stdout. Whole file in memory.
 * ponytail: fixed-Huffman deflate (about what zlib -1 gives on text, worse on binaries), one gzip member per file. */
#include <vutil.h>
#include "image.h"

static int dflag, cflag, kflag, fflag;

static uint8_t *slurpfd(int fd, size_t *n) {
    char *b = readall(fd, n);
    return (uint8_t *)b;
}

static int writeall(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) return -1; p += w; n -= w; }
    return 0;
}

/* in -> out (fd), compressing or decompressing; 0 / -1 */
static int convert(const char *name, uint8_t *in, size_t n, int out) {
    uint8_t *o; size_t on;
    if (dflag) {
        if (gunzip_buf(in, n, &o, &on)) { fprintf(stderr, "gzip: %s: not in gzip format or corrupt\n", name); return -1; }
    } else if (!(o = gzip_buf(in, n, &on))) { fprintf(stderr, "gzip: out of memory\n"); return -1; }
    int rc = writeall(out, o, on);
    free(o);
    if (rc) WARN("gzip", name);
    return rc;
}

static int one(const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { WARN("gzip", path); return 1; }
    if (S_ISDIR(st.st_mode)) { fprintf(stderr, "gzip: %s is a directory -- ignored\n", path); close(fd); return 1; }
    size_t n, pl = strlen(path);
    uint8_t *in = slurpfd(fd, &n);
    close(fd);
    if (!in) { WARN("gzip", path); return 1; }
    char dst[PATH_MAX];
    int rc = 0;
    if (cflag) rc = convert(path, in, n, 1);
    else {
        if (dflag) {
            if (pl > 3 && !strcmp(path + pl - 3, ".gz")) snprintf(dst, sizeof dst, "%.*s", (int)pl - 3, path);
            else if (pl > 4 && !strcmp(path + pl - 4, ".tgz")) snprintf(dst, sizeof dst, "%.*s.tar", (int)pl - 4, path);
            else { fprintf(stderr, "gzip: %s: unknown suffix -- ignored\n", path); free(in); return 1; }
        } else {
            if (pl > 3 && !strcmp(path + pl - 3, ".gz")) { fprintf(stderr, "gzip: %s already has .gz suffix -- unchanged\n", path); free(in); return 1; }
            snprintf(dst, sizeof dst, "%s.gz", path);
        }
        struct stat d;
        if (!fflag && !stat(dst, &d)) { fprintf(stderr, "gzip: %s already exists; not overwriting (-f to force)\n", dst); free(in); return 1; }
        int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) { WARN("gzip", dst); free(in); return 1; }
        rc = convert(path, in, n, out);
        close(out);
        if (rc) unlink(dst);
        else if (!kflag) unlink(path);
    }
    free(in);
    return rc != 0;
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    if (!strcmp(me, "gunzip")) dflag = 1;
    if (!strcmp(me, "zcat")) dflag = cflag = 1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'd') dflag = 1;
            else if (*p == 'c') cflag = 1;
            else if (*p == 'k') kflag = 1;
            else if (*p == 'f') fflag = 1;
            else if (*p >= '1' && *p <= '9') {}                  /* level: one level only */
            else { fprintf(stderr, "usage: gzip [-dckf] [FILE...]   (gunzip, zcat)\n"); return 2; }
        }
    }
    if (i >= argc) {
        size_t n;
        uint8_t *in = slurpfd(0, &n);
        if (!in) { WARN("gzip", "stdin"); return 1; }
        int rc = convert("stdin", in, n, 1);
        free(in);
        return rc != 0;
    }
    int rc = 0;
    for (; i < argc; i++) rc |= one(argv[i]);
    return rc;
}
