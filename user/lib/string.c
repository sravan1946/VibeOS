/* string.c - built with -fno-tree-loop-distribute-patterns so loops here don't turn into calls to themselves */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias, aligned(1))) u32u;

/* Short copies are a few overlapping word moves (no rep startup; the sources are read before the destination can
 * overlap them, so memcpy only, never memmove); from 32 bytes up rep movsb, which current CPUs run at full speed. */
void *memcpy(void *d, const void *s, size_t n) {
    char *dp = d;
    const char *sp = s;
    if (n < 32) {
        if (n >= 16) { __builtin_memcpy(dp, sp, 16); __builtin_memcpy(dp + n - 16, sp + n - 16, 16); }
        else if (n >= 8) { __builtin_memcpy(dp, sp, 8); __builtin_memcpy(dp + n - 8, sp + n - 8, 8); }
        else if (n >= 4) { __builtin_memcpy(dp, sp, 4); __builtin_memcpy(dp + n - 4, sp + n - 4, 4); }
        else if (n) { dp[0] = sp[0]; dp[n >> 1] = sp[n >> 1]; dp[n - 1] = sp[n - 1]; }
        return d;
    }
    __asm__ volatile("rep movsb" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    char *dp = d;
    const char *sp = s;
    if (d == s || !n) return d;
    if (d < s || dp >= sp + n) {                          /* forward is safe, word by word */
        if (n >= 32) { __asm__ volatile("rep movsb" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory"); return d; }
        while (n--) *dp++ = *sp++;
        return d;
    }
    dp += n; sp += n;                                     /* overlapping, d above s: backward */
    while (n >= 4) { dp -= 4; sp -= 4; n -= 4; *(u32u *)dp = *(const u32u *)sp; }
    while (n--) *--dp = *--sp;
    return d;
}

void *memset(void *d, int c, size_t n) {
    char *dp = d;
    uint32_t v = (unsigned char)c * 0x01010101u;
    if (n < 32) {
        if (n >= 16) { *(u32u *)dp = v; *(u32u *)(dp + 4) = v; *(u32u *)(dp + 8) = v; *(u32u *)(dp + 12) = v;
                       dp += n - 16; *(u32u *)dp = v; *(u32u *)(dp + 4) = v; *(u32u *)(dp + 8) = v; *(u32u *)(dp + 12) = v; }
        else if (n >= 8) { *(u32u *)dp = v; *(u32u *)(dp + 4) = v; *(u32u *)(dp + n - 8) = v; *(u32u *)(dp + n - 4) = v; }
        else if (n >= 4) { *(u32u *)dp = v; *(u32u *)(dp + n - 4) = v; }
        else if (n) { dp[0] = c; dp[n >> 1] = c; dp[n - 1] = c; }
        return d;
    }
    __asm__ volatile("rep stosb" : "+D"(dp), "+c"(n) : "a"(c) : "memory");
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n >= 4 && *(const u32u *)x == *(const u32u *)y; n -= 4, x += 4, y += 4) {}
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

/* word at a time: an aligned word never crosses a page, so reading past the NUL inside it is safe */
size_t strlen(const char *s) {
    const char *p = s;
    while ((uintptr_t)p & 3) { if (!*p) return p - s; p++; }
    const uint32_t *w = (const uint32_t *)p;
    while (!((*w - 0x01010101u) & ~*w & 0x80808080u)) w++;
    for (p = (const char *)w; *p; p++) {}
    return p - s;
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n) {
    char *e = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}

size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}

size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return NULL;
}

size_t strspn(const char *s, const char *set) {
    size_t i = 0;
    while (s[i] && strchr(set, s[i])) i++;
    return i;
}

size_t strcspn(const char *s, const char *set) {
    size_t i = 0;
    while (s[i] && !strchr(set, s[i])) i++;
    return i;
}

char *strpbrk(const char *s, const char *set) {
    s += strcspn(s, set);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    s += strspn(s, delim);
    if (!*s) { *save = s; return NULL; }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) *s++ = 0;
    *save = s;
    return tok;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    return p ? memcpy(p, s, n) : NULL;
}

char *strndup(const char *s, size_t n) {
    n = strnlen(s, n);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* the only locale is "C" */
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
size_t strxfrm(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) strlcpy(d, s, n);
    return l;
}
