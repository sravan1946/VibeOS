/* math.c - libm on top of the x87 (long double intermediates, so results are good to ~1 ulp of double).
 * Needs the FPU exceptions masked (the fninit default) so overflow / domain errors give inf / nan. */
#include <math.h>
#include <stdint.h>

static long double f_exp2(long double x) {
    long double i, f;
    if (x > 16384) return __builtin_infl();
    if (x < -16384) return 0;
    __asm__("frndint" : "=t"(i) : "0"(x));
    f = x - i;
    __asm__("f2xm1" : "=t"(f) : "0"(f));
    f += 1;
    __asm__("fscale; fstp %%st(1)" : "=t"(f) : "0"(f), "u"(i) : "st(1)");
    return f;
}

static long double f_log2(long double x) {                 /* log2 via y*log2(x), y=1 */
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(1.0L) : "st(1)");
    return r;
}

static long double f_yl2x(long double x, long double y) {
    long double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double fabs(double x) { return __builtin_fabs(x); }
float fabsf(float x) { return __builtin_fabsf(x); }

/* fsqrt at 53-bit precision is a correctly rounded double sqrt (at 64 bits it would round twice) */
double sqrt(double x) {
    unsigned short old, nw;
    double r;
    __asm__ volatile("fnstcw %0" : "=m"(old));
    nw = (old & ~0x300) | 0x200;
    __asm__ volatile("fldcw %0" : : "m"(nw) : "memory");
    __asm__ volatile("fsqrt" : "=t"(r) : "0"(x));
    __asm__ volatile("fldcw %0" : : "m"(old) : "memory");
    return r;
}
float sqrtf(float x) { return (float)sqrt(x); }

#define BIG 9223372036854775808.0            /* 2^63: fsin/fcos give up beyond this */

/* trig argument reduction: pi/2 split into 32+32+64 bits so n*P1 and n*P2 are exact for |n| < 2^31 */
#define PIO2_1 0x1.921fb544p+0L
#define PIO2_2 0x1.0b4611a6p-34L
#define PIO2_3 2.0222662487959507323765190428442772737e-21L
#define TWO_OVER_PI 0.63661977236758134307553505349005744813783858296182L

static long double f_rint(long double x) {
    long double r;
    __asm__("frndint" : "=t"(r) : "0"(x));
    return r;
}

/* x = n*pi/2 + r with |r| <= pi/4, *q = n mod 4; 0 if |x| is too big (callers fall back to raw fsin) */
static int reduce(double x, long double *r, int *q) {
    if (!(fabs(x) < 1e9)) return 0;
    long double n = f_rint(x * TWO_OVER_PI);
    *q = (int)(long long)n & 3;
    *r = (((long double)x - n * PIO2_1) - n * PIO2_2) - n * PIO2_3;
    return 1;
}

static long double f_sin(long double r) { __asm__("fsin" : "=t"(r) : "0"(r)); return r; }
static long double f_cos(long double r) { __asm__("fcos" : "=t"(r) : "0"(r)); return r; }

double sin(double x) {
    long double r;
    int q;
    if (x == 0) return x;
    if (reduce(x, &r, &q)) return q == 0 ? f_sin(r) : q == 1 ? f_cos(r) : q == 2 ? -f_sin(r) : -f_cos(r);
    if (!(fabs(x) < BIG)) return NAN;
    return f_sin(x);
}

double cos(double x) {
    long double r;
    int q;
    if (reduce(x, &r, &q)) return q == 0 ? f_cos(r) : q == 1 ? -f_sin(r) : q == 2 ? -f_cos(r) : f_sin(r);
    if (!(fabs(x) < BIG)) return NAN;
    return f_cos(x);
}

static long double f_tan(long double x) {
    long double r, one;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x));
    return r;
}

double tan(double x) {
    long double r;
    int q;
    if (x == 0) return x;
    if (reduce(x, &r, &q)) return q & 1 ? -1 / f_tan(r) : f_tan(r);
    if (!(fabs(x) < BIG)) return NAN;
    return f_tan(x);
}

static long double f_atan2(long double y, long double x) {
    long double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double atan2(double y, double x) { return f_atan2(y, x); }
double atan(double x) { return f_atan2(x, 1.0L); }
double asin(double x) { return f_atan2(x, sqrt((1.0 - x) * (1.0 + x))); }
double acos(double x) { return f_atan2(sqrt((1.0 - x) * (1.0 + x)), x); }

double exp(double x) { return f_exp2(x * 1.4426950408889634073599246810018921L); }
double log(double x) { return f_yl2x(x, 0.693147180559945309417232121458176568L); }
double log2(double x) { return f_log2(x); }
double log10(double x) { return f_yl2x(x, 0.301029995663981195213738894724493027L); }

double trunc(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;       /* |x| >= 2^52 is integral; also passes nan/inf */
    double r = (double)(long long)x;
    return r == 0 ? __builtin_copysign(0.0, x) : r;
}
double floor(double x) { double t = trunc(x); return t > x ? t - 1 : t; }
double ceil(double x) { double t = trunc(x); return t < x ? t + 1 : t; }
double round(double x) {
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += x < 0 ? -1 : 1;
    return t;
}

double fmod(double x, double y) {
    long double r = x;
    if (y == 0 || __builtin_isinf(x) || __builtin_isnan(x) || __builtin_isnan(y)) return NAN;
    if (__builtin_isinf(y)) return x;
    __asm__("1: fprem; fstsw %%ax; sahf; jp 1b" : "+t"(r) : "u"((long double)y) : "ax", "cc");
    return r;
}

double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 1) return 1;
    if (__builtin_isnan(x) || __builtin_isnan(y)) return NAN;
    int yint = y == trunc(y), yodd = yint && fabs(y) < 9007199254740992.0 && ((long long)y & 1);
    if (x == 0) {
        if (y < 0) return yodd ? __builtin_copysign(__builtin_inf(), x) : __builtin_inf();
        return yodd ? x : 0;
    }
    if (__builtin_isinf(y)) {
        if (fabs(x) == 1) return 1;
        return (fabs(x) > 1) == (y > 0) ? __builtin_inf() : 0;
    }
    if (yint && fabs(y) <= 64) {                         /* exact-ish repeated multiplication */
        long double r = 1, b = x;
        for (long long n = (long long)fabs(y); n; n >>= 1, b *= b)
            if (n & 1) r *= b;
        return y < 0 ? 1 / r : r;
    }
    if (x < 0) {
        if (!yint) return NAN;
        long double r = f_exp2(y * f_log2(-(long double)x));
        return yodd ? -r : r;
    }
    return f_exp2(y * f_log2(x));
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }
/* exp(x) - 1 without cancellation: f2xm1 is exact-ish for |t| <= 1 */
static long double f_expm1(long double x) {
    long double t = x * 1.4426950408889634073599246810018921L;
    if (t > 1 || t < -1) return f_exp2(t) - 1;
    __asm__("f2xm1" : "=t"(t) : "0"(t));
    return t;
}

double sinh(double x) {
    long double e = f_expm1(fabs(x)), r = (e + e / (e + 1)) / 2;
    return x < 0 ? -r : r;
}
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) {
    if (x > 20) return 1;
    if (x < -20) return -1;
    long double e = f_expm1(2 * fabs(x)), r = e / (e + 2);
    return x < 0 ? -r : r;
}

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float roundf(float x) { return (float)round(x); }

/* frexp/ldexp/modf: bit twiddling, exact */
double frexp(double x, int *e) {
    union { double d; uint64_t u; } v = { x };
    int ex = (v.u >> 52) & 0x7ff;
    *e = 0;
    if (x == 0 || ex == 0x7ff) return x;
    if (!ex) {                                           /* subnormal: scale up first */
        x *= 18446744073709551616.0;
        v.d = x;
        ex = (v.u >> 52) & 0x7ff;
        *e = -64;
    }
    *e += ex - 1022;
    v.u = (v.u & ~(0x7ffull << 52)) | (1022ull << 52);
    return v.d;
}

long double __ldexpl(long double x, int e) {
    long double r, ee = e > 20000 ? 20000 : e < -20000 ? -20000 : e;
    __asm__("fscale; fstp %%st(1)" : "=t"(r) : "0"(x), "u"(ee) : "st(1)");
    return r;
}
double ldexp(double x, int e) { return (double)__ldexpl(x, e); }

double modf(double x, double *ip) {
    double t = trunc(x);
    *ip = t;
    if (__builtin_isinf(x)) return __builtin_copysign(0.0, x);
    return __builtin_copysign(x - t, x);
}
