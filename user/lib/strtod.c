/* strtod.c - string -> double, including hex floats, inf and nan */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static int match_ci(const char *p, const char *w) {
    while (*w && tolower((unsigned char)*p) == *w) p++, w++;
    return *w == 0;
}

long double __ldexpl(long double x, int e);       /* math.c */

/* x87 precision control: 53-bit mantissa makes one multiply/divide a correctly rounded double op */
static unsigned short fpu_cw_prec(unsigned short mask_set) {
    unsigned short old, nw;
    __asm__ volatile("fnstcw %0" : "=m"(old));
    nw = (old & ~0x300) | mask_set;
    __asm__ volatile("fldcw %0" : : "m"(nw));
    return old;
}

static double hexfloat(const char *p, const char *s, char **end, int neg) {
    unsigned long long m = 0;
    int nd = 0, exp2 = 0, any = 0, seen_dot = 0;
    for (p += 2;; p++) {                                 /* p is at the "0x" */
        int d = isxdigit((unsigned char)*p) ? (isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10) : -1;
        if (d >= 0) {
            any = 1;
            if (nd < 16) { m = m * 16 + d; if (m) nd++; if (seen_dot) exp2 -= 4; }
            else if (!seen_dot) exp2 += 4;               /* digit dropped from the integer part */
        } else if (*p == '.' && !seen_dot) seen_dot = 1;
        else break;
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (*p == 'p' || *p == 'P') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) { if (e < 100000) e = e * 10 + (*q - '0'); q++; }
            exp2 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    double d = (double)__ldexpl((long double)m, exp2);
    if (__builtin_isinf(d)) errno = ERANGE;
    return neg ? -d : d;
}

/* ---- exact conversion: a bignum holds the decimal digits, the quotient / product is cut to 64 bits plus a sticky
 * flag, and one round-half-even step gives the correctly rounded double (subnormals and ties included). ---- */
#define MAXD 800                                /* digits kept; 767 significant digits decide any double's rounding */
#define NL 136                                  /* limbs: 800 digits, or 10^1126 shifted by 64 */
typedef unsigned int u32;
typedef unsigned long long u64;
struct big { u32 l[NL]; int n; };

static void b_mul(struct big *b, u32 m, u32 add) {          /* b = b * m + add */
    u64 c = add;
    for (int i = 0; i < b->n; i++) { c += (u64)b->l[i] * m; b->l[i] = (u32)c; c >>= 32; }
    if (c) b->l[b->n++] = (u32)c;
}
static void b_mulpow10(struct big *b, int e) {
    static const u32 p[] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
    for (; e >= 9; e -= 9) b_mul(b, 1000000000u, 0);
    b_mul(b, p[e], 0);
}
static int b_bits(const struct big *b) { return b->n ? 32 * b->n - __builtin_clz(b->l[b->n - 1]) : 0; }
static void b_shl(struct big *b, int k) {
    if (!b->n || !k) return;
    int w = k >> 5, r = k & 31, n = b->n;
    b->l[n + w] = 0;
    for (int i = n - 1; i >= 0; i--) {
        u32 v = b->l[i];
        if (r) b->l[i + w + 1] |= v >> (32 - r);
        b->l[i + w] = v << r;
    }
    for (int i = 0; i < w; i++) b->l[i] = 0;
    b->n = n + w + 1;
    while (b->n && !b->l[b->n - 1]) b->n--;
}
static void b_shr1(struct big *b) {
    for (int i = 0; i < b->n; i++) b->l[i] = b->l[i] >> 1 | (i + 1 < b->n ? b->l[i + 1] << 31 : 0);
    if (b->n && !b->l[b->n - 1]) b->n--;
}
static int b_cmp(const struct big *a, const struct big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->l[i] != b->l[i]) return a->l[i] < b->l[i] ? -1 : 1;
    return 0;
}
static void b_sub(struct big *a, const struct big *b) {      /* a -= b, a >= b */
    long long c = 0;
    for (int i = 0; i < a->n; i++) {
        c += (long long)a->l[i] - (i < b->n ? b->l[i] : 0);
        a->l[i] = (u32)c; c >>= 32;
    }
    while (a->n && !a->l[a->n - 1]) a->n--;
}

/* q has its top bit set and stands for 1.xxx * 2^E (plus a tail below its last bit when sticky) */
static double finish(u64 q, int E, int sticky) {
    int nb = E >= -1022 ? 53 : 53 - (-1022 - E);             /* mantissa bits that fit (subnormals have fewer) */
    if (nb < 0) return 0.0;
    u64 m = 0;
    int up;
    if (!nb) up = q != 1ull << 63 || sticky;                 /* between 2^-1075 and 2^-1074: ties go to zero */
    else {
        int drop = 64 - nb;
        u64 low = q & ((1ull << drop) - 1), half = 1ull << (drop - 1);
        m = q >> drop;
        up = low > half || (low == half && (sticky || (m & 1)));
    }
    return (double)__ldexpl((long double)(m + up), E - nb + 1);
}

/* dig: nd decimal digits (first one non-zero), value = dig * 10^e10 */
static double exact(const char *dig, int nd, int e10) {
    if (nd + e10 > 310) return __builtin_inf();
    if (nd + e10 < -330) return 0.0;
    struct big N = { .n = 0 }, D;
    for (int i = 0; i < nd; ) {
        u32 c = 0, m = 1;
        for (int k = 0; k < 9 && i < nd; k++, i++) { c = c * 10 + (dig[i] - '0'); m *= 10; }
        b_mul(&N, m, c);
    }
    if (e10 >= 0) {
        b_mulpow10(&N, e10);
        int L = b_bits(&N), sticky = 0;
        u64 q;
        if (L <= 64) q = (N.n > 1 ? (u64)N.l[1] << 32 | N.l[0] : N.l[0]) << (64 - L);
        else {                                               /* the top 64 bits, and whether anything below them is set */
            int sh = L - 64, lo = sh >> 5, r = sh & 31;
            u32 w0 = N.l[lo], w1 = lo + 1 < N.n ? N.l[lo + 1] : 0, w2 = lo + 2 < N.n ? N.l[lo + 2] : 0;
            for (int i = 0; i < lo; i++) if (N.l[i]) sticky = 1;
            if (r && (w0 & ((1u << r) - 1))) sticky = 1;
            u32 lo32 = (w0 >> r) | (r ? w1 << (32 - r) : 0), hi32 = (w1 >> r) | (r ? w2 << (32 - r) : 0);
            q = (u64)hi32 << 32 | lo32;
        }
        return finish(q, L - 1, sticky);
    }
    D.n = 1; D.l[0] = 1;
    b_mulpow10(&D, -e10);
    int s = 63 - (b_bits(&N) - b_bits(&D));                  /* so that N/D * 2^s has 63 or 64 bits */
    if (s > 0) b_shl(&N, s); else if (s < 0) b_shl(&D, -s);
    struct big T = D;
    b_shl(&T, 63);
    u64 q = 0;
    for (int i = 63; i >= 0; i--) {                          /* restoring division, one quotient bit per step */
        if (b_cmp(&N, &T) >= 0) { b_sub(&N, &T); q |= 1ull << i; }
        b_shr1(&T);
    }
    int p = 63 - __builtin_clzll(q);
    return finish(q << (63 - p), p - s, N.n != 0);
}

/* Up to 15 digits and |exp10| <= 22 (the classic fast path) is exactly rounded by one x87 operation; everything
 * else goes through exact(). ponytail: digits past MAXD fold into one sticky digit; no locale decimal point. */
double strtod(const char *s, char **end) {
    const char *p = s;
    int neg = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (match_ci(p, "inf")) {
        p += match_ci(p, "infinity") ? 8 : 3;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (match_ci(p, "nan")) {
        if (end) *end = (char *)(p + 3);
        return __builtin_nan("");
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *q = p + 2;
        if (isxdigit((unsigned char)*q) || (*q == '.' && isxdigit((unsigned char)q[1])))
            return hexfloat(p, s, end, neg);
    }
    char dig[MAXD + 2];
    int nd = 0, exp10 = 0, any = 0, seen_dot = 0, sticky = 0;
    for (;; p++) {
        if (isdigit((unsigned char)*p)) {
            any = 1;
            if (nd < MAXD) { if (nd || *p != '0') dig[nd++] = *p; if (seen_dot) exp10--; }
            else { if (*p != '0') sticky = 1; if (!seen_dot) exp10++; }   /* digit dropped */
        } else if (*p == '.' && !seen_dot) seen_dot = 1;
        else break;
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) { if (e < 100000) e = e * 10 + (*q - '0'); q++; }
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    double d;
    if (!nd) d = 0;
    else if (nd <= 15 && exp10 >= -22 && exp10 <= 22) {
        static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
        u64 m = 0;
        for (int i = 0; i < nd; i++) m = m * 10 + (dig[i] - '0');
        unsigned short old = fpu_cw_prec(0x200);
        volatile double v = (double)m;
        v = exp10 < 0 ? v / p10[-exp10] : v * p10[exp10];
        d = v;
        __asm__ volatile("fldcw %0" : : "m"(old));
    } else {
        if (sticky) { dig[nd++] = '1'; exp10--; }            /* a non-zero tail: one more digit keeps every tie/halfway decision right */
        d = exact(dig, nd, exp10);
        if (d == 0) errno = ERANGE;
    }
    if (__builtin_isinf(d)) errno = ERANGE;
    return neg ? -d : d;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
/* ponytail: strtold is strtod widened (53-bit mantissa); tcc only needs it to exist for long double literals */
long double strtold(const char *s, char **end) { return strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

