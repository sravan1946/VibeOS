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

/* Up to 15 digits and |exp10| <= 22 (the classic fast path) is exactly rounded; everything else
 * accumulates 19 digits and scales in x87 long double, so it is good to ~1 ulp.
 * ponytail: not correctly rounded for long inputs; a bignum path (as in printf.c) would fix it. */
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
    unsigned long long m = 0;
    int nd = 0, exp10 = 0, any = 0, seen_dot = 0;
    for (;; p++) {
        if (isdigit((unsigned char)*p)) {
            any = 1;
            if (nd < 19) { m = m * 10 + (*p - '0'); if (m) nd++; if (seen_dot) exp10--; }
            else if (!seen_dot) exp10++;                /* digit dropped from the integer part */
        } else if (*p == '.' && !seen_dot) seen_dot = 1;
        else break;
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) { if (e < 10000) e = e * 10 + (*q - '0'); q++; }
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    double d;
    if (m == 0) d = 0;
    else if (nd <= 15 && exp10 >= -22 && exp10 <= 22) {
        static const double p10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
        unsigned short old = fpu_cw_prec(0x200);
        volatile double v = (double)m;
        v = exp10 < 0 ? v / p10[-exp10] : v * p10[exp10];
        d = v;
        __asm__ volatile("fldcw %0" : : "m"(old));
    } else {
        long double v = m;
        int e = exp10 < 0 ? -exp10 : exp10;
        long double pw = 1, b = 10;
        if (e > 5000) e = 5000;
        for (; e; e >>= 1, b *= b)
            if (e & 1) pw *= b;
        v = exp10 < 0 ? v / pw : v * pw;
        d = (double)v;
    }
    if (__builtin_isinf(d)) errno = ERANGE;
    return neg ? -d : d;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

