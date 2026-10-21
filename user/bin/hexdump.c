/* hexdump [-C] [-n N] [-s N] [file]   always canonical (-C) layout, identical lines collapse to '*' */
#include <vutil.h>

int main(int argc, char **argv) {
    long limit = -1, skip = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-C")) continue;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) skip = atol(argv[++i]);
        else { fprintf(stderr, "usage: hexdump [-C] [-n N] [-s N] [file]\n"); return 2; }
    }
    size_t len;
    char *b = slurp("hexdump", i < argc ? argv[i] : "-", &len);
    if (!b) return 1;
    unsigned char *d = (unsigned char *)b;
    size_t end = limit >= 0 && (size_t)(skip + limit) < len ? (size_t)(skip + limit) : len;
    int star = 0;
    for (size_t o = skip; o < end; o += 16) {
        size_t n = end - o < 16 ? end - o : 16;
        if (o > (size_t)skip && n == 16 && !memcmp(d + o, d + o - 16, 16)) {
            if (!star) puts("*");
            star = 1;
            continue;
        }
        star = 0;
        printf("%08x ", (unsigned)o);
        for (size_t k = 0; k < 16; k++) {
            if (k == 8) putchar(' ');
            if (k < n) printf(" %02x", d[o + k]); else fputs("   ", stdout);
        }
        fputs("  |", stdout);
        for (size_t k = 0; k < n; k++) putchar(d[o + k] >= 32 && d[o + k] < 127 ? d[o + k] : '.');
        puts("|");
    }
    printf("%08x\n", (unsigned)end);
    free(b);
    return 0;
}
