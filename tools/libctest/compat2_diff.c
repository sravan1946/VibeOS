/* compat2_diff.c - user/lib/compat2.c (copysign nearbyint expm1 log1p asinh acosh atanh erf erfc tgamma lgamma
 * fsync nan) against glibc over random inputs. Built 64-bit with the functions renamed to vibe_* (x87 asm runs on x86-64). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define F1(n) double vibe_##n(double);
F1(nearbyint) F1(expm1) F1(log1p) F1(asinh) F1(acosh) F1(atanh) F1(erf) F1(erfc) F1(tgamma) F1(lgamma)
double vibe_copysign(double, double);
double vibe_nan(const char *);

static int bad;

static double ulps(double a, double b) {
    if (a == b || (isnan(a) && isnan(b))) return 0;
    if (isnan(a) || isnan(b) || isinf(a) || isinf(b)) return 1e30;
    double u = fabs(nextafter(b, INFINITY) - b);
    if (u == 0 || fabs(b) < 2.2250738585072014e-308) u = 5e-324;
    return fabs(a - b) / u;
}

static double rnd(double lo, double hi) { return lo + (hi - lo) * (rand() / (double)RAND_MAX); }

#define T1(name, lo, hi, lim) do { double mx = 0, wx = 0; \
    for (int i = 0; i < 20000; i++) { double x = rnd(lo, hi), u = ulps(vibe_##name(x), name(x)); \
        if (u > mx) { mx = u; wx = x; } } \
    printf("%-9s [%g, %g] max %7.2f ulp (at %.17g)%s\n", #name, (double)lo, (double)hi, mx, wx, mx > lim ? "  <-- FAIL" : ""); \
    if (mx > lim) bad++; } while (0)
#define EQ(expr, want) do { double g = (expr), w = (want); \
    if (!(g == w && signbit(g) == signbit(w)) && !(isnan(g) && isnan(w))) { printf("FAIL %s = %.17g, want %.17g\n", #expr, g, w); bad++; } } while (0)

int main(void) {
    srand(11);
    T1(nearbyint, -1e6, 1e6, 0);
    T1(expm1, -1e-6, 1e-6, 1);
    T1(expm1, -40, 700, 2);
    T1(log1p, -0.999, 1e-6, 1);
    T1(log1p, -0.5, 1e300, 1);
    T1(asinh, -1e-4, 1e-4, 1);
    T1(asinh, -1e300, 1e300, 1);
    T1(acosh, 1, 2, 1);
    T1(acosh, 1, 1e300, 1);
    T1(atanh, -0.999999, 0.999999, 1);
    T1(erf, -0.5, 0.5, 1);
    T1(erf, -6, 6, 2);
    T1(erfc, -3, 2.5, 2);
    T1(erfc, 2.5, 27, 2);
    T1(tgamma, 0.5, 171, 4);
    T1(tgamma, -30.5, 0.5, 4);
    T1(lgamma, 0.5, 1e5, 8);
    T1(lgamma, -30.5, 0.5, 16);
    T1(lgamma, 3, 1e300, 2);
    EQ(vibe_nearbyint(2.5), 2); EQ(vibe_nearbyint(3.5), 4); EQ(vibe_nearbyint(-0.4), -0.0);
    EQ(vibe_copysign(3, -0.0), -3); EQ(vibe_copysign(-2, 1), 2); EQ(vibe_copysign(NAN, -1) < 0 || signbit(vibe_copysign(NAN, -1)), 1);
    EQ(vibe_tgamma(5), 24); EQ(vibe_tgamma(1), 1); EQ(vibe_tgamma(0), INFINITY); EQ(vibe_tgamma(-0.0), -INFINITY);
    EQ(vibe_tgamma(-2), NAN); EQ(vibe_tgamma(172), INFINITY);
    EQ(vibe_lgamma(1), 0); EQ(vibe_lgamma(2), 0); EQ(vibe_lgamma(0), INFINITY); EQ(vibe_lgamma(-3), INFINITY);
    EQ(vibe_expm1(0), 0); EQ(vibe_expm1(-INFINITY), -1); EQ(vibe_expm1(INFINITY), INFINITY);
    EQ(vibe_log1p(-1), -INFINITY); EQ(vibe_log1p(-2), NAN); EQ(vibe_log1p(-0.0), -0.0);
    EQ(vibe_acosh(0.5), NAN); EQ(vibe_atanh(1), INFINITY); EQ(vibe_atanh(-1), -INFINITY); EQ(vibe_atanh(2), NAN);
    EQ(vibe_erf(INFINITY), 1); EQ(vibe_erf(-INFINITY), -1); EQ(vibe_erfc(INFINITY), 0); EQ(vibe_erfc(-INFINITY), 2);
    EQ(vibe_asinh(-0.0), -0.0); EQ(vibe_erf(-0.0), -0.0); EQ(isnan(vibe_nan("")), 1);
    printf(bad ? "compat2: %d FAILED\n" : "compat2: all ok\n", bad);
    return bad != 0;
}
