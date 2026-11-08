/* strtod_diff.c - compare the VibeOS strtod with glibc: every input must match bit for bit (the bignum path is
 * correctly rounded: 17-digit round trips, halfway cases, subnormals, very long digit strings, huge exponents). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

double vibe_strtod(const char *s, char **end);
int vibe_errno;
long double __ldexpl(long double x, int e) { return ldexpl(x, e); }

static int bad, total, slow_off, slow_total, off_by_more;

static int same(double a, double b) { return a == b ? signbit(a) == signbit(b) : (isnan(a) && isnan(b)); }

static void chk(const char *s, int must_be_exact) {
    char *e1, *e2;
    double a = strtod(s, &e1), b = vibe_strtod(s, &e2);
    total++;
    if (e1 != e2 || (must_be_exact && !same(a, b))) {
        if (bad++ < 20) printf("MISMATCH \"%s\": glibc %.17g (end %zd), vibe %.17g (end %zd)\n", s, a, e1 - s, b, e2 - s);
    } else if (!must_be_exact) {
        slow_total++;
        if (!same(a, b)) { slow_off++; if (fabs(a - b) > 2 * fabs(nextafter(a, INFINITY) - a)) off_by_more++; }
    }
}

int main(void) {
    static const char *const fixed[] = { "0", "-0", "0.0", "1", "-1", "+1", ".5", "5.", "1e0", "1E5", "1e+5", "1e-5", "0.1", "0.2", "0.3",
        "3.14159265358979", "2.718281828459045", "1.7976931348623157e308", "2.2250738585072014e-308", "4.9e-324", "5e-324", "1e-400", "1e400",
        "123456789012345678901234567890", "9007199254740993", "9007199254740992", "0.000001", "100", "1e22", "1e23", "8.5e-3",
        "0x1p0", "0x1p4", "0X.8", "0xA.8p1", "0x1.fffffffffffffp1023", "0x1p-1074", "0x1p-1080", "0x1p1024", "0x10", "0xff.f", "0x1.8p+1",
        "0x", "0xg", "0x.p1", "0x1p", "0x1p+", "1e", "1e+", "e5", ".", "-", "+.e1", "inf", "-Infinity", "nan", "  12.5abc", "\t-7e2x", "1.5.5", "--1", "0.1e1e1" };
    for (unsigned i = 0; i < sizeof fixed / sizeof *fixed; i++) chk(fixed[i], 1);
    srand(99);
    char b[128];
    for (int i = 0; i < 200000; i++) {                 /* exact path: <= 15 digits, |exponent| <= 22 */
        int nd = 1 + rand() % 15, o = 0, dot = rand() % (nd + 1);
        for (int k = 0; k < nd; k++) { if (k == dot) b[o++] = '.'; b[o++] = '0' + rand() % 10; }
        snprintf(b + o, 20, "e%d", rand() % 31 - 15);
        chk(b, 1);
    }
    for (int i = 0; i < 100000; i++) {                 /* round trips of random doubles: 17 digits, long path */
        uint64_t u = ((uint64_t)rand() << 42) ^ ((uint64_t)rand() << 21) ^ rand();
        double d; memcpy(&d, &u, 8);
        if (isnan(d) || isinf(d)) continue;
        snprintf(b, sizeof b, "%.17g", d);
        chk(b, 1);
    }
    for (int i = 0; i < 50000; i++) {                  /* hex floats from %a */
        uint64_t u = ((uint64_t)rand() << 42) ^ ((uint64_t)rand() << 21) ^ rand();
        double d; memcpy(&d, &u, 8);
        if (isnan(d) || isinf(d)) continue;
        snprintf(b, sizeof b, "%a", d);
        chk(b, 1);
    }
    for (int i = 0; i < 100000; i++) {                 /* random digit strings of 16..60 digits at random exponents */
        int nd = 16 + rand() % 45, o = 0;
        b[o++] = '1' + rand() % 9;
        for (int k = 1; k < nd; k++) b[o++] = '0' + rand() % 10;
        snprintf(b + o, 20, "e%d", rand() % 700 - 380);
        chk(b, 1);
    }
    for (int i = 0; i < 50000; i++) {                  /* halfway between two doubles (exact decimal expansion of the midpoint), then +/- a hair */
        uint64_t u = ((uint64_t)rand() << 42) ^ ((uint64_t)rand() << 21) ^ rand();
        u &= 0x7fefffffffffffffull;
        if (i % 3 == 0) u &= 0x000fffffffffffffull;     /* subnormals */
        double d, e; memcpy(&d, &u, 8); u++; memcpy(&e, &u, 8);
        long double mid = ((long double)d + (long double)e) / 2;   /* exact in x87 (64-bit mantissa) */
        static char big[1200];
        snprintf(big, sizeof big, "%.*Le", 25, mid);       /* only a rounded view: the neighbours below still test the rounding step */
        chk(big, 1);
        snprintf(big, sizeof big, "%.*Le", 780, mid);
        chk(big, 1);
    }
    {                                                  /* very long inputs: 800+ digits, and the classic hard cases */
        static char big[2000];
        int o = 0;
        for (int k = 0; k < 1500; k++) big[o++] = k ? '0' + rand() % 10 : '4';
        strcpy(big + o, "e-1500"); chk(big, 1);
        strcpy(big + o, "e-1450"); chk(big, 1);
        memset(big, '0', 700); big[700] = '.'; for (int k = 0; k < 700; k++) big[701 + k] = '0' + rand() % 10; big[1401] = 0; chk(big, 1);
        const char *hard[] = { "2.2250738585072011e-308", "2.2250738585072012e-308", "2.2250738585072013e-308", "2.2250738585072014e-308",
            "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "2.4703282292062329e-324",
            "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1.797693134862315807e308",
            "9007199254740993.0000000000000000001", "9007199254740993.0", "9007199254740995", "1.00000000000000011102230246251565404236316680908203125",
            "1.00000000000000011102230246251565404236316680908203126", "1.00000000000000011102230246251565404236316680908203124",
            "0.00000000000000000000000000000000000000000000001", "123456789012345678901234567890e-30", "7.038531e-26", "5.0e-324", "3e-324", "2e-324",
            "8.41e21", "8.589973e9", "1e23", "9.999999999999999e22", "1.0000000000000001e23" };
        for (unsigned i = 0; i < sizeof hard / sizeof *hard; i++) chk(hard[i], 1);
    }
    printf("strtod_diff: %d inputs, %d hard mismatches; long path: %d/%d not bit-exact (%d by more than 2 ulp)\n",
           total, bad, slow_off, slow_total, off_by_more);
    return bad != 0 || off_by_more != 0;
}
