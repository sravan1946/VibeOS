/* md5sum [-c] [file...]   print "HASH  name"; -c reads such lines from the files and verifies them */
#include <vutil.h>

typedef unsigned u32;
#define ROL(x, n) ((x) << (n) | (x) >> (32 - (n)))
static u32 K[64];
static const unsigned char S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

typedef struct { u32 h[4]; unsigned char buf[64]; unsigned long long len; int n; } Ctx;

static void block(Ctx *c, const unsigned char *p) {
    u32 m[16], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    for (int i = 0; i < 16; i++) m[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        u32 f; int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }
        f += a + K[i] + m[g];
        a = d; d = cc; cc = b; b += ROL(f, S[i]);
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
}

static void init(Ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe; c->h[3] = 0x10325476; c->len = 0; c->n = 0;
    if (!K[0]) {                                  /* K[i] = floor(2^32 * |sin(i + 1)|), the standard table */
        static const u32 t[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        memcpy(K, t, sizeof t);
    }
}
static void update(Ctx *c, const unsigned char *p, size_t n) {
    c->len += n;
    while (n) {
        size_t k = (size_t)(64 - c->n) < n ? (size_t)(64 - c->n) : n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { block(c, c->buf); c->n = 0; }
    }
}
static void final(Ctx *c, char *hex) {
    unsigned long long bits = c->len * 8;
    unsigned char pad[72] = { 0x80 };
    int padn = (c->n < 56 ? 56 : 120) - c->n;
    for (int i = 0; i < 8; i++) pad[padn + i] = bits >> (8 * i);
    update(c, pad, padn + 8);
    for (int i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", c->h[i / 4] >> (8 * (i % 4)) & 255);
}

static int hashfile(const char *name, char *hex) {
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
    if (fd < 0) { WARN("md5sum", name); return -1; }
    Ctx c;
    unsigned char b[4096];
    ssize_t n;
    init(&c);
    while ((n = read(fd, b, sizeof b)) > 0) update(&c, b, n);
    if (fd) close(fd);
    if (n < 0) { WARN("md5sum", name); return -1; }
    final(&c, hex);
    return 0;
}

int main(int argc, char **argv) {
    int check = 0, i = 1, rc = 0;
    char hex[33];
    if (i < argc && !strcmp(argv[i], "-c")) { check = 1; i++; }
    else if (i < argc && argv[i][0] == '-' && argv[i][1]) { fputs("usage: md5sum [-c] [file...]\n", stderr); return 2; }
    if (!check) {
        if (i >= argc) { if (hashfile("-", hex)) return 1; printf("%s  -\n", hex); return 0; }
        for (; i < argc; i++) { if (hashfile(argv[i], hex)) rc = 1; else printf("%s  %s\n", hex, argv[i]); }
        return rc;
    }
    int bad = 0;
    if (i >= argc) argv[--i] = "-";
    for (; i < argc; i++) {
        size_t len;
        char *b = slurp("md5sum", argv[i], &len);
        if (!b) { rc = 1; continue; }
        for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
            if (strlen(l) < 35 || l[32] != ' ') continue;
            char *name = l + 34;                 /* "HASH  name" or "HASH *name" */
            l[32] = 0;
            if (hashfile(name, hex)) { rc = 1; printf("%s: FAILED open or read\n", name); bad++; }
            else if (!strcasecmp(hex, l)) { printf("%s: OK\n", name); }
            else { printf("%s: FAILED\n", name); bad++; }
        }
        free(b);
    }
    if (bad) fprintf(stderr, "md5sum: WARNING: %d computed checksum%s did NOT match\n", bad, bad > 1 ? "s" : "");
    return rc || bad;
}
