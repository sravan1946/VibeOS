/* math_diff.c - compare the VibeOS libm (x87) with glibc: max error in ulps per function over random inputs.
 * Built as a 64-bit program with user/lib/math.c renamed to vibe_* (the x87 asm runs fine on x86-64). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#define F1(n) double vibe_##n(double);
#define F2(n) double vibe_##n(double, double);
F1(sin) F1(cos) F1(tan) F1(asin) F1(acos) F1(atan) F1(sqrt) F1(exp) F1(log) F1(log2) F1(log10)
F1(floor) F1(ceil) F1(round) F1(trunc) F1(sinh) F1(cosh) F1(tanh)
F2(atan2) F2(pow) F2(fmod) F2(hypot)
double vibe_ldexp(double, int);
double vibe_frexp(double, int *);
double vibe_modf(double, double *);

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
    printf("%-6s [%g, %g] max %7.2f ulp (at %.17g)%s\n", #name, (double)lo, (double)hi, mx, wx, mx > lim ? "  <-- FAIL" : ""); if (mx > lim) bad++; } while (0)
#define T2(name, lo, hi, lo2, hi2, lim) do { double mx = 0, wx = 0, wy = 0; \
    for (int i = 0; i < 20000; i++) { double x = rnd(lo, hi), y = rnd(lo2, hi2), u = ulps(vibe_##name(x, y), name(x, y)); \
        if (u > mx) { mx = u; wx = x; wy = y; } } \
    printf("%-6s max %7.2f ulp (at %.17g, %.17g)%s\n", #name, mx, wx, wy, mx > lim ? "  <-- FAIL" : ""); if (mx > lim) bad++; } while (0)

int main(void) {
    srand(7);
    T1(sin, -100, 100, 4); T1(cos, -100, 100, 4); T1(tan, -100, 100, 8);
    T1(sin, -1e6, 1e6, 8);
    T1(asin, -1, 1, 4); T1(acos, -1, 1, 4); T1(atan, -100, 100, 4);
    T1(sqrt, 0, 1e6, 0);
    T1(exp, -700, 700, 2); T1(exp, -1, 1, 2);
    T1(log, 1e-300, 1e300, 2); T1(log, 0.5, 2, 4); T1(log2, 1e-10, 1e10, 2); T1(log10, 1e-10, 1e10, 2);
    T1(floor, -1e6, 1e6, 0); T1(ceil, -1e6, 1e6, 0); T1(round, -1e6, 1e6, 0); T1(trunc, -1e6, 1e6, 0);
    T1(sinh, -20, 20, 4); T1(sinh, -1e-3, 1e-3, 4); T1(cosh, -20, 20, 4); T1(tanh, -5, 5, 4); T1(tanh, -1e-3, 1e-3, 4);
    T2(atan2, -100, 100, -100, 100, 4);
    T2(pow, 0.001, 1000, -20, 20, 4); T2(pow, 1, 10, 0, 300, 8);
    T2(fmod, -1e6, 1e6, 0.001, 1000, 0);
    T2(hypot, -1e6, 1e6, -1e6, 1e6, 2);
    for (int i = 0; i < 20000; i++) {                  /* frexp / ldexp / modf are exact */
        uint64_t u = ((uint64_t)rand() << 42) ^ ((uint64_t)rand() << 21) ^ rand();
        double d; memcpy(&d, &u, 8);
        if (isnan(d) || isinf(d)) continue;
        int e1, e2; double ip1, ip2;
        double f1 = frexp(d, &e1), f2 = vibe_frexp(d, &e2);
        if (f1 != f2 || e1 != e2) { if (bad++ < 10) printf("frexp(%.17g) glibc %.17g,%d vibe %.17g,%d\n", d, f1, e1, f2, e2); }
        if (vibe_modf(d, &ip2) != modf(d, &ip1) || ip1 != ip2) { if (bad++ < 10) printf("modf(%.17g) differs\n", d); }
        int sh = rand() % 200 - 100;
        if (vibe_ldexp(d, sh) != ldexp(d, sh)) { if (bad++ < 10) printf("ldexp(%.17g,%d) differs\n", d, sh); }
    }
    printf("math_diff: %d failures\n", bad);
    return bad != 0;
}
