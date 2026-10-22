/* printf.c - the formatting engine (__vformat) and the string-targeting printf family.
 * Floating point is converted exactly (big-integer arithmetic, round-half-even) so output
 * matches glibc digit for digit; precision is capped at 340 digits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stddef.h>

typedef void (*sink_t)(void *ctx, const char *s, size_t n);
int __vformat(sink_t out, void *ctx, const char *fmt, va_list ap);

/* ---- exact double -> decimal digits ---- */

#define SMAX 1800
#define FMAX 40
struct digs {
    char s[SMAX];           /* s[0] is a '0' guard for carries, then integer digits, then fraction digits */
    int n, nint, sticky;    /* sticky: value continues past s[n-1] with nonzero digits */
};

static int nz(const uint32_t *f, int w) {
    for (int i = 0; i < w; i++) if (f[i]) return 1;
    return 0;
}

/* mode_f: generate prec+2 fraction digits; else enough for prec+2 significant digits */
static void build(struct digs *d, double v, int mode_f, int prec) {
    union { double d; uint64_t u; } x = { v };
    int ex = (x.u >> 52) & 0x7ff, e;
    uint64_t m = x.u & ((1ull << 52) - 1);
    if (ex) { m |= 1ull << 52; e = ex - 1075; } else e = -1074;
    char *s = d->s;
    int n = 1, k = 0;
    uint32_t F[FMAX] = { 0 };
    s[0] = '0';
    d->nint = 0;
    if (m && e >= 0) {                                  /* integer, base 1e9 limbs */
        uint32_t L[FMAX];
        int nl = 0;
        for (uint64_t t = m; t; t /= 1000000000u) L[nl++] = t % 1000000000u;
        for (int sh = e; sh > 0;) {
            int b = sh > 29 ? 29 : sh;
            uint64_t c = 0;
            sh -= b;
            for (int i = 0; i < nl; i++) {
                uint64_t y = ((uint64_t)L[i] << b) + c;
                L[i] = y % 1000000000u;
                c = y / 1000000000u;
            }
            for (; c; c /= 1000000000u) L[nl++] = c % 1000000000u;
        }
        for (int i = nl - 1; i >= 0; i--) {
            char t[9];
            int w = 0;
            for (uint32_t q = L[i]; q; q /= 10) t[w++] = '0' + q % 10;
            if (i != nl - 1) while (w < 9) t[w++] = '0';
            while (w) s[n++] = t[--w];
        }
        d->nint = n - 1;
    } else if (m) {                                     /* m * 2^e with e < 0 */
        k = -e;
        uint64_t ip = k < 64 ? m >> k : 0, fr = k < 64 ? m & ((1ull << k) - 1) : m;
        char t[24];
        int w = 0;
        for (; ip; ip /= 10) t[w++] = '0' + ip % 10;
        d->nint = w;
        while (w) s[n++] = t[--w];
        F[0] = (uint32_t)fr;
        F[1] = (uint32_t)(fr >> 32);
    }
    int first = d->nint ? 1 : -1, fdig = 0, W = k / 32 + 2, wi = k / 32, sh = k % 32;
    while (k && nz(F, W)) {
        if (mode_f ? fdig >= prec + 2 : (first >= 0 && n - first >= prec + 2)) break;
        uint32_t c = 0;
        for (int i = 0; i < W; i++) {                   /* F *= 10 */
            uint64_t y = (uint64_t)F[i] * 10 + c;
            F[i] = (uint32_t)y;
            c = y >> 32;
        }
        int dg = (int)((((uint64_t)F[wi + 1] << 32 | F[wi]) >> sh) & 0xf);
        F[wi] &= sh ? (1u << sh) - 1 : 0;
        F[wi + 1] = 0;
        s[n++] = '0' + dg;
        fdig++;
        if (first < 0 && dg) first = n - 1;
    }
    d->n = n;
    d->sticky = k && nz(F, W);
}

/* round s[0..n) to pos digits, half-even on the exact value */
static void round_at(struct digs *d, int pos) {
    char *s = d->s;
    int dg = pos < d->n ? s[pos] - '0' : 0, rest = d->sticky;
    for (int i = pos + 1; i < d->n && !rest; i++) rest = s[i] != '0';
    int up = dg > 5 || (dg == 5 && (rest || ((s[pos - 1] - '0') & 1)));
    while (d->n < pos) s[d->n++] = '0';
    d->n = pos;
    if (up) {
        int i = pos - 1;
        while (s[i] == '9') s[i--] = '0';
        s[i]++;
    }
}

static int fmt_f(char *out, double v, int prec, int alt) {
    struct digs d;
    build(&d, v, 1, prec);
    round_at(&d, 1 + d.nint + prec);
    int end = 1 + d.nint, i = 0, o = 0;
    while (i < end - 1 && d.s[i] == '0') i++;
    while (i < end) out[o++] = d.s[i++];
    if (prec > 0 || alt) out[o++] = '.';
    for (i = 0; i < prec; i++) out[o++] = d.s[end + i];
    return o;
}

static int fmt_e(char *out, double v, int prec, int alt, int *xp) {
    struct digs d;
    build(&d, v, 0, prec);
    int f0 = 1, x = 0, o = 0;
    while (f0 < d.n && d.s[f0] == '0') f0++;
    char *dig = NULL;                                   /* NULL: value is zero */
    if (f0 != d.n) {
        x = d.nint - f0;
        round_at(&d, f0 + prec + 1);
        if (d.s[f0 - 1] != '0') { f0--; x++; }          /* carry ate a new leading digit */
        dig = d.s + f0;
    }
    out[o++] = dig ? dig[0] : '0';
    if (prec > 0 || alt) out[o++] = '.';
    for (int i = 1; i <= prec; i++) out[o++] = dig ? dig[i] : '0';
    out[o++] = 'e';
    if (xp) *xp = x;
    out[o++] = x < 0 ? '-' : '+';
    if (x < 0) x = -x;
    if (x >= 100) out[o++] = '0' + x / 100;
    out[o++] = '0' + x / 10 % 10;
    out[o++] = '0' + x % 10;
    return o;
}

/* drop trailing fraction zeros (and a bare '.') from a %f / %e body */
static int strip_zeros(char *b, int len) {
    int ep = len;
    for (int i = 0; i < len; i++) if (b[i] == 'e') { ep = i; break; }
    int m = ep;
    if (memchr(b, '.', ep)) {
        while (b[m - 1] == '0') m--;
        if (b[m - 1] == '.') m--;
    }
    memmove(b + m, b + ep, len - ep);
    return m + len - ep;
}

static int fmt_g(char *out, double v, int prec, int alt) {
    int p = prec ? prec : 1, x, len = fmt_e(out, v, p - 1, alt, &x);
    if (x < -4 || x >= p) return alt ? len : strip_zeros(out, len);
    len = fmt_f(out, v, p - 1 - x, alt);
    return alt ? len : strip_zeros(out, len);
}

/* %a: hex mantissa digits + exponent, glibc style ("0x1.8p+1"; subnormals keep a 0 lead digit).
 * v is non-negative and finite; the "0x" is added by the caller. */
static int fmt_a(char *out, double v, int prec, int alt) {
    union { double d; uint64_t u; } x = { v };
    int ex = (x.u >> 52) & 0x7ff, e = ex ? ex - 1023 : (x.u << 1 ? -1022 : 0), o = 0;
    uint64_t m = (x.u & ((1ull << 52) - 1)) | (ex ? 1ull << 52 : 0);   /* lead digit + 13 hex digits */
    int nd = 13;
    if (prec >= 0 && prec < 13) {                       /* round half-even to prec hex digits */
        int sh = (13 - prec) * 4;
        uint64_t rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        m >>= sh;
        if (rem > half || (rem == half && (m & 1))) m++;
        nd = prec;
    } else if (prec < 0) {
        while (nd && !(m & 0xf)) { m >>= 4; nd--; }     /* shortest exact */
    }
    out[o++] = '0' + (char)(m >> (nd * 4));
    if (nd > 0 || prec > 13 || alt) out[o++] = '.';
    for (int i = nd - 1; i >= 0; i--) out[o++] = "0123456789abcdef"[(m >> (i * 4)) & 15];
    for (int i = 13; i < prec; i++) out[o++] = '0';
    out[o++] = 'p';
    out[o++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    char t[8];
    int w = 0;
    do t[w++] = '0' + e % 10; while (e /= 10);
    while (w) out[o++] = t[--w];
    return o;
}

/* ---- the engine ---- */

static void pad(sink_t out, void *ctx, char c, int n) {
    char b[16];
    memset(b, c, sizeof b);
    while (n > 0) {
        int k = n < 16 ? n : 16;
        out(ctx, b, k);
        n -= k;
    }
}

/* [spaces][prefix][zeros][body][spaces]; returns chars written */
static int emit(sink_t out, void *ctx, int left, int zero, int w, const char *pre, int plen,
                int zeros, const char *body, int blen) {
    int len = plen + zeros + blen, sp = w > len ? w - len : 0, ret = len + sp;
    if (zero && !left) { zeros += sp; sp = 0; }
    if (!left) pad(out, ctx, ' ', sp);
    out(ctx, pre, plen);
    pad(out, ctx, '0', zeros);
    out(ctx, body, blen);
    if (left) pad(out, ctx, ' ', sp);
    return ret;
}

enum { L_NONE, L_HH, L_H, L_L, L_LL, L_Z, L_J, L_T, L_BIGL };

int __vformat(sink_t out, void *ctx, const char *fmt, va_list ap) {
    int total = 0;
    while (*fmt) {
        const char *p = fmt;
        while (*p && *p != '%') p++;
        if (p > fmt) { out(ctx, fmt, p - fmt); total += p - fmt; fmt = p; }
        if (!*fmt) break;
        fmt++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, w = 0, prec = -1, len = L_NONE;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        if (*fmt == '*') {
            w = va_arg(ap, int);
            if (w < 0) { left = 1; w = -w; }
            fmt++;
        } else while (*fmt >= '0' && *fmt <= '9') w = w * 10 + *fmt++ - '0';
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + *fmt++ - '0';
        }
        switch (*fmt) {
        case 'h': len = L_H; fmt++; if (*fmt == 'h') { len = L_HH; fmt++; } break;
        case 'l': len = L_L; fmt++; if (*fmt == 'l') { len = L_LL; fmt++; } break;
        case 'z': len = L_Z; fmt++; break;
        case 'j': len = L_J; fmt++; break;
        case 't': len = L_T; fmt++; break;
        case 'L': len = L_BIGL; fmt++; break;
        }
        char c = *fmt;
        if (!c) break;
        fmt++;
        char tmp[72], fb[768];
        const char *pre = "", *body = tmp;
        int plen = 0, blen = 0, zeros = 0;

        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : 10, neg = 0;
            uintmax_t u;
            if (c == 'd' || c == 'i') {
                intmax_t v;
                switch (len) {
                case L_LL: v = va_arg(ap, long long); break;
                case L_L: v = va_arg(ap, long); break;
                case L_J: v = va_arg(ap, intmax_t); break;
                case L_Z: case L_T: v = va_arg(ap, ptrdiff_t); break;
                case L_H: v = (short)va_arg(ap, int); break;
                case L_HH: v = (signed char)va_arg(ap, int); break;
                default: v = va_arg(ap, int);
                }
                neg = v < 0;
                u = neg ? -(uintmax_t)v : (uintmax_t)v;
                pre = neg ? "-" : plus ? "+" : space ? " " : "";
                plen = *pre != 0;
            } else {
                switch (len) {
                case L_LL: u = va_arg(ap, unsigned long long); break;
                case L_L: u = va_arg(ap, unsigned long); break;
                case L_J: u = va_arg(ap, uintmax_t); break;
                case L_Z: case L_T: u = va_arg(ap, size_t); break;
                case L_H: u = (unsigned short)va_arg(ap, unsigned); break;
                case L_HH: u = (unsigned char)va_arg(ap, unsigned); break;
                default: u = va_arg(ap, unsigned);
                }
                if (alt && base == 16 && u) { pre = c == 'X' ? "0X" : "0x"; plen = 2; }
            }
            const char *dg = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char *e = tmp + sizeof tmp, *q = e;
            if (u >> 32 == 0) { uint32_t v = (uint32_t)u; while (v) { *--q = dg[v % base]; v /= base; } }
            else while (u) { *--q = dg[u % base]; u /= base; }
            blen = e - q;
            body = q;
            if (prec == 0 && blen == 0 && !(alt && base == 8)) { body = ""; }   /* "%.0d" of 0 prints nothing */
            else if (blen == 0) { *--q = '0'; body = q; blen = 1; }
            if (prec > blen) zeros = prec - blen;
            if (alt && base == 8 && !zeros && body[0] != '0') zeros = 1;
            if (prec >= 0) zero = 0;
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            blen = 1;
            zero = 0;
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            body = s;
            blen = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s);
            zero = 0;
            break;
        }
        case 'p': {
            uintptr_t u = (uintptr_t)va_arg(ap, void *);
            zero = 0;
            if (!u) { body = "(nil)"; blen = 5; break; }
            char *e = tmp + sizeof tmp, *q = e;
            for (; u; u >>= 4) *--q = "0123456789abcdef"[u & 15];
            body = q;
            blen = e - q;
            pre = "0x";
            plen = 2;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = len == L_BIGL ? (double)va_arg(ap, long double) : va_arg(ap, double);
            int up = c == 'F' || c == 'E' || c == 'G' || c == 'A';
            body = fb;
            if (__builtin_signbit(v)) { pre = "-"; plen = 1; v = -v; }
            else if (plus) { pre = "+"; plen = 1; }
            else if (space) { pre = " "; plen = 1; }
            if (__builtin_isnan(v) || __builtin_isinf(v)) {
                memcpy(fb, __builtin_isnan(v) ? (up ? "NAN" : "nan") : (up ? "INF" : "inf"), 3);
                blen = 3;
                zero = 0;
                break;
            }
            if (c == 'a' || c == 'A') {
                if (prec > 100) prec = 100;
                blen = fmt_a(fb, v, prec, alt);
                memcpy(tmp, pre, plen);                  /* sign, then 0x: zero padding goes between */
                tmp[plen++] = '0';
                tmp[plen++] = up ? 'X' : 'x';
                tmp[plen] = 0;
                pre = tmp;
                if (up) for (int i = 0; i < blen; i++) fb[i] = toupper((unsigned char)fb[i]);
                break;
            }
            if (prec < 0) prec = 6;
            if (prec > 340) prec = 340;     /* ponytail: digits past 340 would need a bigger buffer */
            if (c == 'f' || c == 'F') blen = fmt_f(fb, v, prec, alt);
            else if (c == 'e' || c == 'E') blen = fmt_e(fb, v, prec, alt, NULL);
            else blen = fmt_g(fb, v, prec, alt);
            if (up) for (int i = 0; i < blen; i++) if (fb[i] == 'e') fb[i] = 'E';
            break;
        }
        case 'n':
            *va_arg(ap, int *) = total;
            continue;
        case '%':
            tmp[0] = '%';
            blen = 1;
            zero = 0;
            break;
        default:                                         /* unknown: echo it */
            tmp[0] = '%';
            tmp[1] = c;
            blen = 2;
            zero = 0;
        }
        total += emit(out, ctx, left, zero, w, pre, plen, zeros, body, blen);
    }
    return total;
}

/* ---- string targets ---- */

struct sn { char *b; size_t cap, n; };

static void sn_out(void *c, const char *s, size_t k) {
    struct sn *x = c;
    if (x->cap && x->n + 1 < x->cap) {
        size_t room = x->cap - 1 - x->n;
        memcpy(x->b + x->n, s, k < room ? k : room);
    }
    x->n += k;
}

int vsnprintf(char *b, size_t cap, const char *fmt, va_list ap) {
    struct sn x = { b, cap, 0 };
    int r = __vformat(sn_out, &x, fmt, ap);
    if (cap) b[x.n < cap ? x.n : cap - 1] = 0;
    return r;
}

int snprintf(char *b, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(b, cap, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *b, const char *fmt, va_list ap) { return vsnprintf(b, (size_t)-1 >> 1, fmt, ap); }

int sprintf(char *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(b, fmt, ap);
    va_end(ap);
    return r;
}

int vasprintf(char **out, const char *fmt, va_list ap) {
    va_list c;
    va_copy(c, ap);
    int n = vsnprintf(NULL, 0, fmt, c);
    va_end(c);
    *out = malloc(n + 1);
    if (!*out) return -1;
    return vsnprintf(*out, n + 1, fmt, ap);
}

int asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}
