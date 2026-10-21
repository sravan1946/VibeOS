/* stdlib.c - number conversion, qsort, env, process exit, misc */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <assert.h>

char **environ;

void __libc_init(int argc, char **argv, char **envp) { environ = envp; }

/* ---- integers ---- */

/* parses [ws][sign][0x]digits; returns magnitude, *ovf set if it exceeded 64 bits */
static unsigned long long conv(const char *s, char **end, int base, int *neg, int *ovf) {
    const char *p = s;
    unsigned long long v = 0;
    *neg = *ovf = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-' || *p == '+') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) base = p[0] == '0' ? 8 : 10;
    const char *digits = p;
    for (;; p++) {
        int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
        if (d >= base) break;
        if (v > (ULLONG_MAX - d) / base) *ovf = 1;
        v = v * base + d;
    }
    if (end) *end = (char *)(p == digits ? s : p);
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = conv(s, end, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = conv(s, end, base, &neg, &ovf);
    if (ovf || v > (unsigned long long)LLONG_MAX + neg) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? -(long long)v : (long long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = conv(s, end, base, &neg, &ovf);
    if (ovf || v > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return neg ? -(unsigned long)v : (unsigned long)v;
}

long strtol(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = conv(s, end, base, &neg, &ovf);
    if (ovf || v > (unsigned long)LONG_MAX + neg) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? -(long)v : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }

/* ---- floating point ---- */

static int match_ci(const char *p, const char *w) {
    while (*w && tolower((unsigned char)*p) == *w) p++, w++;
    return *w == 0;
}

/* Accumulates up to 19 digits exactly, scales in x87 long double (64-bit mantissa), so
 * results are right to ~1 ulp of double; not guaranteed correctly rounded. */
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
    long double v = m;
    if (v != 0) {
        int e = exp10 < 0 ? -exp10 : exp10;
        long double pw = 1, b = 10;
        if (e > 5000) e = 5000;
        for (; e; e >>= 1, b *= b)
            if (e & 1) pw *= b;
        v = exp10 < 0 ? v / pw : v * pw;
    }
    double d = (double)v;
    if (__builtin_isinf(d)) errno = ERANGE;
    return neg ? -d : d;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

/* ---- qsort (heapsort: no recursion, no scratch memory, O(n log n) worst case) ---- */

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void sift(char *base, size_t sz, size_t i, size_t n, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && cmp(base + c * sz, base + (c + 1) * sz) < 0) c++;
        if (cmp(base + i * sz, base + c * sz) >= 0) return;
        swap_bytes(base + i * sz, base + c * sz, sz);
        i = c;
    }
}

void qsort(void *vbase, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    char *base = vbase;
    if (n < 2) return;
    for (size_t i = n / 2; i-- > 0;) sift(base, sz, i, n, cmp);
    for (size_t e = n - 1; e > 0; e--) {
        swap_bytes(base, base + e * sz, sz);
        sift(base, sz, 0, e, cmp);
    }
}

void *bsearch(const void *key, const void *vbase, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    const char *base = vbase;
    while (n) {
        const char *mid = base + (n / 2) * sz;
        int c = cmp(key, mid);
        if (!c) return (void *)mid;
        if (c > 0) { base = mid + sz; n -= n / 2 + 1; }
        else n /= 2;
    }
    return NULL;
}

static unsigned rand_state = 1;
void srand(unsigned seed) { rand_state = seed; }
int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (rand_state ^ (rand_state >> 16)) >> 1;   /* fold the high bits in: LCG low bits have tiny periods */
}

/* ---- environment ---- */

static int env_owned;           /* environ array was malloc'd by us */

static char **env_find(const char *name, size_t len) {
    if (!environ) return NULL;
    for (char **e = environ; *e; e++)
        if (!strncmp(*e, name, len) && (*e)[len] == '=') return e;
    return NULL;
}

char *getenv(const char *name) {
    char **e = env_find(name, strlen(name));
    return e ? *e + strlen(name) + 1 : NULL;
}

static int env_add(char *entry) {
    size_t n = 0;
    while (environ && environ[n]) n++;
    char **ne = malloc((n + 2) * sizeof *ne);
    if (!ne) return -1;
    for (size_t i = 0; i < n; i++) ne[i] = environ[i];
    ne[n] = entry;
    ne[n + 1] = NULL;
    if (env_owned) free(environ);
    environ = ne;
    env_owned = 1;
    return 0;
}

int putenv(char *s) {
    char *eq = strchr(s, '=');
    if (!eq) { errno = EINVAL; return -1; }
    char **e = env_find(s, eq - s);
    if (e) { *e = s; return 0; }
    return env_add(s);
}

int setenv(const char *name, const char *val, int overwrite) {
    size_t nl = strlen(name);
    if (!nl || strchr(name, '=')) { errno = EINVAL; return -1; }
    char **e = env_find(name, nl);
    if (e && !overwrite) return 0;
    char *s = malloc(nl + strlen(val) + 2);                  /* ponytail: old string leaks on overwrite */
    if (!s) return -1;
    memcpy(s, name, nl);
    s[nl] = '=';
    strcpy(s + nl + 1, val);
    if (e) { *e = s; return 0; }
    return env_add(s);
}

int unsetenv(const char *name) {
    char **e = env_find(name, strlen(name));
    if (e) for (; *e; e++) *e = e[1];
    return 0;
}

/* ---- process ---- */

#define ATEXIT_MAX 32
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_n;

int atexit(void (*fn)(void)) {
    if (atexit_n >= ATEXIT_MAX) return -1;
    atexit_fns[atexit_n++] = fn;
    return 0;
}

void exit(int code) {
    while (atexit_n) atexit_fns[--atexit_n]();
    fflush(NULL);
    _exit(code);
}

void abort(void) {
    fflush(NULL);
    kill(getpid(), SIGKILL);
    _exit(134);
}

void __assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

int system(const char *cmd) {
    if (!cmd) return 1;
    char *argv[] = { "sh", "-c", (char *)cmd, NULL };
    fflush(NULL);
    int pid = spawnve("/bin/sh", argv, environ, NULL), st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return -1;
    return st;
}

/* lexical only: there are no symlinks to resolve */
char *realpath(const char *path, char *out) {
    char buf[PATH_MAX], *res = out ? out : malloc(PATH_MAX);
    if (!res) return NULL;
    size_t len = 0;
    if (path[0] != '/') {
        if (!getcwd(buf, sizeof buf)) goto fail;
        len = strlen(buf);
        if (len == 1) len = 0;
        memcpy(res, buf, len);
    }
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        const char *c = p;
        while (*p && *p != '/') p++;
        size_t cl = p - c;
        if (!cl || (cl == 1 && c[0] == '.')) continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (len && res[len - 1] != '/') len--;
            if (len) len--;
            continue;
        }
        if (len + cl + 2 > PATH_MAX) { errno = ENAMETOOLONG; goto fail; }
        res[len++] = '/';
        memcpy(res + len, c, cl);
        len += cl;
    }
    if (!len) res[len++] = '/';
    res[len] = 0;
    struct stat st;
    if (stat(res, &st) < 0) goto fail;
    return res;
fail:
    if (!out) free(res);
    return NULL;
}
