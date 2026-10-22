/* image.c - zlib inflate, PNG/BMP/PPM decoders, PNG encoder, resampler. See image.h. */
#include "image.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

#define MAXDIM 16384

/* ================= inflate (puff-style canonical Huffman) ================= */
struct inf { const uint8_t *in; size_t n, pos; uint32_t bb; int bc; uint8_t *out; size_t len, cap; int err; };
struct huff { uint16_t count[16], sym[288]; };

static int getbits(struct inf *s, int need) {
    while (s->bc < need) {
        if (s->pos >= s->n) { s->err = 1; return 0; }
        s->bb |= (uint32_t)s->in[s->pos++] << s->bc;
        s->bc += 8;
    }
    int v = s->bb & ((1u << need) - 1);
    s->bb >>= need; s->bc -= need;
    return v;
}

static void huff_build(struct huff *h, const uint8_t *len, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = i;
}

static int huff_dec(struct inf *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        code |= getbits(s, 1);
        if (s->err) return -1;
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static const uint16_t LBASE[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t LEXT[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t DEXT[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inf_room(struct inf *s, size_t more) {
    if (s->len + more <= s->cap) return 0;
    size_t c = s->cap ? s->cap : 65536;
    while (s->len + more > c) c *= 2;
    uint8_t *p = realloc(s->out, c);
    if (!p) return -1;
    s->out = p; s->cap = c;
    return 0;
}

static int inf_codes(struct inf *s, const struct huff *lc, const struct huff *dc) {
    for (;;) {
        int sym = huff_dec(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (inf_room(s, 1)) return -1;
            s->out[s->len++] = sym;
        } else if (sym == 256) return 0;
        else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t len = LBASE[sym] + getbits(s, LEXT[sym]);
            int ds = huff_dec(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = DBASE[ds] + getbits(s, DEXT[ds]);
            if (s->err || dist > s->len || inf_room(s, len)) return -1;
            for (; len; len--, s->len++) s->out[s->len] = s->out[s->len - dist];
        }
    }
}

int zlib_inflate(const uint8_t *in, size_t n, uint8_t **out, size_t *olen, size_t hint) {
    struct inf s = { .in = in, .n = n };
    if (n < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31) return -1;
    s.pos = 2;
    if (hint && !(s.out = malloc(s.cap = hint))) return -1;
    int last, rc = 0;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.err) { rc = -1; break; }
        if (type == 0) {
            s.bb = 0; s.bc = 0;
            if (s.pos + 4 > s.n) { rc = -1; break; }
            size_t len = in[s.pos] | (in[s.pos + 1] << 8);
            if ((len ^ 0xFFFF) != (size_t)(in[s.pos + 2] | (in[s.pos + 3] << 8))) { rc = -1; break; }
            s.pos += 4;
            if (s.pos + len > s.n || inf_room(&s, len)) { rc = -1; break; }
            memcpy(s.out + s.len, in + s.pos, len);
            s.len += len; s.pos += len;
        } else if (type == 1) {
            struct huff lc, dc;
            uint8_t l[288];
            for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            huff_build(&lc, l, 288);
            for (int i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dc, l, 30);
            rc = inf_codes(&s, &lc, &dc);
        } else if (type == 2) {
            static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t l[320] = { 0 };
            int nl = getbits(&s, 5) + 257, nd = getbits(&s, 5) + 1, nc = getbits(&s, 4) + 4;
            if (nl > 286 || nd > 30) { rc = -1; break; }
            for (int i = 0; i < nc; i++) l[ord[i]] = getbits(&s, 3);
            struct huff cl, lc, dc;
            huff_build(&cl, l, 19);
            memset(l, 0, sizeof l);
            for (int i = 0; i < nl + nd && !rc;) {
                int sym = huff_dec(&s, &cl);
                if (sym < 0) { rc = -1; break; }
                if (sym < 16) l[i++] = sym;
                else {
                    int prev = 0, rep;
                    if (sym == 16) { if (!i) { rc = -1; break; } prev = l[i - 1]; rep = 3 + getbits(&s, 2); }
                    else if (sym == 17) rep = 3 + getbits(&s, 3);
                    else rep = 11 + getbits(&s, 7);
                    if (i + rep > nl + nd) { rc = -1; break; }
                    while (rep--) l[i++] = prev;
                }
            }
            if (rc) break;
            huff_build(&lc, l, nl);
            huff_build(&dc, l + nl, nd);
            rc = inf_codes(&s, &lc, &dc);
        } else rc = -1;
        if (s.err) rc = -1;
    } while (!last && !rc);
    if (rc) { free(s.out); return -1; }
    *out = s.out ? s.out : malloc(1);
    *olen = s.len;
    return 0;
}

/* ================= checksums ================= */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    crc = ~crc;
    while (n--) crc = t[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return b << 16 | a;
}

/* ================= PNG decode ================= */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* sample #i of a row at the given depth (full 16 bits for depth 16) */
static unsigned smp(const uint8_t *row, int i, int depth) {
    if (depth == 8) return row[i];
    if (depth == 16) return row[2 * i] << 8 | row[2 * i + 1];
    int bit = i * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
}

static int png_decode(const uint8_t *d, size_t n, struct image *im) {
    static const uint8_t X0[] = { 0,4,0,2,0,1,0 }, Y0[] = { 0,0,4,0,2,0,1 }, DX[] = { 8,8,4,4,2,2,1 }, DY[] = { 8,8,8,4,4,2,2 };
    uint32_t pal[256], w = 0, h = 0;
    uint8_t trns[256], key[6] = { 0 };
    int depth = 0, ct = -1, inter = 0, npal = 0, haskey = 0, got_hdr = 0, rc = -1;
    uint8_t *idat = NULL, *raw = NULL, *zero = NULL;
    uint32_t *px = NULL;
    size_t ilen = 0, pos = 8;
    memset(trns, 255, sizeof trns);
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t *t = d + pos + 4, *c = t + 4;
        if (len > n - pos - 12) break;
        pos += 12 + len;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = be32(c); h = be32(c + 4); depth = c[8]; ct = c[9]; inter = c[12];
            if (c[10] || c[11] || inter > 1 || !w || !h || w > MAXDIM || h > MAXDIM) goto out;
            static const uint8_t okd[7] = { 0x1F, 0, 0x18, 0x0F, 0x18, 0, 0x18 };   /* allowed depths per type, bit i = 1<<i bits */
            int bi = depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : -1;
            if (ct < 0 || ct > 6 || bi < 0 || !(okd[ct] >> bi & 1)) goto out;
            got_hdr = 1;
        } else if (!memcmp(t, "PLTE", 4)) {
            npal = len / 3 > 256 ? 256 : len / 3;
            for (int i = 0; i < npal; i++) pal[i] = 0xFF000000u | c[3 * i] << 16 | c[3 * i + 1] << 8 | c[3 * i + 2];
        } else if (!memcmp(t, "tRNS", 4)) {
            if (ct == 3) memcpy(trns, c, len > 256 ? 256 : len);
            else if ((ct == 0 && len >= 2) || (ct == 2 && len >= 6)) { memcpy(key, c, ct ? 6 : 2); haskey = 1; }
        } else if (!memcmp(t, "IDAT", 4)) {
            uint8_t *p = realloc(idat, ilen + len + 1);
            if (!p) goto out;
            idat = p;
            memcpy(idat + ilen, c, len);
            ilen += len;
        } else if (!memcmp(t, "IEND", 4)) break;
    }
    if (!got_hdr || !idat || (ct == 3 && !npal)) goto out;
    int ch = ct == 2 ? 3 : ct == 4 ? 2 : ct == 6 ? 4 : 1;
    int bits = ch * depth, bpp = bits >= 8 ? bits / 8 : 1;
    int np = inter ? 7 : 1;
    size_t need = 0, rlen;
    for (int p = 0; p < np; p++) {
        int x0 = inter ? X0[p] : 0, y0 = inter ? Y0[p] : 0, dx = inter ? DX[p] : 1, dy = inter ? DY[p] : 1;
        if ((int)w <= x0 || (int)h <= y0) continue;
        size_t pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        need += ph * (1 + (pw * bits + 7) / 8);
    }
    if (zlib_inflate(idat, ilen, &raw, &rlen, need) || rlen < need) goto out;
    px = malloc((size_t)w * h * 4);
    zero = calloc(1, ((size_t)w * bits + 7) / 8 + 8);
    if (!px || !zero) goto out;
    uint8_t *row = raw;
    for (int p = 0; p < np; p++) {
        int x0 = inter ? X0[p] : 0, y0 = inter ? Y0[p] : 0, dx = inter ? DX[p] : 1, dy = inter ? DY[p] : 1;
        if ((int)w <= x0 || (int)h <= y0) continue;
        int pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        size_t stride = ((size_t)pw * bits + 7) / 8;
        const uint8_t *prev = zero;
        for (int j = 0; j < ph; j++, row += 1 + stride) {
            uint8_t *r = row + 1;
            int f = row[0];
            if (f > 4) goto out;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? r[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
                switch (f) {
                case 1: r[i] += a; break;
                case 2: r[i] += b; break;
                case 3: r[i] += (a + b) >> 1; break;
                case 4: r[i] += paeth(a, b, c); break;
                }
            }
            prev = r;
            uint32_t *o = px + (size_t)(y0 + j * dy) * w + x0;
            for (int i = 0; i < pw; i++, o += dx) {
                unsigned v[4];
                for (int k = 0; k < ch; k++) v[k] = smp(r, i * ch + k, depth);
                unsigned R, G, B, A = 255, s = depth == 16 ? 8 : 0;
                if (ct == 3) {
                    *o = v[0] < (unsigned)npal ? (pal[v[0]] & 0xFFFFFF) | (uint32_t)trns[v[0]] << 24 : 0xFF000000u;
                    continue;
                }
                if (ct == 0 || ct == 4) {
                    R = G = B = depth < 8 ? v[0] * 255 / ((1 << depth) - 1) : v[0] >> s;
                    if (ct == 4) A = v[1] >> s;
                    else if (haskey && v[0] == (unsigned)(key[0] << 8 | key[1])) A = 0;
                } else {
                    R = v[0] >> s; G = v[1] >> s; B = v[2] >> s;
                    if (ct == 6) A = v[3] >> s;
                    else if (haskey && v[0] == (unsigned)(key[0] << 8 | key[1]) && v[1] == (unsigned)(key[2] << 8 | key[3]) &&
                             v[2] == (unsigned)(key[4] << 8 | key[5])) A = 0;
                }
                *o = A << 24 | R << 16 | G << 8 | B;
            }
        }
    }
    im->w = w; im->h = h; im->px = px;
    px = NULL;
    rc = 0;
out:
    free(px); free(zero); free(raw); free(idat);
    return rc;
}

/* ================= BMP / PPM ================= */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int bmp_decode(const uint8_t *d, size_t n, struct image *im) {
    if (n < 26 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t off = le32(d + 10), hs = le32(d + 14);
    if (hs < 40 || n < 14 + hs) return -1;
    int32_t w = le32(d + 18), h = le32(d + 22);
    int bpp = d[28] | d[29] << 8, comp = le32(d + 30), top = h < 0;
    if (top) h = -h;
    if (w <= 0 || h <= 0 || w > MAXDIM || h > MAXDIM || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) return -1;
    size_t rowb = ((size_t)w * bpp / 8 + 3) & ~3u;
    if (off > n || (size_t)h * rowb > n - off) return -1;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return -1;
    int has_a = 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = d + off + (size_t)(top ? y : h - 1 - y) * rowb;
        uint32_t *o = px + (size_t)y * w;
        for (int x = 0; x < w; x++, s += bpp / 8) {
            uint32_t a = bpp == 32 ? s[3] : 255;
            has_a |= a;
            o[x] = a << 24 | s[2] << 16 | s[1] << 8 | s[0];
        }
    }
    if (bpp == 32 && !has_a) for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xFF000000u;   /* BI_RGB: X channel */
    im->w = w; im->h = h; im->px = px;
    return 0;
}

static int ppm_num(const uint8_t *d, size_t n, size_t *pos) {
    for (;;) {
        while (*pos < n && (d[*pos] == ' ' || d[*pos] == '\n' || d[*pos] == '\r' || d[*pos] == '\t')) (*pos)++;
        if (*pos < n && d[*pos] == '#') { while (*pos < n && d[*pos] != '\n') (*pos)++; } else break;
    }
    long v = 0;
    int any = 0;
    while (*pos < n && d[*pos] >= '0' && d[*pos] <= '9') { v = v * 10 + d[(*pos)++] - '0'; any = 1; if (v > 1 << 20) return -1; }
    return any ? (int)v : -1;
}

static int ppm_decode(const uint8_t *d, size_t n, struct image *im) {     /* binary P5 (gray) and P6 */
    if (n < 3 || d[0] != 'P' || (d[1] != '5' && d[1] != '6')) return -1;
    int ch = d[1] == '6' ? 3 : 1;
    size_t pos = 2;
    int w = ppm_num(d, n, &pos), h = ppm_num(d, n, &pos), mv = ppm_num(d, n, &pos);
    if (w <= 0 || h <= 0 || w > MAXDIM || h > MAXDIM || mv <= 0 || mv > 65535 || pos >= n) return -1;
    pos++;                                                                  /* one whitespace byte */
    int two = mv > 255;
    if ((size_t)w * h * ch * (1 + two) > n - pos) return -1;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return -1;
    const uint8_t *s = d + pos;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned c[3];
        for (int k = 0; k < ch; k++) {
            unsigned v = two ? s[0] << 8 | s[1] : s[0];
            s += 1 + two;
            c[k] = v >= (unsigned)mv ? 255 : (v * 255 + mv / 2) / mv;
        }
        if (ch == 1) c[1] = c[2] = c[0];
        px[i] = 0xFF000000u | c[0] << 16 | c[1] << 8 | c[2];
    }
    im->w = w; im->h = h; im->px = px;
    return 0;
}

int image_decode(const uint8_t *d, size_t n, struct image *im) {
    im->w = im->h = 0; im->px = NULL;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return png_decode(d, n, im);
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return bmp_decode(d, n, im);
    if (n >= 2 && d[0] == 'P') return ppm_decode(d, n, im);
    return -1;
}

int image_load(const char *path, struct image *im) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 16, len = 0, r;
    uint8_t *b = malloc(cap);
    while (b && (r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) { uint8_t *nb = realloc(b, cap *= 2); if (!nb) { free(b); b = NULL; } else b = nb; }
    }
    fclose(f);
    if (!b) return -1;
    int rc = image_decode(b, len, im);
    free(b);
    return rc;
}

void image_free(struct image *im) { free(im->px); im->px = NULL; im->w = im->h = 0; }

int image_is_ext(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !strcasecmp(e, ".png") || !strcasecmp(e, ".bmp") || !strcasecmp(e, ".ppm") || !strcasecmp(e, ".pgm");
}

/* ================= PNG encode: filter 0, LZ77 (one hash slot) + fixed Huffman ================= */
struct bw { uint8_t *p; size_t len, cap; uint32_t acc; int nb; int err; };

static void bw_byte(struct bw *b, uint8_t v) {
    if (b->err) return;
    if (b->len == b->cap) {
        uint8_t *p = realloc(b->p, b->cap = b->cap ? b->cap * 2 : 1 << 16);
        if (!p) { b->err = 1; return; }
        b->p = p;
    }
    b->p[b->len++] = v;
}
static void bw_bits(struct bw *b, uint32_t v, int n) {      /* LSB first */
    b->acc |= v << b->nb; b->nb += n;
    while (b->nb >= 8) { bw_byte(b, b->acc & 255); b->acc >>= 8; b->nb -= 8; }
}
static void bw_code(struct bw *b, uint32_t code, int n) {   /* Huffman codes go MSB first */
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= (code >> i & 1) << (n - 1 - i);
    bw_bits(b, r, n);
}
static void bw_lit(struct bw *b, int s) {
    if (s < 144) bw_code(b, 0x30 + s, 8);
    else if (s < 256) bw_code(b, 0x190 + s - 144, 9);
    else if (s < 280) bw_code(b, s - 256, 7);
    else bw_code(b, 0xC0 + s - 280, 8);
}

#define HB 15
static uint32_t hash3(const uint8_t *p) { return ((p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u) >> (32 - HB); }

static void deflate_fixed(struct bw *b, const uint8_t *in, size_t n) {
    int32_t *head = malloc(sizeof(int32_t) << HB);
    if (!head) { b->err = 1; return; }
    memset(head, 0xFF, sizeof(int32_t) << HB);
    bw_bits(b, 1, 1); bw_bits(b, 1, 2);                      /* final block, fixed Huffman */
    size_t i = 0;
    while (i < n) {
        size_t best = 0, dist = 0;
        if (i + 3 <= n) {
            uint32_t h = hash3(in + i);
            int32_t c = head[h];
            head[h] = i;
            if (c >= 0 && i - c <= 32768) {
                size_t l = 0, mx = n - i < 258 ? n - i : 258;
                while (l < mx && in[c + l] == in[i + l]) l++;
                if (l >= 3) { best = l; dist = i - c; }
            }
        }
        if (!best) { bw_lit(b, in[i++]); continue; }
        int lc = 28;
        while (LBASE[lc] > best) lc--;
        bw_lit(b, 257 + lc);
        bw_bits(b, best - LBASE[lc], LEXT[lc]);
        int dc = 29;
        while (DBASE[dc] > dist) dc--;
        bw_code(b, dc, 5);
        bw_bits(b, dist - DBASE[dc], DEXT[dc]);
        for (size_t k = 1; k < best && i + k + 3 <= n; k++) head[hash3(in + i + k)] = i + k;   /* index covered positions */
        i += best;
    }
    bw_lit(b, 256);
    if (b->nb) bw_bits(b, 0, 8 - b->nb);
    free(head);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(struct bw *o, const char *type, const uint8_t *data, size_t n) {
    uint8_t h[8], c[4];
    put32(h, n); memcpy(h + 4, type, 4);
    for (int i = 0; i < 8; i++) bw_byte(o, h[i]);
    for (size_t i = 0; i < n; i++) bw_byte(o, data[i]);
    put32(c, crc32_update(crc32_update(0, h + 4, 4), data, n));
    for (int i = 0; i < 4; i++) bw_byte(o, c[i]);
}

uint8_t *image_png_encode(const uint32_t *px, int w, int h, size_t *len) {
    if (w <= 0 || h <= 0) return NULL;
    int opaque = 1;
    for (size_t i = 0; i < (size_t)w * h && opaque; i++) opaque = px[i] >> 24 == 255;
    int ch = opaque ? 3 : 4;
    size_t rb = (size_t)w * ch + 1, rawn = rb * h;
    uint8_t *raw = malloc(rawn);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + y * rb;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            *r++ = p >> 16; *r++ = p >> 8; *r++ = p;
            if (!opaque) *r++ = p >> 24;
        }
    }
    struct bw z = { 0 }, o = { 0 };
    bw_byte(&z, 0x78); bw_byte(&z, 0x01);
    deflate_fixed(&z, raw, rawn);
    uint8_t ad[4];
    put32(ad, adler32(raw, rawn));
    for (int i = 0; i < 4; i++) bw_byte(&z, ad[i]);
    free(raw);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) bw_byte(&o, sig[i]);
    uint8_t ih[13] = { 0 };
    put32(ih, w); put32(ih + 4, h);
    ih[8] = 8; ih[9] = ch == 3 ? 2 : 6;
    chunk(&o, "IHDR", ih, 13);
    if (!z.err) chunk(&o, "IDAT", z.p, z.len);
    chunk(&o, "IEND", NULL, 0);
    int err = z.err || o.err;
    free(z.p);
    if (err) { free(o.p); return NULL; }
    *len = o.len;
    return o.p;
}

int image_save_png(const char *path, const uint32_t *px, int w, int h) {
    size_t n;
    uint8_t *b = image_png_encode(px, w, h, &n);
    if (!b) return -1;
    int fd = -1;
    for (int i = 0; i < 4 && fd < 0; i++) {            /* the ext2 driver sometimes returns a transient EIO under load */
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) usleep(100000);
    }
    int ok = fd >= 0, fails = 0;
    for (size_t o = 0; ok && o < n;) {
        int r = write(fd, b + o, n - o);
        if (r > 0) { o += r; fails = 0; } else if (++fails > 8) ok = 0; else usleep(100000);
    }
    if (fd >= 0) close(fd);
    free(b);
    return ok ? 0 : -1;
}

/* ================= resampling ================= */
static uint32_t px_at(const struct image *im, int x, int y) {
    if (x < 0) x = 0; else if (x >= im->w) x = im->w - 1;
    if (y < 0) y = 0; else if (y >= im->h) y = im->h - 1;
    return im->px[(size_t)y * im->w + x];
}

/* weighted blend of 4 pixels (weights sum to 256), alpha-weighted so transparent edges don't fringe */
static uint32_t px_mix(const uint32_t *p, const int *wt) {
    uint32_t a = 0, r = 0, g = 0, b = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t al = (p[i] >> 24) * wt[i];
        a += al; r += al * (p[i] >> 16 & 255); g += al * (p[i] >> 8 & 255); b += al * (p[i] & 255);
    }
    if (!a) return 0;
    return (a >> 8) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a);
}

/* packed interpolation of two ARGB pixels (all four channels at once, 0 <= t <= 256) */
static uint32_t lerp(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t rb = ((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8 & 0xFF00FF;
    uint32_t ag = ((a >> 8 & 0xFF00FF) * (256 - t) + (b >> 8 & 0xFF00FF) * t) & 0xFF00FF00;
    return rb | ag;
}

void image_sample(const struct image *im, uint32_t *dst, int dw, int dh, int stride,
                  float ox, float oy, float scale, int smooth) {
    if (dw <= 0 || dh <= 0 || im->w <= 0 || im->h <= 0) return;
    float inv = 1 / scale;
    int *xa = malloc(dw * 3 * sizeof(int));                   /* per column: start/left index, end/right index, weight */
    if (!xa) return;
    int *xb = xa + dw, *xt = xb + dw;
    int box = smooth && scale < 1, bil = smooth && scale > 1;
    for (int x = 0; x < dw; x++) {
        float u = ox + (x + 0.5f) * inv;
        if (box) {
            xa[x] = (int)(u - inv / 2 + 0.5f); xb[x] = (int)(u + inv / 2 + 0.5f);
            if (xb[x] <= xa[x]) xb[x] = xa[x] + 1;
        } else if (bil) {
            float f = u - 0.5f;
            int i = (int)floorf(f);
            xt[x] = (int)((f - i) * 256); xa[x] = i; xb[x] = i + 1;
        } else xa[x] = (int)floorf(u);
    }
    int exact = scale == 1 && ox == floorf(ox) && oy == floorf(oy);
    for (int y = 0; y < dh; y++) {
        uint32_t *o = dst + (size_t)y * stride;
        float v = oy + (y + 0.5f) * inv;
        int y0, y1 = 0, ty = 0;
        if (box) {
            y0 = (int)(v - inv / 2 + 0.5f); y1 = (int)(v + inv / 2 + 0.5f);
            if (y1 <= y0) y1 = y0 + 1;
        } else if (bil) {
            float f = v - 0.5f;
            y0 = (int)floorf(f); y1 = y0 + 1; ty = (int)((f - y0) * 256);
        } else y0 = (int)floorf(v);
        if (v < 0 || v >= im->h) { for (int x = 0; x < dw; x++) o[x] = 0; continue; }
        if (exact) {                                          /* 1:1 and pixel aligned: plain row copy */
            for (int x = 0; x < dw; x++) o[x] = xa[x] >= 0 && xa[x] < im->w ? im->px[(size_t)y0 * im->w + xa[x]] : 0;
            continue;
        }
        for (int x = 0; x < dw; x++) {
            float u = ox + (x + 0.5f) * inv;
            if (u < 0 || u >= im->w) { o[x] = 0; continue; }
            if (box) {
                int x0 = xa[x] < 0 ? 0 : xa[x], x1 = xb[x] > im->w ? im->w : xb[x];
                int ya = y0 < 0 ? 0 : y0, yb = y1 > im->h ? im->h : y1;
                uint32_t r = 0, g = 0, b = 0, andA = 255, n = 0;
                for (int yy = ya; yy < yb; yy++) {
                    const uint32_t *s = im->px + (size_t)yy * im->w;
                    for (int xx = x0; xx < x1; xx++) {
                        uint32_t p = s[xx];
                        andA &= p >> 24;
                        r += p >> 16 & 255; g += p >> 8 & 255; b += p & 255; n++;
                    }
                }
                if (!n) { o[x] = 0; continue; }
                if (andA == 255) { o[x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n); continue; }
                float fa = 0, fr = 0, fg = 0, fb = 0;          /* transparency in the footprint: alpha-weighted */
                for (int yy = ya; yy < yb; yy++) for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = im->px[(size_t)yy * im->w + xx];
                    float al = (p >> 24) * (1.0f / 255);
                    fa += al; fr += al * (p >> 16 & 255); fg += al * (p >> 8 & 255); fb += al * (p & 255);
                }
                if (fa <= 0.0001f) { o[x] = 0; continue; }
                int A = (int)(fa / n * 255 + 0.5f);
                o[x] = (uint32_t)(A > 255 ? 255 : A) << 24 | (uint32_t)(fr / fa + 0.5f) << 16 | (uint32_t)(fg / fa + 0.5f) << 8 | (uint32_t)(fb / fa + 0.5f);
            } else if (bil) {
                uint32_t p[4] = { px_at(im, xa[x], y0), px_at(im, xb[x], y0), px_at(im, xa[x], y1), px_at(im, xb[x], y1) };
                if ((p[0] & p[1] & p[2] & p[3]) >> 24 == 255) o[x] = lerp(lerp(p[0], p[1], xt[x]), lerp(p[2], p[3], xt[x]), ty) | 0xFF000000u;
                else {
                    int wt[4] = { (256 - xt[x]) * (256 - ty) >> 8, xt[x] * (256 - ty) >> 8, (256 - xt[x]) * ty >> 8, xt[x] * ty >> 8 };
                    o[x] = px_mix(p, wt);
                }
            } else o[x] = im->px[(size_t)y0 * im->w + xa[x]];
        }
    }
    free(xa);
}
