/* jpeg.c - JPEG decoder for libgui (see image.h): baseline and progressive Huffman, 8-bit, greyscale or YCbCr
 * (Adobe transform 0 = plain RGB), any sampling whose factors divide the largest (4:4:4, 4:2:2, 4:2:0, 4:4:0,
 * 4:1:1), restart intervals, EXIF orientation. The IDCT is the integer 13-bit "islow" butterfly; 2:1 chroma is
 * upsampled with the usual triangle filter, so output stays within a level or two of libjpeg. Truncated or corrupt
 * entropy data decodes as far as it goes (the rest stays mid-grey). Not supported: arithmetic coding, 12-bit,
 * lossless, CMYK/YCCK. */
#include "image.h"
#include <stdlib.h>
#include <string.h>

#define NOSAN __attribute__((no_sanitize("signed-integer-overflow")))   /* corrupt coefficients may wrap; output is clamped */

static const uint8_t zz[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13,  6,  7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/* fast[]: 9-bit lookup (len << 8 | sym, 0 = longer); maxcode/off: canonical decode for lengths 10..16 */
struct jhuff { uint16_t fast[512]; int32_t maxcode[17], off[17]; uint8_t val[256]; int ok; };
struct jcomp {
    int id, h, v, tq, td, ta, pred;
    int cw, ch;                         /* real size in samples */
    int bw, bh, pw;                     /* blocks per row / column (MCU padded), plane stride */
    uint8_t *pl;
    int16_t *co;                        /* progressive: all coefficients, natural order, not dequantised */
    uint16_t q[64];                     /* quantiser latched at the first scan, natural order */
    int qok;
};
struct jd {
    const uint8_t *d; size_t n, pos;
    uint32_t acc; int nb, marker, pad;  /* bit reader; pad counts zero bytes fed past the data */
    uint16_t qt[4][64];
    struct jhuff dc[4], ac[4];
    int w, h, nc, prog, hmax, vmax, mcux, mcuy, ri, eobrun, adobe, orient, err, sof;
    struct jcomp c[3];
};

/* ---------- bit reader ---------- */
static void refill(struct jd *j) {
    while (j->nb <= 24) {
        int b = 0;
        if (!j->marker && j->pos < j->n) {
            b = j->d[j->pos];
            if (b == 0xFF) {
                int b2 = j->pos + 1 < j->n ? j->d[j->pos + 1] : -1;
                if (b2 == 0) j->pos += 2;
                else if (b2 == 0xFF) { j->pos++; continue; }       /* fill byte */
                else { j->marker = 1; b = 0; j->pad++; }           /* a marker (or EOF): feed zeros from here */
            } else j->pos++;
        } else j->pad++;
        j->acc = j->acc << 8 | b;
        j->nb += 8;
    }
}

static int getbits(struct jd *j, int n) {       /* 1..16 */
    if (j->nb < n) refill(j);
    j->nb -= n;
    return (j->acc >> j->nb) & ((1 << n) - 1);
}

static int hdec(struct jd *j, const struct jhuff *h) {
    if (j->nb < 16) refill(j);
    int e = h->fast[(j->acc >> (j->nb - 9)) & 511];
    if (e) { j->nb -= e >> 8; return e & 255; }
    for (int l = 10; l <= 16; l++) {
        int c = (j->acc >> (j->nb - l)) & ((1 << l) - 1);
        if (c <= h->maxcode[l]) { j->nb -= l; return h->val[(c + h->off[l]) & 255]; }
    }
    j->err = 1;
    return 0;
}

static int ext(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }   /* sign-extend a t-bit magnitude */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int huff_build(struct jhuff *h, const uint8_t *cnt, const uint8_t *vals) {
    memset(h->fast, 0, sizeof h->fast);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->off[l] = k - code;
        for (int i = 0; i < cnt[l - 1]; i++, k++, code++) {
            if (code >= 1 << l) return -1;
            if (l <= 9)
                for (int f = code << (9 - l); f < (code + 1) << (9 - l); f++) h->fast[f] = l << 8 | vals[k];
        }
        h->maxcode[l] = cnt[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    memcpy(h->val, vals, k);
    h->ok = 1;
    return 0;
}

/* ---------- IDCT (in: dequantised, natural order; out: clamped 8-bit samples) ---------- */
#define F_0_298 2446
#define F_0_390 3196
#define F_0_541 4433
#define F_0_765 6270
#define F_0_899 7373
#define F_1_175 9633
#define F_1_501 12299
#define F_1_847 15137
#define F_1_961 16069
#define F_2_053 16819
#define F_2_562 20995
#define F_3_072 25172
#define DESC(x, n) (((x) + (1 << ((n) - 1))) >> (n))

static inline uint8_t px8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* one 1-D pass over 8 values in[0], in[s], ..; o[] receives the 8 results scaled by 2^13 (unrounded) in out order */
#define IDCT1D(i0, i1, i2, i3, i4, i5, i6, i7, sh0, sh1, O)                                          \
    do {                                                                                             \
        int z2 = i2, z3 = i6, z1 = (z2 + z3) * F_0_541;                                              \
        int t2 = z1 + z3 * -F_1_847, t3 = z1 + z2 * F_0_765;                                         \
        int t0 = (i0 + i4) * 8192, t1 = (i0 - i4) * 8192;                                            \
        int t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;                              \
        t0 = i7; t1 = i5; t2 = i3; t3 = i1;                                                          \
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2; int z4 = t1 + t3, z5 = (z3 + z4) * F_1_175;        \
        t0 *= F_0_298; t1 *= F_2_053; t2 *= F_3_072; t3 *= F_1_501;                                  \
        z1 *= -F_0_899; z2 *= -F_2_562; z3 *= -F_1_961; z4 *= -F_0_390;                              \
        z3 += z5; z4 += z5;                                                                          \
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;                                  \
        O(0, DESC(t10 + t3, sh0)); O(7, DESC(t10 - t3, sh0)); O(1, DESC(t11 + t2, sh0));             \
        O(6, DESC(t11 - t2, sh0)); O(2, DESC(t12 + t1, sh0)); O(5, DESC(t12 - t1, sh0));             \
        O(3, DESC(t13 + t0, sh0)); O(4, DESC(t13 - t0, sh0));                                        \
    } while (0)

NOSAN static void idct_put(const int *in, uint8_t *out, int stride) {
    int ws[64];
    for (int c = 0; c < 8; c++) {                               /* columns, result scaled by 2^2 */
        const int *p = in + c;
        if (!(p[8] | p[16] | p[24] | p[32] | p[40] | p[48] | p[56])) {
            int dc = p[0] * 4;
            for (int r = 0; r < 8; r++) ws[r * 8 + c] = dc;
            continue;
        }
#define OC(k, v) ws[(k) * 8 + c] = (v)
        IDCT1D(p[0], p[8], p[16], p[24], p[32], p[40], p[48], p[56], 11, 11, OC);
#undef OC
    }
    for (int r = 0; r < 8; r++) {                               /* rows: drop 13 + 2 + 3 bits, level shift */
        const int *p = ws + r * 8;
        uint8_t *o = out + r * stride;
        if (!(p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7])) {
            uint8_t v = px8(DESC(p[0], 5) + 128);
            for (int k = 0; k < 8; k++) o[k] = v;
            continue;
        }
#define OR(k, v) o[k] = px8((v) + 128)
        IDCT1D(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 18, 18, OR);
#undef OR
    }
}

/* ---------- scan decoding ---------- */
static void block_base(struct jd *j, struct jcomp *c, int *blk) {
    int t = hdec(j, &j->dc[c->td]);
    if (t > 11) { j->err = 1; return; }
    c->pred = clampi(c->pred + (t ? ext(getbits(j, t), t) : 0), -32768, 32767);
    blk[0] = clampi(c->pred * c->q[0], -2047, 2047);
    for (int k = 1; k < 64 && !j->err;) {
        int rs = hdec(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
        if (!s) { if (r < 15) break; k += 16; continue; }
        k += r;
        if (k > 63) { j->err = 1; break; }
        blk[zz[k]] = clampi(ext(getbits(j, s), s) * c->q[zz[k]], -2047, 2047);
        k++;
    }
}

static void block_prog(struct jd *j, struct jcomp *c, int16_t *co, int ss, int se, int ah, int al) {
    if (ss == 0) {                                              /* DC */
        if (ah == 0) {
            int t = hdec(j, &j->dc[c->td]);
            if (t > 11) { j->err = 1; return; }
            c->pred = clampi(c->pred + (t ? ext(getbits(j, t), t) : 0), -32768, 32767);
            co[0] = clampi(c->pred * (1 << al), -32768, 32767);
        } else if (getbits(j, 1)) co[0] |= 1 << al;
        return;
    }
    const struct jhuff *h = &j->ac[c->ta];
    if (ah == 0) {                                              /* AC, first pass */
        if (j->eobrun > 0) { j->eobrun--; return; }
        for (int k = ss; k <= se && !j->err;) {
            int rs = hdec(j, h), r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += getbits(j, r); break; }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) { j->err = 1; break; }
            co[zz[k]] = clampi(ext(getbits(j, s), s) * (1 << al), -32768, 32767);
            k++;
        }
        return;
    }
    int p1 = 1 << al, m1 = -p1, k = ss;                         /* AC refinement: nudge nonzeros, place new +-1s */
    if (j->eobrun == 0) {
        for (; k <= se && !j->err; k++) {
            int rs = hdec(j, h), r = rs >> 4, s = rs & 15, nv = 0;
            if (!s) {
                if (r < 15) { j->eobrun = 1 << r; if (r) j->eobrun += getbits(j, r); break; }
            } else nv = getbits(j, 1) ? p1 : m1;
            for (; k <= se; k++) {
                int16_t *cf = &co[zz[k]];
                if (*cf) {
                    if (getbits(j, 1) && !(*cf & p1)) *cf += *cf >= 0 ? p1 : m1;
                } else if (--r < 0) break;
            }
            if (nv && k <= 63) co[zz[k]] = nv;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            int16_t *cf = &co[zz[k]];
            if (*cf && getbits(j, 1) && !(*cf & p1)) *cf += *cf >= 0 ? p1 : m1;
        }
        j->eobrun--;
    }
}

/* byte-align at a restart: drop buffered bits, step over the RSTn, reset predictors */
static void restart(struct jd *j, struct jcomp **cs, int ns) {
    j->nb = 0; j->acc = 0; j->pad = 0;
    if (!j->marker)
        while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] && j->d[j->pos + 1] != 0xFF)) j->pos++;
    if (j->pos + 1 < j->n && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) { j->pos += 2; j->marker = 0; }
    for (int i = 0; i < ns; i++) cs[i]->pred = 0;
    j->eobrun = 0;
}

static void scan(struct jd *j, int ns, struct jcomp **cs, int ss, int se, int ah, int al) {
    int single = ns == 1, mx = j->mcux, my = j->mcuy, cnt = 0;
    if (single) { mx = (cs[0]->cw + 7) / 8; my = (cs[0]->ch + 7) / 8; }
    j->eobrun = 0; j->err = 0;
    for (int i = 0; i < ns; i++) cs[i]->pred = 0;
    for (int y = 0; y < my; y++)
        for (int x = 0; x < mx; x++, cnt++) {
            if (j->ri && cnt && cnt % j->ri == 0) restart(j, cs, ns);
            if (j->err || j->pad > 64) return;                  /* corrupt or ran out of data: keep what we have */
            for (int i = 0; i < ns; i++) {
                struct jcomp *c = cs[i];
                int nh = single ? 1 : c->h, nv = single ? 1 : c->v;
                for (int v = 0; v < nv; v++)
                    for (int h = 0; h < nh; h++) {
                        int bx = single ? x : x * c->h + h, by = single ? y : y * c->v + v;
                        if (j->prog) block_prog(j, c, c->co + ((size_t)by * c->bw + bx) * 64, ss, se, ah, al);
                        else {
                            int blk[64] = { 0 };
                            block_base(j, c, blk);
                            idct_put(blk, c->pl + (size_t)by * 8 * c->pw + bx * 8, c->pw);
                        }
                    }
            }
        }
}

/* ---------- markers ---------- */
static void exif(struct jd *j, const uint8_t *p, int n) {       /* orientation tag out of the TIFF header */
    if (n < 14 || memcmp(p, "Exif\0\0", 6)) return;
    const uint8_t *t = p + 6;
    int le = t[0] == 'I', tn = n - 6;
#define U16(o) (le ? t[o] | t[(o) + 1] << 8 : t[o] << 8 | t[(o) + 1])
#define U32(o) (le ? (uint32_t)t[o] | t[(o) + 1] << 8 | t[(o) + 2] << 16 | (uint32_t)t[(o) + 3] << 24 \
                   : (uint32_t)t[o] << 24 | t[(o) + 1] << 16 | t[(o) + 2] << 8 | t[(o) + 3])
    if ((t[0] != 'I' && t[0] != 'M') || U16(2) != 42) return;
    uint32_t o = U32(4);
    if (o > (uint32_t)tn - 2) return;
    int cnt = U16(o);
    for (int i = 0; i < cnt; i++) {
        uint32_t e = o + 2 + 12 * i;
        if (e + 12 > (uint32_t)tn) return;
        if (U16(e) == 0x0112 && U16(e + 2) == 3) { j->orient = U16(e + 8); return; }
    }
#undef U16
#undef U32
}

static int do_sof(struct jd *j, const uint8_t *p, int n, int prog) {
    if (j->sof || n < 6 || p[0] != 8) return -1;
    j->h = p[1] << 8 | p[2]; j->w = p[3] << 8 | p[4]; j->nc = p[5];
    if (!j->w || !j->h || (j->nc != 1 && j->nc != 3) || n < 6 + 3 * j->nc) return -1;
    if ((uint64_t)j->w * j->h > 1u << 25) return -1;
    j->prog = prog;
    for (int i = 0; i < j->nc; i++) {
        struct jcomp *c = &j->c[i];
        c->id = p[6 + 3 * i]; c->h = p[7 + 3 * i] >> 4; c->v = p[7 + 3 * i] & 15; c->tq = p[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) return -1;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->nc; i++) {
        struct jcomp *c = &j->c[i];
        if (j->hmax % c->h || j->vmax % c->v) return -1;
        c->cw = (j->w * c->h + j->hmax - 1) / j->hmax;
        c->ch = (j->h * c->v + j->vmax - 1) / j->vmax;
        c->bw = j->mcux * c->h; c->bh = j->mcuy * c->v; c->pw = c->bw * 8;
        c->pl = malloc((size_t)c->pw * c->bh * 8);
        if (!c->pl) return -1;
        memset(c->pl, 128, (size_t)c->pw * c->bh * 8);
        if (prog && !(c->co = calloc((size_t)c->bw * c->bh * 64, 2))) return -1;
    }
    j->sof = 1;
    return 0;
}

static int do_sos(struct jd *j, const uint8_t *p, int n) {
    if (!j->sof || n < 1) return -1;
    int ns = p[0];
    if (ns < 1 || ns > j->nc || n < 4 + 2 * ns) return -1;
    struct jcomp *cs[3];
    for (int i = 0; i < ns; i++) {
        int k = 0;
        while (k < j->nc && j->c[k].id != p[1 + 2 * i]) k++;
        if (k == j->nc) return -1;
        cs[i] = &j->c[k];
        cs[i]->td = p[2 + 2 * i] >> 4; cs[i]->ta = p[2 + 2 * i] & 15;
        if (cs[i]->td > 3 || cs[i]->ta > 3) return -1;
        if (!cs[i]->qok) { memcpy(cs[i]->q, j->qt[cs[i]->tq], sizeof cs[i]->q); cs[i]->qok = 1; }
    }
    int ss = p[1 + 2 * ns], se = p[2 + 2 * ns], ah = p[3 + 2 * ns] >> 4, al = p[3 + 2 * ns] & 15;
    if (!j->prog) ss = 0, se = 63, ah = al = 0;
    else if (ss > se || se > 63 || al > 13 || (ss == 0 && se != 0) || (ss && ns != 1)) return -1;
    for (int i = 0; i < ns; i++) {                              /* every table the scan will read must exist */
        int dc = !j->prog || (ss == 0 && ah == 0), ac = !j->prog || ss > 0;
        if ((dc && !j->dc[cs[i]->td].ok) || (ac && !j->ac[cs[i]->ta].ok)) return -1;
    }
    j->acc = 0; j->nb = 0; j->marker = 0; j->pad = 0;
    scan(j, ns, cs, ss, se, ah, al);
    return 0;
}

/* ---------- upsampling + colour ---------- */
static void up_h2v1(const uint8_t *in, int n, uint8_t *out) {
    if (n == 1) { out[0] = out[1] = in[0]; return; }
    out[0] = in[0];
    out[1] = (in[0] * 3 + in[1] + 2) >> 2;
    for (int i = 1; i < n - 1; i++) {
        int v = in[i] * 3;
        out[2 * i] = (v + in[i - 1] + 1) >> 2;
        out[2 * i + 1] = (v + in[i + 1] + 2) >> 2;
    }
    out[2 * n - 2] = (in[n - 1] * 3 + in[n - 2] + 1) >> 2;
    out[2 * n - 1] = in[n - 1];
}

static void up_h2v2(const uint8_t *a, const uint8_t *b, int n, uint8_t *out) {    /* a: nearer row (3/4), b: farther (1/4) */
    int this = a[0] * 3 + b[0], next, last;
    if (n == 1) { out[0] = (this * 4 + 8) >> 4; out[1] = (this * 4 + 7) >> 4; return; }
    next = a[1] * 3 + b[1];
    out[0] = (this * 4 + 8) >> 4;
    out[1] = (this * 3 + next + 7) >> 4;
    last = this; this = next;
    for (int i = 1; i < n - 1; i++) {
        next = a[i + 1] * 3 + b[i + 1];
        out[2 * i] = (this * 3 + last + 8) >> 4;
        out[2 * i + 1] = (this * 3 + next + 7) >> 4;
        last = this; this = next;
    }
    out[2 * n - 2] = (this * 3 + last + 8) >> 4;
    out[2 * n - 1] = (this * 4 + 7) >> 4;
}

/* row y of component c at full resolution (a plane row, or buf) */
static const uint8_t *up_row(struct jd *j, struct jcomp *c, int y, uint8_t *buf) {
    int hs = j->hmax / c->h, vs = j->vmax / c->v;
    const uint8_t *r = c->pl + (size_t)(y / vs) * c->pw;
    if (hs == 1 && vs == 1) return r;
    if (hs == 2 && vs == 1) { up_h2v1(r, c->cw, buf); return buf; }
    if (hs == 2 && vs == 2) {
        int r2 = clampi((y & 1) ? (y >> 1) + 1 : (y >> 1) - 1, 0, c->ch - 1);
        up_h2v2(r, c->pl + (size_t)r2 * c->pw, c->cw, buf);
        return buf;
    }
    for (int x = 0; x < j->w; x++) buf[x] = r[x / hs];
    return buf;
}

static int crr[256], cbb[256], crg[256], cbg[256], ycc_ready;

static void ycc_init(void) {
    for (int i = 0; i < 256; i++) {
        int x = i - 128;
        crr[i] = (91881 * x + 32768) >> 16;
        cbb[i] = (116130 * x + 32768) >> 16;
        crg[i] = -46802 * x;
        cbg[i] = -22554 * x + 32768;
    }
    ycc_ready = 1;
}

static int finish(struct jd *j, struct image *im) {
    int W = j->w, H = j->h;
    uint32_t *px = malloc((size_t)W * H * 4);
    size_t rb = (size_t)j->mcux * j->hmax * 8 + 16;
    uint8_t *buf = malloc(rb * 3);
    if (!px || !buf) { free(px); free(buf); return -1; }
    if (!ycc_ready) ycc_init();
    int rgb = j->adobe == 0 || (j->nc == 3 && j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B');
    for (int y = 0; y < H; y++) {
        uint32_t *o = px + (size_t)y * W;
        const uint8_t *r0 = up_row(j, &j->c[0], y, buf);
        if (j->nc == 1) {
            for (int x = 0; x < W; x++) o[x] = 0xFF000000u | r0[x] * 0x010101u;
            continue;
        }
        const uint8_t *r1 = up_row(j, &j->c[1], y, buf + rb), *r2 = up_row(j, &j->c[2], y, buf + 2 * rb);
        if (rgb) for (int x = 0; x < W; x++) o[x] = 0xFF000000u | r0[x] << 16 | r1[x] << 8 | r2[x];
        else for (int x = 0; x < W; x++) {
            int yy = r0[x], cb = r1[x], cr = r2[x];
            o[x] = 0xFF000000u | px8(yy + crr[cr]) << 16 | px8(yy + ((cbg[cb] + crg[cr]) >> 16)) << 8 | px8(yy + cbb[cb]);
        }
    }
    free(buf);
    im->w = W; im->h = H; im->px = px;
    return 0;
}

/* EXIF orientations 2..8 -> pixel moves (Pillow's exif_transpose); on OOM the picture stays as decoded */
static void orient(struct image *im, int o) {
    if (o < 2 || o > 8) return;
    int w = im->w, h = im->h, sw = o >= 5 ? h : w;
    uint32_t *np = malloc((size_t)w * h * 4);
    if (!np) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx, dy;
            switch (o) {
            case 2: dx = w - 1 - x; dy = y; break;
            case 3: dx = w - 1 - x; dy = h - 1 - y; break;
            case 4: dx = x; dy = h - 1 - y; break;
            case 5: dx = y; dy = x; break;
            case 6: dx = h - 1 - y; dy = x; break;
            case 7: dx = h - 1 - y; dy = w - 1 - x; break;
            default: dx = y; dy = w - 1 - x; break;
            }
            np[(size_t)dy * sw + dx] = im->px[(size_t)y * w + x];
        }
    free(im->px);
    im->px = np; im->w = sw; im->h = o >= 5 ? w : h;
}

int jpeg_decode(const uint8_t *d, size_t n, struct image *im) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    struct jd *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->d = d; j->n = n; j->pos = 2; j->orient = 1; j->adobe = -1;
    int rc = -1, scans = 0;
    for (;;) {
        while (j->pos < n && d[j->pos] != 0xFF) j->pos++;
        while (j->pos < n && d[j->pos] == 0xFF) j->pos++;
        if (j->pos >= n) break;
        int m = d[j->pos++];
        if (m == 0 || m == 1 || m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;
        if (j->pos + 2 > n) break;
        size_t len = d[j->pos] << 8 | d[j->pos + 1];
        if (len < 2 || j->pos + len > n) break;                  /* truncated segment */
        const uint8_t *p = d + j->pos + 2;
        int pl = len - 2;
        j->pos += len;
        if (m == 0xDB) {
            while (pl >= 1) {
                int pq = p[0] >> 4, tq = p[0] & 15, sz = pq ? 2 : 1;
                if (tq > 3 || pl < 1 + 64 * sz) goto out;
                for (int i = 0; i < 64; i++) j->qt[tq][zz[i]] = pq ? p[1 + 2 * i] << 8 | p[2 + 2 * i] : p[1 + i];
                p += 1 + 64 * sz; pl -= 1 + 64 * sz;
            }
        } else if (m == 0xC4) {
            while (pl >= 17) {
                int tc = p[0] >> 4, th = p[0] & 15, tot = 0;
                for (int i = 1; i <= 16; i++) tot += p[i];
                if (tc > 1 || th > 3 || tot > 256 || pl < 17 + tot) goto out;
                if (huff_build(tc ? &j->ac[th] : &j->dc[th], p + 1, p + 17)) goto out;
                p += 17 + tot; pl -= 17 + tot;
            }
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (do_sof(j, p, pl, m == 0xC2)) goto out;
        } else if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) goto out;   /* lossless, arithmetic, hierarchical */
        else if (m == 0xDD) { if (pl >= 2) j->ri = p[0] << 8 | p[1]; }
        else if (m == 0xE1) exif(j, p, pl);
        else if (m == 0xEE && pl >= 12 && !memcmp(p, "Adobe", 5)) j->adobe = p[11];
        else if (m == 0xDA) {
            if (do_sos(j, p, pl)) goto out;
            scans++;
        }
    }
    if (!j->sof || !scans) goto out;
    if (j->prog)                                                 /* all scans in: dequantise + IDCT every block */
        for (int i = 0; i < j->nc; i++) {
            struct jcomp *c = &j->c[i];
            if (!c->qok) memcpy(c->q, j->qt[c->tq], sizeof c->q);
            for (int by = 0; by < c->bh; by++)
                for (int bx = 0; bx < c->bw; bx++) {
                    const int16_t *co = c->co + ((size_t)by * c->bw + bx) * 64;
                    int blk[64];
                    for (int k = 0; k < 64; k++) blk[k] = clampi(co[k] * c->q[k], -2047, 2047);
                    idct_put(blk, c->pl + (size_t)by * 8 * c->pw + bx * 8, c->pw);
                }
        }
    rc = finish(j, im);
    if (!rc) orient(im, j->orient);
out:
    for (int i = 0; i < 3; i++) { free(j->c[i].pl); free(j->c[i].co); }
    free(j);
    return rc;
}
