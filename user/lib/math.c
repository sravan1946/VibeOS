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
    __asm__("fscale" : "=t"(f) : "0"(f), "u"(i) : "st(1)");
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

double sqrt(double x) {
    double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
float sqrtf(float x) { return (float)sqrt(x); }

#define BIG 9223372036854775808.0            /* 2^63: fsin/fcos give up beyond this */

double sin(double x) {
    long double r;
    if (!(fabs(x) < BIG)) return NAN;
    __asm__("fsin" : "=t"(r) : "0"((long double)x));
    return r;
}

double cos(double x) {
    long double r;
    if (!(fabs(x) < BIG)) return NAN;
    __asm__("fcos" : "=t"(r) : "0"((long double)x));
    return r;
}

double tan(double x) {
    long double r, one;
    if (!(fabs(x) < BIG)) return NAN;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"((long double)x));
    return r;
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
    return (double)(long long)x;
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
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) {
    if (x > 20) return 1;
    if (x < -20) return -1;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float roundf(float x) { return (float)round(x); }
