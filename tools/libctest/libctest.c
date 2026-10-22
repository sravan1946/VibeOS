/* libctest.c - runs the portable parts of VibeOS's libc as a freestanding 32-bit Linux program */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <regex.h>

static int fails, checks;

static void fail(const char *file, int line, const char *what, const char *got, const char *want) {
    char b[400];
    snprintf(b, sizeof b, "FAIL %s:%d %s\n  got:  [%s]\n  want: [%s]\n", file, line, what, got, want);
    write(2, b, strlen(b));
    fails++;
}
#define CHECK(c) do { checks++; if (!(c)) fail(__FILE__, __LINE__, #c, "", ""); } while (0)
#define CHECK_STR(got, want) do { checks++; if (strcmp((got), (want))) fail(__FILE__, __LINE__, #got, (got), (want)); } while (0)
#define FMT(want, ...) do { char _b[1024]; snprintf(_b, sizeof _b, __VA_ARGS__); \
    checks++; if (strcmp(_b, (want))) fail(__FILE__, __LINE__, #__VA_ARGS__, _b, (want)); } while (0)
#define NEAR(a, b) do { double _a = (a), _b = (b); checks++; \
    if (fabs(_a - _b) > 1e-12 * (fabs(_b) > 1 ? fabs(_b) : 1)) { char _s[2][64]; \
    snprintf(_s[0], 64, "%.17g", _a); snprintf(_s[1], 64, "%.17g", _b); fail(__FILE__, __LINE__, #a, _s[0], _s[1]); } } while (0)

static void test_string(void) {
    char b[64];
    CHECK(strlen("hello") == 5);
    strcpy(b, "abc"); strcat(b, "def"); CHECK_STR(b, "abcdef");
    strncpy(b, "xy", 5); CHECK(b[2] == 0 && b[4] == 0);
    strcpy(b, "ab"); strncat(b, "cdef", 2); CHECK_STR(b, "abcd");
    CHECK(strcmp("a", "b") < 0 && strcmp("b", "a") > 0 && !strcmp("x", "x"));
    CHECK(strncmp("abcd", "abzz", 2) == 0 && strncmp("abcd", "abzz", 3) < 0);
    CHECK(strcasecmp("HeLLo", "hello") == 0 && strncasecmp("HEx", "hey", 2) == 0 && strncasecmp("HEx", "hey", 3) < 0);
    CHECK_STR(strchr("hello", 'l'), "llo");
    CHECK_STR(strrchr("hello", 'l'), "lo");
    CHECK(strchr("hello", 'z') == NULL && strchr("hello", 0) != NULL);
    CHECK_STR(strstr("find the needle here", "needle"), "needle here");
    CHECK(strstr("abc", "abd") == NULL && strstr("abc", "") != NULL);
    CHECK(strspn("aabbcc", "ab") == 4 && strcspn("hello world", " ") == 5);
    CHECK_STR(strpbrk("hello", "lo"), "llo");
    char t[] = "  a,b;;c  ", *save;
    CHECK_STR(strtok_r(t, " ,;", &save), "a");
    CHECK_STR(strtok_r(NULL, " ,;", &save), "b");
    CHECK_STR(strtok_r(NULL, " ,;", &save), "c");
    CHECK(strtok_r(NULL, " ,;", &save) == NULL);
    char t2[] = "x y"; CHECK_STR(strtok(t2, " "), "x"); CHECK_STR(strtok(NULL, " "), "y"); CHECK(!strtok(NULL, " "));
    char *d = strdup("dup"); CHECK_STR(d, "dup"); free(d);
    d = strndup("truncate", 5); CHECK_STR(d, "trunc"); free(d);
    CHECK(memchr("abc", 'c', 3) != NULL && memchr("abc", 'c', 2) == NULL);
    char m[11] = "0123456789"; memmove(m + 2, m, 6); CHECK(!memcmp(m, "0101234589", 10));
    memmove(m, m + 2, 6); CHECK(!memcmp(m, "0123454589", 10));
    memset(m, 'z', 3); CHECK(!memcmp(m, "zzz3454589", 10));
    CHECK(memcmp("abc", "abd", 3) < 0);
    CHECK(strlcpy(b, "toolongstring", 5) == 13 && !strcmp(b, "tool"));
    CHECK_STR(strerror(ENOENT), "No such file or directory");
    CHECK(isalpha('a') && !isalpha('1') && isdigit('7') && isspace('\t') && isupper('Q') && !islower('Q'));
    CHECK(isxdigit('f') && !isxdigit('g') && ispunct('!') && isprint(' ') && !isprint('\n') && isalnum('z'));
    CHECK(toupper('a') == 'A' && tolower('Z') == 'z' && toupper('1') == '1');
}

static void test_printf(void) {
    FMT("42", "%d", 42);
    FMT("-42", "%i", -42);
    FMT("   42|42   |00042", "%5d|%-5d|%05d", 42, 42, 42);
    FMT("+5 -5  5", "%+d %+d % d", 5, -5, 5);
    FMT("ff FF 0xff 0XFF 377 0377", "%x %X %#x %#X %o %#o", 255, 255, 255, 255, 255, 255);
    FMT("4294967295", "%u", 0xffffffffu);
    FMT("18446744073709551615 -9223372036854775808", "%llu %lld", 18446744073709551615ull, (long long)(-9223372036854775807ll - 1));
    FMT("-1 65535 255", "%hhd %hu %hhu", 255, -1, -1);
    FMT("    7|0007|  0007", "%5.1d|%.4d|%6.4d", 7, 7, 7);
    FMT("|", "%.0d|", 0);
    FMT("   42", "%*d", 5, 42);
    FMT("42   |", "%-*d|", 5, 42);
    FMT("007", "%0*d", 3, 7);
    FMT("a|  b|c  |", "%c|%3c|%-3c|", 'a', 'b', 'c');
    FMT("hello|  hi|hel|(null)", "%s|%4s|%.3s|%s", "hello", "hi", "hello", (char *)0);
    FMT("   abc|abc   |", "%6s|%-6s|", "abc", "abc");
    FMT("100%", "%d%%", 100);
    FMT("0x1234abcd", "%p", (void *)0x1234abcd);
    FMT("(nil)", "%p", (void *)0);
    FMT("10 5", "%zu %zd", (size_t)10, (ssize_t)5);
    FMT("a=1 b=-2", "a=%d b=%d", 1, -2);
    /* floating point */
    FMT("3.141593", "%f", 3.14159265358979);
    FMT("3.14", "%.2f", 3.14159);
    FMT("3", "%.0f", 3.14159);
    FMT("4", "%.0f", 3.5);
    FMT("2", "%.0f", 2.5);                              /* exact tie: half-even like glibc */
    FMT("3.", "%#.0f", 3.0);
    FMT("0003.142", "%08.3f", 3.14159);
    FMT("-003.142", "%08.3f", -3.14159);
    FMT("+1.50", "%+.2f", 1.5);
    FMT(" 1.50", "% .2f", 1.5);
    FMT("1.500000e+00", "%e", 1.5);
    FMT("1.23457e+08", "%g", 123456789.0);
    FMT("0.0001", "%g", 0.0001);
    FMT("1e-05", "%g", 0.00001);
    FMT("100000", "%g", 100000.0);
    FMT("1e+06", "%g", 1000000.0);
    FMT("0.1", "%g", 0.1);
    FMT("1.5", "%g", 1.5);
    FMT("1.50000", "%#g", 1.5);
    FMT("12346", "%.5G", 12345.678);
    FMT("1.235E+04", "%.4G", 12345.678);
    FMT("0.100000000000000006", "%.18f", 0.1);
    FMT("0.1000000000000000055511151231257827", "%.34f", 0.1);
    FMT("1e+22", "%g", 1e22);
    FMT("10000000000000000000000.000000", "%f", 1e22);
    FMT("1e+23", "%g", 1e23);
    FMT("99999999999999991611392.000000", "%f", 1e23);
    FMT("4.94066e-324", "%g", 4.9406564584124654e-324);
    FMT("1.79769e+308", "%g", 1.7976931348623157e308);
    FMT("0.9", "%.1f", 0.95);                           /* 0.95 is really 0.94999999999999996 */
    FMT("inf -inf nan", "%f %f %f", (double)INFINITY, -(double)INFINITY, (double)NAN);
    FMT("  inf|INF", "%5f|%F", (double)INFINITY, (double)INFINITY);
    FMT("-0.00", "%.2f", -0.0);
    FMT("0.00", "%.2f", 0.001);
    FMT("1.00", "%.2f", 0.999);
    FMT("10.0", "%.1f", 9.96);
    FMT("1.0e+01", "%.1e", 9.96);
    FMT("   2.50", "%7.2f", 2.5);
    FMT("2.50   |", "%-7.2f|", 2.5);
    FMT("6.02e+23", "%.3g", 6.02214076e23);
    FMT("123.456", "%g", 123.456);
    /* snprintf semantics */
    char b[8];
    CHECK(snprintf(b, sizeof b, "%s", "0123456789") == 10 && !strcmp(b, "0123456"));
    CHECK(snprintf(NULL, 0, "%d", 12345) == 5);
    char *as;
    CHECK(asprintf(&as, "%s-%d", "x", 7) == 3 && !strcmp(as, "x-7"));
    free(as);
}

static void test_conv(void) {
    CHECK(atoi("  -123abc") == -123);
    CHECK(strtol("0x1F", NULL, 16) == 31 && strtol("0x1F", NULL, 0) == 31 && strtol("077", NULL, 0) == 63);
    CHECK(strtol("zz", NULL, 36) == 35 * 36 + 35);
    char *e;
    CHECK(strtol("123xyz", &e, 10) == 123 && !strcmp(e, "xyz"));
    CHECK(strtol("xyz", &e, 10) == 0 && !strcmp(e, "xyz"));
    errno = 0; CHECK(strtol("99999999999", NULL, 10) == LONG_MAX && errno == ERANGE);
    errno = 0; CHECK(strtol("-99999999999", NULL, 10) == LONG_MIN && errno == ERANGE);
    CHECK(strtoul("4294967295", NULL, 10) == 4294967295ul);
    CHECK(strtoll("-9223372036854775808", NULL, 10) == (-9223372036854775807ll - 1));
    CHECK(strtoull("18446744073709551615", NULL, 10) == 18446744073709551615ull);
    NEAR(strtod("3.14159", NULL), 3.14159);
    NEAR(strtod("-2.5e3", NULL), -2500.0);
    NEAR(strtod("1e-5", NULL), 1e-5);
    NEAR(strtod(".5", NULL), 0.5);
    { char *e; NEAR((double)strtold("2.5e2xyz", &e), 250.0); CHECK(*e == 'x'); }
    NEAR(strtod("1.7976931348623157e308", NULL), 1.7976931348623157e308);
    NEAR(strtod("123456789012345678901234567890", NULL), 1.2345678901234568e29);
    CHECK(strtod("0.1", NULL) == 0.1 && strtod("0.3", NULL) == 0.3 && strtod("2.2250738585072014e-308", NULL) == 2.2250738585072014e-308);
    CHECK(strtod("12.5abc", &e) == 12.5 && !strcmp(e, "abc"));
    CHECK(isinf(strtod("-inf", NULL)) && isnan(strtod("nan", NULL)));
    CHECK(atof("2.5") == 2.5);
    CHECK(abs(-3) == 3 && labs(-3L) == 3);
    div_t dv = div(17, 5); CHECK(dv.quot == 3 && dv.rem == 2);
}

static void test_new(void) {
    /* hex floats, %a */
    CHECK(strtod("0x1p4", NULL) == 16 && strtod("0x.8", NULL) == 0.5 && strtod("0xA.8p1", NULL) == 21);
    CHECK(strtod("0x1.fffffffffffffp1023", NULL) == 1.7976931348623157e308 && strtod("0x1p-1074", NULL) == 4.9406564584124654e-324);
    char *e;
    CHECK(strtod("0xg", &e) == 0 && !strcmp(e, "xg") && strtod("0x1p+", &e) == 1 && !strcmp(e, "p+"));
    CHECK(strtod("0.1", NULL) == 0.1 && strtod("123456789.123456", NULL) == 123456789.123456 && strtod("1e22", NULL) == 1e22);
    CHECK(strtod("9007199254740993", NULL) == 9007199254740992.0 && strtod("1.5e-7", NULL) == 1.5e-7);
    FMT("0x1p+0 0x1.8p+0 -0x1.8p+1 0x0p+0 0x1.999999999999ap-4", "%a %a %a %a %a", 1.0, 1.5, -3.0, 0.0, 0.1);
    FMT("0X1.FFP+7 0x1.00p+0 0x2p+0 0x0.0000000000001p-1022", "%A %.2a %.0a %a", 255.5, 1.0, 1.5, 5e-324);
    FMT("0x001.8p+0|  0x1p+0|0x1p+0  |", "%#010.1a|%8a|%-8a|", 1.5, 1.0, 1.0);
    /* libm odds and ends */
    int ex;
    CHECK(frexp(8, &ex) == 0.5 && ex == 4 && frexp(0, &ex) == 0 && ex == 0 && frexp(-0.75, &ex) == -0.75 && ex == 0);
    CHECK(frexp(5e-324, &ex) == 0.5 && ex == -1073 && ldexp(1, 10) == 1024 && ldexp(0.5, -1073) == 5e-324 && ldexp(1, 5000) > 1e308);
    double ip;
    CHECK(modf(3.25, &ip) == 0.25 && ip == 3 && modf(-3.25, &ip) == -0.25 && ip == -3);
    CHECK(signbit(ceil(-0.5)) && signbit(round(-0.4)) && !signbit(floor(0.5)) && signbit(sin(-0.0)));
    CHECK(sqrt(2) == 1.4142135623730951 && sqrt(1e300) == 1e150);
    /* strftime ISO week numbers and E/O modifiers */
    char b[64];
    struct tm tm;
    time_t t = 1609632000;                   /* Sun 2021-01-03: ISO week 53 of 2020 */
    gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%U %W %V %G %g %Ey %Od", &tm);
    CHECK_STR(b, "01 00 53 2020 20 21 03");
    t = 1735516800;                          /* Mon 2024-12-30: ISO week 1 of 2025 */
    gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%U %W %V %G %g", &tm);
    CHECK_STR(b, "52 53 01 2025 25");
    t = 1700000000; gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%U %W %V %G %g", &tm);
    CHECK_STR(b, "46 46 46 2023 23");
    /* misc libc additions */
    CHECK(strcoll("a", "b") < 0 && !strcoll("x", "x") && !strcmp(setlocale(LC_ALL, ""), "C") && setlocale(LC_ALL, "de_DE") == NULL);
    CHECK_STR(localeconv()->decimal_point, ".");
    FILE *f = tmpfile();
    CHECK(f != NULL);
    fputs("tmp data", f); rewind(f);
    CHECK(fgets(b, sizeof b, f) && !strcmp(b, "tmp data"));
    fclose(f);
    char n1[L_tmpnam], n2[L_tmpnam];
    CHECK(tmpnam(n1) && tmpnam(n2) && strcmp(n1, n2) && !strncmp(n1, "/tmp/", 5));
    f = fopen("libctest.re1", "w"); fputs("one\n", f); fclose(f);
    f = fopen("libctest.re2", "w"); fputs("two\n", f); fclose(f);
    f = fopen("libctest.re1", "r");
    CHECK(fgetc(f) == 'o');
    CHECK(freopen("libctest.re2", "r", f) == f && fgets(b, sizeof b, f) && !strcmp(b, "two\n"));
    CHECK(freopen("/nonexistent/zz", "r", f) == NULL);
    remove("libctest.re1"); remove("libctest.re2");
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_sort_rand(void) {
    int v[200];
    srand(7);
    for (int i = 0; i < 200; i++) v[i] = rand() % 1000;
    qsort(v, 200, sizeof v[0], cmp_int);
    int ok = 1;
    for (int i = 1; i < 200; i++) ok &= v[i - 1] <= v[i];
    CHECK(ok);
    int key = v[77], *f = bsearch(&key, v, 200, sizeof v[0], cmp_int);
    CHECK(f && *f == key);
    key = 5000; CHECK(!bsearch(&key, v, 200, sizeof v[0], cmp_int));
    struct { char name[12]; int x; } s[3] = { { "c", 3 }, { "a", 1 }, { "b", 2 } };
    qsort(s, 3, sizeof s[0], (int (*)(const void *, const void *))strcmp);   /* name is the first member */
    CHECK(!strcmp(s[0].name, "a") && !strcmp(s[1].name, "b") && s[2].x == 3);
    CHECK(rand() >= 0);
}

static void test_malloc(void) {
    void *p[300];
    unsigned char *q;
    srand(1);
    memset(p, 0, sizeof p);
    for (int round = 0; round < 3000; round++) {
        int i = rand() % 300;
        if (p[i]) {
            q = p[i];
            size_t n = (q[0] | q[1] << 8) % 2000 + 2;
            int ok = 1;
            for (size_t k = 2; k < n; k++) ok &= q[k] == (unsigned char)(i + k);
            CHECK(ok);
            free(p[i]);
            p[i] = NULL;
        } else {
            size_t n = rand() % 2000 + 2;
            q = malloc(n);
            CHECK(q != NULL && ((unsigned)q & 7) == 0);
            q[0] = (n - 2) & 0xff; q[1] = (n - 2) >> 8;
            for (size_t k = 2; k < n; k++) q[k] = (unsigned char)(i + k);
            p[i] = q;
        }
    }
    for (int i = 0; i < 300; i++) free(p[i]);
    /* everything freed and coalesced: one big block must satisfy a large request w/o growing the heap */
    void *a = sbrk(0);
    void *big = malloc(200000);
    CHECK(big != NULL);
    free(big);
    void *b = malloc(100); void *c = malloc(100);
    CHECK(b != c);
    b = realloc(b, 5000); CHECK(b != NULL);
    free(b); free(c);
    int *z = calloc(100, sizeof(int)); int zero = 1;
    for (int i = 0; i < 100; i++) zero &= z[i] == 0;
    CHECK(zero); free(z);
    CHECK(calloc(0x7fffffff, 4) == NULL);
    (void)a;
    char *s = malloc(4); strcpy(s, "abc");
    s = realloc(s, 10000); CHECK_STR(s, "abc"); free(s);
}

/* randomised malloc/realloc/free with content checks; then everything must coalesce back into one block */
static void test_malloc_stress(void) {
    enum { N = 2000 };
    static unsigned char *p[N];
    static size_t len[N];
    srand(7);
    int bad = 0;
    for (int op = 0; op < 60000; op++) {
        int i = rand() % N;
        size_t n = rand() % 7 == 0 ? rand() % 6000 : rand() % 120;
        if (p[i]) {
            for (size_t k = 0; k < len[i]; k++) bad += p[i][k] != (unsigned char)(i * 31 + k);
            if (rand() % 3 == 0) { free(p[i]); p[i] = NULL; continue; }
            unsigned char *q = realloc(p[i], n ? n : 1);
            size_t keep = n < len[i] ? n : len[i];
            for (size_t k = 0; k < keep; k++) bad += q[k] != (unsigned char)(i * 31 + k);
            p[i] = q; len[i] = n ? n : 1;
            for (size_t k = keep; k < len[i]; k++) q[k] = (unsigned char)(i * 31 + k);
        } else {
            p[i] = malloc(n ? n : 1); len[i] = n ? n : 1;
            bad += !p[i] || ((unsigned)p[i] & 7);
            for (size_t k = 0; k < len[i]; k++) p[i][k] = (unsigned char)(i * 31 + k);
        }
    }
    CHECK(bad == 0);
    for (int i = 0; i < N; i++) free(p[i]);
    void *top = sbrk(0);
    void *big = malloc(100000);                  /* needs one contiguous block: only possible if all merged */
    CHECK(big && sbrk(0) == top);
    free(big);
    char *m = malloc(1 << 20), *m2;              /* >= 128 KiB: its own mapping, outside the heap */
    CHECK(m && (m >= (char *)top || m < (char *)top - (8 << 20)) && sbrk(0) == top);
    memset(m, 0x5A, 1 << 20);
    m2 = realloc(m, 3 << 20);
    CHECK(m2 && m2[0] == 0x5A && m2[(1 << 20) - 1] == 0x5A);
    m = realloc(m2, 1000);                       /* back to the heap */
    CHECK(m && m[999] == 0x5A);
    free(m);
    int *zz = calloc(1 << 18, 4);
    CHECK(zz && zz[0] == 0 && zz[(1 << 18) - 1] == 0);
    free(zz);
    for (int i = 0; i < 100000; i++) p[i % N] = malloc(24 + i % 40);   /* many small blocks, freed in a scattered order */
    for (int i = 0; i < N; i++) free(p[(i * 757) % N]);
}

static void test_time(void) {
    time_t t = 1700000000;                   /* Tue 2023-11-14 22:13:20 UTC */
    struct tm tm;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 123 && tm.tm_mon == 10 && tm.tm_mday == 14 && tm.tm_hour == 22 && tm.tm_min == 13 && tm.tm_sec == 20);
    CHECK(tm.tm_wday == 2 && tm.tm_yday == 317);
    char b[128];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %a %A %b %B %j %y %e %p %I", &tm);
    CHECK_STR(b, "2023-11-14 22:13:20 Tue Tuesday Nov November 318 23 14 PM 10");
    strftime(b, sizeof b, "%F %T %D %R %c|%%|%u %w %Z %z", &tm);
    CHECK_STR(b, "2023-11-14 22:13:20 11/14/23 22:13 Tue Nov 14 22:13:20 2023|%|2 2 UTC +0000");
    CHECK(mktime(&tm) == t);                 /* TZ_OFFSET unset: local == UTC */
    time_t z = 0; gmtime_r(&z, &tm); CHECK(tm.tm_year == 70 && tm.tm_wday == 4 && tm.tm_yday == 0);
    time_t neg = -1; gmtime_r(&neg, &tm); CHECK(tm.tm_year == 69 && tm.tm_mon == 11 && tm.tm_mday == 31 && tm.tm_sec == 59);
    time_t leap = 951782400;                 /* 2000-02-29 */
    gmtime_r(&leap, &tm); CHECK(tm.tm_mon == 1 && tm.tm_mday == 29 && tm.tm_yday == 59);
    setenv("TZ_OFFSET", "330", 1);           /* IST */
    localtime_r(&t, &tm);
    CHECK(tm.tm_mday == 15 && tm.tm_hour == 3 && tm.tm_min == 43 && tm.tm_wday == 3);
    strftime(b, sizeof b, "%H:%M %z %Z", &tm);
    CHECK_STR(b, "03:43 +0530 UTC+05:30");
    CHECK(mktime(&tm) == t);
    setenv("TZ_OFFSET", "-300", 1);
    localtime_r(&t, &tm); CHECK(tm.tm_hour == 17 && tm.tm_mday == 14);
    strftime(b, sizeof b, "%z", &tm); CHECK_STR(b, "-0500");
    unsetenv("TZ_OFFSET");
    struct tm n = { .tm_year = 124, .tm_mon = 13, .tm_mday = 32, .tm_hour = 25 };   /* normalises */
    time_t nt = timegm(&n);
    CHECK(n.tm_year == 125 && n.tm_mon == 2 && n.tm_mday == 5 && n.tm_hour == 1);
    CHECK(nt == 1741136400);
    CHECK(time(NULL) == 1700000000 && clock() == 1234);
    CHECK(strftime(b, 4, "%Y-%m", &tm) == 0);   /* doesn't fit */
}

static void test_scanf(void) {
    int a, c; unsigned u; char s[32], s2[32], ch; double d; float f; long l; long long ll;
    CHECK(sscanf("42 -7", "%d %d", &a, &c) == 2 && a == 42 && c == -7);
    CHECK(sscanf("ff 0x10 077", "%x %i %o", &u, &a, &c) == 3 && u == 255 && a == 16 && c == 63);
    CHECK(sscanf("hello world", "%s %s", s, s2) == 2 && !strcmp(s, "hello") && !strcmp(s2, "world"));
    CHECK(sscanf("abc", "%c%c", &ch, &ch) == 2 && ch == 'b');
    CHECK(sscanf("3.5 -1e2 2.25", "%lf %f %lf", &d, &f, &d) == 3 && f == -100.0f && d == 2.25);
    CHECK(sscanf("key=val;rest", "%[a-z]=%[^;]", s, s2) == 2 && !strcmp(s, "key") && !strcmp(s2, "val"));
    CHECK(sscanf("12345", "%2d%3d", &a, &c) == 2 && a == 12 && c == 345);
    CHECK(sscanf("x", "%d", &a) == 0);
    CHECK(sscanf("", "%d", &a) == EOF);
    CHECK(sscanf("10 20", "%*d %d", &a) == 1 && a == 20);
    CHECK(sscanf("100%", "%d%%", &a) == 1 && a == 100);
    CHECK(sscanf("12 34", "%d %n", &a, &c) == 1 && c == 3);
    CHECK(sscanf("123456789012 99999999999", "%ld %lld", &l, &ll) == 2 && ll == 99999999999ll);
    CHECK(sscanf("  7,8", "%d,%d", &a, &c) == 2 && c == 8);
    CHECK(sscanf("v1.2", "v%d.%d", &a, &c) == 2 && a == 1 && c == 2);
    CHECK(sscanf("-12abc", "%d%s", &a, s) == 2 && a == -12 && !strcmp(s, "abc"));
    void *p; CHECK(sscanf("0xdeadbeef", "%p", &p) == 1 && (unsigned)p == 0xdeadbeef);
}

static void test_math(void) {
    NEAR(sqrt(2), 1.4142135623730951);
    NEAR(sin(1), 0.8414709848078965);
    NEAR(cos(1), 0.5403023058681398);
    NEAR(tan(1), 1.5574077246549023);
    NEAR(sin(M_PI / 6), 0.5);
    NEAR(atan(1), M_PI_4);
    NEAR(atan2(1, -1), 3 * M_PI_4);
    NEAR(atan2(-1, -1), -3 * M_PI_4);
    NEAR(asin(0.5), 0.5235987755982989);
    NEAR(acos(0.5), 1.0471975511965979);
    NEAR(exp(1), M_E);
    NEAR(exp(-2.5), 0.0820849986238988);
    NEAR(exp(10), 22026.465794806718);
    NEAR(log(M_E), 1);
    NEAR(log(10), 2.302585092994046);
    NEAR(log10(1000), 3);
    NEAR(log2(1024), 10);
    NEAR(pow(2, 10), 1024);
    NEAR(pow(2, 0.5), 1.4142135623730951);
    NEAR(pow(10, -3), 0.001);
    NEAR(pow(-2, 3), -8);
    NEAR(pow(9, 0.5), 3);
    NEAR(pow(1.0001, 10000.5), 2.7182818307237544);
    CHECK(pow(0, 0) == 1 && pow(0, 2) == 0 && isnan(pow(-2, 0.5)));
    CHECK(floor(2.7) == 2 && floor(-2.7) == -3 && ceil(2.2) == 3 && ceil(-2.2) == -2 && floor(5.0) == 5);
    CHECK(round(2.5) == 3 && round(-2.5) == -3 && round(2.4) == 2 && trunc(-2.9) == -2);
    CHECK(fabs(-3.5) == 3.5);
    NEAR(fmod(10, 3), 1);
    NEAR(fmod(-10, 3), -1);
    NEAR(fmod(5.5, 2), 1.5);
    NEAR(fmod(1e10, 7), 4);
    NEAR(sinh(1), 1.1752011936438014);
    NEAR(cosh(1), 1.5430806348152437);
    NEAR(tanh(0.5), 0.46211715726000974);
    NEAR(hypot(3, 4), 5);
    CHECK(isinf(exp(1000)));
    { char _x[64]; snprintf(_x, 64, "%g", exp(-1000)); CHECK_STR(_x, "0"); }
    CHECK(isinf(log(0)));
    CHECK(isnan(log(-1)));
    CHECK(isnan(sqrt(-1)));
    CHECK(isnan(sin(1e300)));
}

static jmp_buf jb;
static void jumper(int v) { longjmp(jb, v); }

static void test_setjmp(void) {
    volatile int n = 0;
    int r = setjmp(jb);
    n++;
    if (r == 0) jumper(5);
    CHECK(r == 5 && n == 2);
    r = setjmp(jb);
    if (!r) longjmp(jb, 0);
    CHECK(r == 1);                       /* longjmp(.., 0) yields 1 */
}

static void test_stdio(void) {
    const char *fn = "libctest.tmp";
    FILE *f = fopen(fn, "w");
    CHECK(f != NULL);
    fprintf(f, "line one\nline %d\n", 2);
    fputs("third", f); fputc('\n', f);
    CHECK(fwrite("abcdef", 1, 6, f) == 6);
    CHECK(ftell(f) == 28);
    CHECK(fclose(f) == 0);
    f = fopen(fn, "r");
    char buf[64];
    CHECK_STR(fgets(buf, sizeof buf, f), "line one\n");
    CHECK(fgetc(f) == 'l' && ungetc('X', f) == 'X' && fgetc(f) == 'X' && fgetc(f) == 'i');
    CHECK(ftell(f) == 11);
    fseek(f, 0, SEEK_SET);
    int n;
    CHECK(fscanf(f, "line %s line %d", buf, &n) == 2 && n == 2 && !strcmp(buf, "one"));
    char *line = NULL; size_t cap = 0;
    getline(&line, &cap, f);             /* rest of line 2 */
    CHECK(getline(&line, &cap, f) == 6 && !strcmp(line, "third\n"));
    CHECK(fread(buf, 1, 20, f) == 6 && !memcmp(buf, "abcdef", 6));
    CHECK(feof(f) && fgetc(f) == EOF);
    fseek(f, -3, SEEK_END);
    CHECK(fread(buf, 1, 3, f) == 3 && !memcmp(buf, "def", 3));
    fseek(f, 2, SEEK_SET); fseek(f, 3, SEEK_CUR);
    CHECK(fgetc(f) == 'o' && ftell(f) == 6);
    rewind(f); CHECK(!feof(f) && fgetc(f) == 'l');
    free(line);
    fclose(f);
    f = fopen(fn, "a+");
    fputs("++", f);
    fseek(f, 0, SEEK_SET);
    CHECK(fgetc(f) == 'l');
    fclose(f);
    f = fopen(fn, "r+");
    fseek(f, 5, SEEK_SET); fputc('O', f); fflush(f); fseek(f, 0, SEEK_SET);
    CHECK_STR(fgets(buf, sizeof buf, f), "line One\n");
    fclose(f);
    CHECK(fopen("/nonexistent/x", "r") == NULL && errno == ENOENT);
    CHECK(remove(fn) == 0);
    /* big binary transfer through the unbuffered-read fast path */
    f = fopen(fn, "w");
    static unsigned char big[5000], back[5000];
    for (int i = 0; i < 5000; i++) big[i] = i * 7;
    CHECK(fwrite(big, 1, 5000, f) == 5000);
    fclose(f);
    f = fopen(fn, "r");
    CHECK(fread(back, 1, 5000, f) == 5000 && !memcmp(big, back, 5000));
    fclose(f);
    remove(fn);
    /* stdout goes through the Linux write(2): visible proof the stream path works */
    printf("stdout ok: %d %s %.2f\n", 7, "x", 2.5);
    fflush(stdout);
    fprintf(stderr, "stderr ok\n");
}

static int atexit_ran;
static void at_exit_fn(void) { atexit_ran = 1; }

static void test_env(void) {
    CHECK(getenv("NOPE") == NULL);
    CHECK(setenv("FOO", "bar", 1) == 0 && !strcmp(getenv("FOO"), "bar"));
    CHECK(setenv("FOO", "baz", 0) == 0 && !strcmp(getenv("FOO"), "bar"));
    CHECK(setenv("FOO", "baz", 1) == 0 && !strcmp(getenv("FOO"), "baz"));
    setenv("A", "1", 1); setenv("B", "2", 1);
    unsetenv("FOO"); CHECK(!getenv("FOO") && !strcmp(getenv("A"), "1") && !strcmp(getenv("B"), "2"));
    CHECK(atexit(at_exit_fn) == 0);
}

/* regcomp/regexec sanity on the real 32-bit build; the broad comparison is regex_diff */
static int rx(const char *pat, int cf, const char *s, int so, int eo, int so1, int eo1) {
    regex_t r; regmatch_t m[3];
    if (regcomp(&r, pat, cf)) return 0;
    int e = regexec(&r, s, 3, m, 0), ok = so < 0 ? e == REG_NOMATCH : !e && m[0].rm_so == so && m[0].rm_eo == eo && m[1].rm_so == so1 && m[1].rm_eo == eo1;
    regfree(&r);
    return ok;
}
static void test_regex(void) {
    CHECK(rx("a\\(b*\\)c", 0, "xabbcx", 1, 5, 2, 4));
    CHECK(rx("(a|ab)(c|bcd)?", REG_EXTENDED, "abcd", 0, 4, 0, 1));
    CHECK(rx("^(x+)$", REG_EXTENDED | REG_ICASE, "XxX", 0, 3, 0, 3));
    CHECK(rx("\\(a*\\)\\1", 0, "aaaaa", 0, 4, 0, 2));
    CHECK(rx("z", 0, "abc", -1, 0, 0, 0));
    regex_t r; char eb[64];
    CHECK(regcomp(&r, "a\\{2", 0) == REG_EBRACE);
    regerror(REG_EBRACE, &r, eb, sizeof eb);
    CHECK_STR(eb, "Unmatched \\{");
}

int tmain(void) {
    test_string(); test_printf(); test_conv(); test_sort_rand(); test_malloc();
    test_new(); test_malloc_stress(); test_time(); test_scanf(); test_math(); test_setjmp(); test_stdio(); test_env(); test_regex();
    char b[64];
    snprintf(b, sizeof b, "%d checks, %d failed\n", checks, fails);
    write(1, b, strlen(b));
    return fails ? 1 : 0;
}

/* freestanding entry: Linux gives us argc at (%esp) */
__asm__(".globl _start\n_start:\n xor %ebp,%ebp\n and $-16,%esp\n call tmain\n"
        " sub $12,%esp\n push %eax\n call exit\n");
