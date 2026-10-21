/* wc [-l] [-w] [-c] [file...] */
#include <vutil.h>
#include <ctype.h>

static int fl, fw, fc;
static unsigned tl, tw, tc;

static void show(unsigned l, unsigned w, unsigned c, const char *name) {
    int cols = fl + fw + fc;
    const char *f = cols > 1 ? "%7u" : "%u";
    if (fl) printf(f, l);
    if (fw) printf(cols > 1 ? " " "%7u" : f, w);
    if (fc) printf(cols > 1 ? " " "%7u" : f, c);
    if (name) printf(" %s", name);
    putchar('\n');
}

static int count(const char *name, int fd) {
    char buf[4096];
    ssize_t n;
    unsigned l = 0, w = 0, c = 0;
    int inw = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        c += n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') l++;
            if (isspace((unsigned char)buf[i])) inw = 0;
            else if (!inw) { inw = 1; w++; }
        }
    }
    if (n < 0) { WARN("wc", name ? name : "stdin"); return 1; }
    show(l, w, c, name);
    tl += l; tw += w; tc += c;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'l') fl = 1;
            else if (*f == 'w') fw = 1;
            else if (*f == 'c' || *f == 'm') fc = 1;
            else { fprintf(stderr, "usage: wc [-lwc] [file...]\n"); return 2; }
    }
    if (!fl && !fw && !fc) fl = fw = fc = 1;
    if (i >= argc) return count(NULL, 0);
    for (int n = argc - i; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { WARN("wc", argv[i]); rc = 1; continue; }
        rc |= count(argv[i], fd);
        if (fd) close(fd);
        if (n > 1 && i == argc - 1) show(tl, tw, tc, "total");
    }
    return rc;
}
