#pragma once
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
extern char **environ;
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);   /* = strtod widened: 53-bit precision */
int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned seed);
char *getenv(const char *name);
int setenv(const char *name, const char *val, int overwrite);
int unsetenv(const char *name);
int putenv(char *s);
void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*fn)(void));
int system(const char *cmd);
char *realpath(const char *path, char *out);
