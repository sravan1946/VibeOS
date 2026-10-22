/* compat2.c - libm functions MicroPython's math module needs beyond math.c: copysign nearbyint expm1 log1p
 * acosh asinh atanh erf erfc tgamma lgamma nan, and fsync. Worked in x87 long double so the double results are close to exact. */
#include <math.h>
#include <stdint.h>
#include <unistd.h>

typedef long double ld;

static ld l_abs(ld x) { return x < 0 ? -x : x; }

static ld l_rint(ld x) { __asm__("frndint" : "=t"(x) : "0"(x)); return x; }
static ld l_sqrt(ld x) { __asm__("fsqrt" : "=t"(x) : "0"(x)); return x; }
static ld l_yl2x(ld x, ld y) { ld r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static ld l_yl2xp1(ld x, ld y) { ld r; __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static ld l_2xm1(ld x) { __asm__("f2xm1" : "=t"(x) : "0"(x)); return x; }   /* |x| <= 1 */
static ld l_exp2(ld x) {
    ld i = l_rint(x), f = l_2xm1(x - i) + 1;
    __asm__("fscale; fstp %%st(1)" : "=t"(f) : "0"(f), "u"(i) : "st(1)");
    return f;
}

#define LN2 0.693147180559945309417232121458176568L
#define LOG2E 1.44269504088896340735992468100189214L
#define PI 3.14159265358979323846264338327950288L

static ld l_log(ld x) { return l_yl2x(x, LN2); }
static ld l_log1p(ld x) { return l_abs(x) < 0.29L ? l_yl2xp1(x, LN2) : l_yl2x(1 + x, LN2); }
static ld l_exp(ld x) { return l_exp2(x * LOG2E); }

static ld sinl_pi(ld x) {                                       /* sin(pi x), reduced exactly first */
    x -= 2 * l_rint(x / 2);
    ld r = x * PI;
    __asm__("fsin" : "=t"(r) : "0"(r));
    return r;
}

double copysign(double x, double y) {
    union { double d; uint64_t u; } a = { x }, b = { y };
    a.u = (a.u & ~(1ull << 63)) | (b.u & 1ull << 63);
    return a.d;
}

double nan(const char *tag) { return NAN; }

double nearbyint(double x) { return isfinite(x) ? copysign(l_rint(x), x) : x; }   /* ties to even, keeps -0 */

double expm1(double x) {
    if (isnan(x) || x == INFINITY) return x;
    if (x < -50) return -1;
    ld t = x * LOG2E;
    return l_abs(t) <= 1 ? l_2xm1(t) : l_exp2(t) - 1;
}

double log1p(double x) {
    if (isnan(x) || x == INFINITY) return x;
    if (x == -1) return -INFINITY;
    if (x < -1) return NAN;
    return l_log1p(x);
}

double asinh(double x) {
    if (!isfinite(x) || x == 0) return x;
    ld a = l_abs(x);
    ld r = a > 1e18L ? l_log(a) + LN2 : l_log1p(a + a * a / (1 + l_sqrt(1 + a * a)));
    return x < 0 ? -r : r;
}

double acosh(double x) {
    if (x < 1) return NAN;
    if (!isfinite(x)) return x;
    ld t = (ld)x - 1;
    return x > 1e18 ? l_log(x) + LN2 : l_log1p(t + l_sqrt(2 * t + t * t));
}

double atanh(double x) {
    ld a = l_abs(x);
    if (a > 1 || isnan(x)) return NAN;
    if (a == 1) return x > 0 ? INFINITY : -INFINITY;
    ld r = 0.5L * l_log1p(2 * a / (1 - a));
    return x < 0 ? -r : r;
}

/* erf by its Taylor series below ERF_CUT, erfc by a continued fraction (modified Lentz) above */
#define ERF_CUT 1.5L
static ld erf_series(ld x) {
    ld sum = x, term = x, x2 = x * x;
    for (int n = 1; n < 200; n++) {
        term *= -x2 / n;
        ld d = term / (2 * n + 1);
        sum += d;
        if (l_abs(d) < 1e-22L * l_abs(sum)) break;
    }
    return sum * 1.12837916709551257389615890312154517L;          /* 2/sqrt(pi) */
}

static ld erfc_cf(ld x) {                                       /* x >= ERF_CUT */
    ld f = x, c = x, d = 0;
    for (int n = 1; n < 5000; n++) {
        ld an = n * 0.5L;
        d = x + an * d; d = d == 0 ? 1e-300L : 1 / d;
        c = x + an / c; if (c == 0) c = 1e-300L;
        ld delta = c * d;
        f *= delta;
        if (l_abs(delta - 1) < 1e-21L) break;
    }
    return l_exp(-(ld)x * x) / (f * 1.77245385090551602729816748334114518L);   /* sqrt(pi) */
}

double erf(double x) {
    if (isnan(x)) return x;
    ld a = l_abs(x);
    ld r = a > 6 ? 1 : a < ERF_CUT ? erf_series(a) : 1 - erfc_cf(a);
    return copysign(r, x);
}

double erfc(double x) {
    if (isnan(x)) return x;
    if (x < -6) return 2;
    if (x < ERF_CUT) return 1 - (x < 0 ? -erf_series(-(ld)x) : erf_series(x));
    if (x > 27.3) return 0;
    return erfc_cf(x);
}

/* ln Gamma(x) for x > 0: shift x up past 12 (Gamma(x) = Gamma(x + n) / (x (x+1) ... (x+n-1))), then Stirling's series.
 * *p gets the shift product, so ln Gamma(x) = result - ln(*p). */
static ld lgamma_shifted(ld x, ld *p) {
    *p = 1;
    for (; x < 12; x++) *p *= x;
    ld r = 1 / x, r2 = r * r;
    ld s = r * (1.0L / 12 - r2 * (1.0L / 360 - r2 * (1.0L / 1260 - r2 * (1.0L / 1680 - r2 * (1.0L / 1188 - r2 * (691.0L / 360360
           - r2 * (1.0L / 156 - r2 * 3617.0L / 122400)))))));
    return (x - 0.5L) * l_log(x) - x + 0.918938533204672741780329736405617639L + s;   /* ln sqrt(2pi) */
}

double tgamma(double x) {
    if (isnan(x) || x == INFINITY) return x;
    if (x == 0) return copysign(INFINITY, x);
    if (x < 0 && x == l_rint(x)) return NAN;
    if (x > 171.7) return INFINITY;
    if (x == l_rint(x) && x <= 30) {                            /* exact factorials */
        ld p = 1;
        for (int i = 2; i < x; i++) p *= i;
        return p;
    }
    ld p, lg;
    if (x < 0.5) {                                              /* reflection, with 1 - x kept exact */
        lg = lgamma_shifted(1 - (ld)x, &p);
        return PI * p / (sinl_pi(x) * l_exp(lg));
    }
    lg = lgamma_shifted(x, &p);
    return l_exp(lg) / p;
}

double lgamma(double x) {
    if (isnan(x) || isinf(x)) return fabs(x);
    if (x <= 0 && x == l_rint(x)) return INFINITY;
    if (x == 1 || x == 2) return 0;
    ld p, lg;
    if (x < 0.5) {
        lg = lgamma_shifted(1 - (ld)x, &p);
        return l_log(PI / l_abs(sinl_pi(x))) - (lg - l_log(p));
    }
    lg = lgamma_shifted(x, &p);
    return lg - l_log(p);
}

int fsync(int fd) { return 0; }            /* ext2 writes go to the disk as they happen */
