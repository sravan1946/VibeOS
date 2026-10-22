/* sha256sum [-c] [file...]   print "HASH  name"; -c reads such lines from the files and verifies them */
#include <vutil.h>

typedef unsigned u32;
static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

typedef struct { u32 h[8]; unsigned char buf[64]; unsigned long long len; int n; } Ctx;

static void block(Ctx *c, const unsigned char *p) {
    u32 w[64], a, b, d, e, f, g, h, cc, t1, t2;
    for (int i = 0; i < 16; i++) w[i] = p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void init(Ctx *c) {
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
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
    for (int i = 0; i < 8; i++) pad[padn + i] = bits >> (56 - 8 * i);
    update(c, pad, padn + 8);
    for (int i = 0; i < 8; i++) sprintf(hex + 8 * i, "%08x", c->h[i]);
}

static int hashfile(const char *name, char *hex) {
    int fd = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
    if (fd < 0) { WARN("sha256sum", name); return -1; }
    Ctx c;
    unsigned char b[4096];
    ssize_t n;
    init(&c);
    while ((n = read(fd, b, sizeof b)) > 0) update(&c, b, n);
    if (fd) close(fd);
    if (n < 0) { WARN("sha256sum", name); return -1; }
    final(&c, hex);
    return 0;
}

int main(int argc, char **argv) {
    int check = 0, i = 1, rc = 0;
    char hex[65];
    if (i < argc && !strcmp(argv[i], "-c")) { check = 1; i++; }
    else if (i < argc && argv[i][0] == '-' && argv[i][1]) { fputs("usage: sha256sum [-c] [file...]\n", stderr); return 2; }
    if (!check) {
        if (i >= argc) { if (hashfile("-", hex)) return 1; printf("%s  -\n", hex); return 0; }
        for (; i < argc; i++) { if (hashfile(argv[i], hex)) rc = 1; else printf("%s  %s\n", hex, argv[i]); }
        return rc;
    }
    int bad = 0;
    if (i >= argc) argv[--i] = "-";
    for (; i < argc; i++) {
        size_t len;
        char *b = slurp("sha256sum", argv[i], &len);
        if (!b) { rc = 1; continue; }
        for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) {
            if (strlen(l) < 67 || l[64] != ' ') continue;
            char *name = l + 66;                 /* "HASH  name" or "HASH *name" */
            l[64] = 0;
            if (hashfile(name, hex)) { rc = 1; printf("%s: FAILED open or read\n", name); bad++; }
            else if (!strcasecmp(hex, l)) { printf("%s: OK\n", name); }
            else { printf("%s: FAILED\n", name); bad++; }
        }
        free(b);
    }
    if (bad) fprintf(stderr, "sha256sum: WARNING: %d computed checksum%s did NOT match\n", bad, bad > 1 ? "s" : "");
    return rc || bad;
}
