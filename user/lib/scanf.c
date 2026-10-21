/* scanf.c - sscanf/fscanf/scanf: %d %i %u %x %o %p %f %e %g %s %c %[set] %n %%, width, '*', hh h l ll z j L */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>

struct src {
    int (*get)(struct src *);
    void (*unget)(struct src *, int);
    void *p;
    int n;                      /* chars consumed */
};

struct ss { const char *s; };

static int str_get(struct src *r) {
    struct ss *x = r->p;
    return *x->s ? (unsigned char)*x->s++ : EOF;
}
static void str_unget(struct src *r, int c) { if (c != EOF) ((struct ss *)r->p)->s--; }
static int file_get(struct src *r) { return fgetc(r->p); }
static void file_unget(struct src *r, int c) { if (c != EOF) ungetc(c, r->p); }

static int get(struct src *r) {
    int c = r->get(r);
    if (c != EOF) r->n++;
    return c;
}
static void unget(struct src *r, int c) {
    if (c != EOF) { r->n--; r->unget(r, c); }
}
static int skip_ws(struct src *r) {
    int c;
    while ((c = get(r)) != EOF && isspace(c)) {}
    unget(r, c);
    return c;
}

enum { L_NONE, L_HH, L_H, L_L, L_LL, L_Z, L_J, L_BIGL };

static int scan(struct src *r, const char *fmt, va_list ap) {
    int nconv = 0, c;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) { skip_ws(r); continue; }
        if (*fmt != '%') {
            c = get(r);
            if (c != *fmt) { unget(r, c); return nconv || c != EOF ? nconv : EOF; }
            continue;
        }
        fmt++;
        int sup = 0, width = 0, len = L_NONE;
        if (*fmt == '*') { sup = 1; fmt++; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + *fmt++ - '0';
        if (!width) width = INT_MAX;
        switch (*fmt) {
        case 'h': len = L_H; fmt++; if (*fmt == 'h') { len = L_HH; fmt++; } break;
        case 'l': len = L_L; fmt++; if (*fmt == 'l') { len = L_LL; fmt++; } break;
        case 'z': len = L_Z; fmt++; break;
        case 'j': len = L_J; fmt++; break;
        case 'L': len = L_BIGL; fmt++; break;
        }
        char conv = *fmt;
        if (!conv) break;
        if (conv == '%') {
            skip_ws(r);
            c = get(r);
            if (c != '%') { unget(r, c); return nconv; }
            continue;
        }
        if (conv == 'n') {
            if (!sup) *va_arg(ap, int *) = r->n;
            continue;
        }
        if (conv != 'c' && conv != '[' && skip_ws(r) == EOF) return nconv ? nconv : EOF;

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') {
            char buf[80];
            int n = 0, base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            int lim = width < 78 ? width : 78, digits = 0;
            c = get(r);
            if ((c == '-' || c == '+') && n < lim) { buf[n++] = c; c = get(r); }
            if (c == '0' && n < lim && (base == 0 || base == 16)) {     /* 0, 0x prefix */
                buf[n++] = c;
                digits = 1;
                c = get(r);
                if ((c == 'x' || c == 'X') && n < lim) {
                    buf[n++] = c;
                    digits = 0;
                    c = get(r);
                    base = 16;
                } else if (base == 0) base = 8;
            }
            if (base == 0) base = 10;
            for (; c != EOF && n < lim; c = get(r)) {
                int d = isdigit(c) ? c - '0' : isalpha(c) ? tolower(c) - 'a' + 10 : 99;
                if (d >= base) break;
                buf[n++] = c;
                digits = 1;
            }
            unget(r, c);
            if (!digits) return nconv;
            buf[n] = 0;
            if (sup) continue;
            if (conv == 'd' || conv == 'i') {
                long long v = strtoll(buf, NULL, base);
                switch (len) {
                case L_HH: *va_arg(ap, signed char *) = v; break;
                case L_H: *va_arg(ap, short *) = v; break;
                case L_L: *va_arg(ap, long *) = v; break;
                case L_LL: *va_arg(ap, long long *) = v; break;
                case L_Z: *va_arg(ap, ssize_t *) = v; break;
                case L_J: *va_arg(ap, intmax_t *) = v; break;
                default: *va_arg(ap, int *) = v;
                }
            } else {
                unsigned long long v = strtoull(buf, NULL, base);
                switch (conv == 'p' ? L_Z : len) {
                case L_HH: *va_arg(ap, unsigned char *) = v; break;
                case L_H: *va_arg(ap, unsigned short *) = v; break;
                case L_L: *va_arg(ap, unsigned long *) = v; break;
                case L_LL: *va_arg(ap, unsigned long long *) = v; break;
                case L_Z:
                    if (conv == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                    else *va_arg(ap, size_t *) = v;
                    break;
                case L_J: *va_arg(ap, uintmax_t *) = v; break;
                default: *va_arg(ap, unsigned *) = v;
                }
            }
            nconv++;
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a') {
            char buf[80];
            int n = 0, lim = width < 78 ? width : 78, digits = 0, dot = 0;
            c = get(r);
            if ((c == '-' || c == '+') && n < lim) { buf[n++] = c; c = get(r); }
            if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {          /* inf / nan */
                for (; c != EOF && isalpha(c) && n < lim; c = get(r)) buf[n++] = c;
                digits = 1;
            } else {
                for (; c != EOF && n < lim; c = get(r)) {
                    if (isdigit(c)) digits = 1;
                    else if (c == '.' && !dot) dot = 1;
                    else break;
                    buf[n++] = c;
                }
                if (digits && (c == 'e' || c == 'E') && n < lim - 1) {
                    int save = n;
                    buf[n++] = c;
                    c = get(r);
                    if ((c == '-' || c == '+') && n < lim - 1) { buf[n++] = c; c = get(r); }
                    if (isdigit(c)) {
                        for (; c != EOF && isdigit(c) && n < lim; c = get(r)) buf[n++] = c;
                    } else n = save;     /* ponytail: "1e" leaves the 'e' consumed */
                }
            }
            unget(r, c);
            if (!digits) return nconv;
            buf[n] = 0;
            if (!sup) {
                double v = strtod(buf, NULL);
                if (len == L_L) *va_arg(ap, double *) = v;
                else if (len == L_BIGL) *va_arg(ap, long double *) = v;
                else *va_arg(ap, float *) = (float)v;
                nconv++;
            }
        } else if (conv == 's') {
            char *d = sup ? NULL : va_arg(ap, char *);
            int n = 0;
            for (c = get(r); c != EOF && !isspace(c) && n < width; c = get(r)) {
                if (d) d[n] = c;
                n++;
            }
            unget(r, c);
            if (d) d[n] = 0;
            if (!n) return nconv;
            if (!sup) nconv++;
        } else if (conv == 'c') {
            char *d = sup ? NULL : va_arg(ap, char *);
            int n = 0;
            if (width == INT_MAX) width = 1;
            for (; n < width; n++) {
                c = get(r);
                if (c == EOF) break;
                if (d) d[n] = c;
            }
            if (n < width) return nconv || n ? nconv : EOF;
            if (!sup) nconv++;
        } else if (conv == '[') {
            unsigned char set[256];
            int neg = 0;
            fmt++;
            if (*fmt == '^') { neg = 1; fmt++; }
            memset(set, 0, sizeof set);
            if (*fmt == ']') set[(unsigned char)*fmt++] = 1;
            while (*fmt && *fmt != ']') {
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                    for (int ch = (unsigned char)fmt[0]; ch <= (unsigned char)fmt[2]; ch++) set[ch] = 1;
                    fmt += 3;
                } else set[(unsigned char)*fmt++] = 1;
            }
            char *d = sup ? NULL : va_arg(ap, char *);
            int n = 0;
            for (c = get(r); c != EOF && set[c] != neg && n < width; c = get(r)) {
                if (d) d[n] = c;
                n++;
            }
            unget(r, c);
            if (d) d[n] = 0;
            if (!n) return nconv || c != EOF ? nconv : EOF;
            if (!sup) nconv++;
        } else return nconv;                 /* unknown conversion */
    }
    return nconv;
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    struct ss x = { s };
    struct src r = { str_get, str_unget, &x, 0 };
    return scan(&r, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    struct src r = { file_get, file_unget, f, 0 };
    return scan(&r, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}
