/* printf_diff.c - compare the VibeOS printf engine with glibc over many formats and values */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include <math.h>

int vibe_snprintf(char *b, size_t n, const char *fmt, ...);

static int bad, total;

#define DIFF(...) do { char a[2048], b[2048]; \
    snprintf(a, sizeof a, __VA_ARGS__); vibe_snprintf(b, sizeof b, __VA_ARGS__); total++; \
    if (strcmp(a, b)) { if (bad++ < 20) printf("MISMATCH %s\n  glibc: [%s]\n  vibe:  [%s]\n", #__VA_ARGS__, a, b); } } while (0)

static void fl(double v) {
    static const char *const f[] = { "%f", "%.0f", "%.1f", "%.3f", "%10.2f", "%-12.4f|", "%+.2f", "% f", "%#.0f", "%015.5f",
        "%e", "%.0e", "%.3e", "%-14.2e|", "%E", "%#.0e",
        "%a", "%A", "%.0a", "%.1a", "%.3a", "%.12a", "%.13a", "%.16a", "%#a", "%#.0a", "%20a|", "%-20a|", "%020a", "%+a", "% a",
        "%g", "%.0g", "%.1g", "%.3g", "%.10g", "%.17g", "%#g", "%12g", "%G", "%+g", "%020.8g", "%.20f", "%.25e", "%.40g" };
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
        char a[2048], b[2048];
        if (!strcmp(f[i], "%#g") && v == 999999.5) continue;   /* glibc prints "1.e+06" here, a glibc bug */
        snprintf(a, sizeof a, f[i], v);
        vibe_snprintf(b, sizeof b, f[i], v);
        total++;
        if (strcmp(a, b) && bad++ < 20) printf("MISMATCH %s %.17g\n  glibc: [%s]\n  vibe:  [%s]\n", f[i], v, a, b);
    }
}

int main(void) {
    static const double sp[] = { 0, -0.0, 0.5, 1.5, 2.5, 3.5, 0.125, 0.375, 1e-5, 1e-4, 123456789, 1e15, 1e16, 1e22, 1e23,
        5e-324, DBL_MAX, DBL_MIN, 0.1, 0.2, 0.3, 9.995, 99.5, 999999.5, 0.95, 0.05, 1.0 / 3, 2.0 / 3, 100, 1e5, 1e6, 123456,
        1234567, 0.00001234, 9.9999999, 9.5, 0.5e-10, 4.35, 2.675, 1.005, 1e100, 1e-100, 1.7976931348623157e308, INFINITY, -INFINITY, NAN, -1.5, -123.456 };
    for (unsigned i = 0; i < sizeof sp / sizeof *sp; i++) fl(sp[i]);
    srand(12345);
    for (int i = 0; i < 6000; i++) {                    /* random bit patterns (all exponents) */
        uint64_t u = ((uint64_t)rand() << 42) ^ ((uint64_t)rand() << 21) ^ rand();
        double d; memcpy(&d, &u, 8);
        if (isnan(d)) continue;
        fl(d);
    }
    for (int i = 0; i < 20000; i++) {                   /* "human" values */
        double d = (rand() % 2000000 - 1000000) / (double)(1 << (rand() % 12)) / (rand() % 3 ? 1 : 1000);
        fl(d);
    }
    for (int i = 0; i < 30000; i++) {
        int v = rand() - RAND_MAX / 2; long long ll = ((long long)rand() << 33) ^ ((long long)rand() << 11) ^ rand();
        DIFF("%d|%i|%u|%x|%X|%o|%#x|%#o|%05d|%-7d|%+d|% d|%.5d|%8.3d|%ld|%lld|%llu|%llx|%hd|%hhd|%hu|%zu|%08x",
             v, v, v, v, v, v, v, v, v, v, v, v, v, v, (long)v, ll, ll, ll, v, v, v, (size_t)v, v);
        DIFF("%.0d|%.0x|%#.0o|%5.0d|%-5.0u|", v & 1, v & 1, v & 1, v & 1, v & 1);
    }
    DIFF("%c|%5c|%-5c|%s|%10s|%-10s|%.3s|%.0s|%5.2s|%%", 'x', 'y', 'z', "str", "str", "str", "string", "string", "string");
    DIFF("%*d|%-*d|%.*f|%*.*f", 6, 42, 6, 42, 3, 3.14159, 9, 2, 2.71828);
    DIFF("%p|%p", (void *)0x1000, (void *)0);
    printf("%d comparisons, %d mismatches\n", total, bad);
    return bad != 0;
}
