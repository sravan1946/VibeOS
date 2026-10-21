#include "kernel.h"
#include "console.h"

struct out { char *buf; size_t n, pos; };

static void put(struct out *o, char c) {
    if (o->pos + 1 < o->n) o->buf[o->pos] = c;
    o->pos++;
}

static void put_num(struct out *o, uint64_t v, int base, int neg, int width, char pad, int upper) {
    char tmp[24]; int i = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[i++] = dig[v % base]; v /= base; } while (v);
    if (neg) { if (pad == '0') { put(o, '-'); width--; } else tmp[i++] = '-'; }
    for (int w = i; w < width; w++) put(o, pad);
    while (i) put(o, tmp[--i]);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out o = { buf, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        char pad = ' '; int width = 0, left = 0, lng = 0, prec = -1;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0, width, pad, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = lng >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            put_num(&o, v, base, 0, width, pad, *fmt == 'X');
            break;
        }
        case 'p':
            put(&o, '0'); put(&o, 'x');
            put_num(&o, (uintptr_t)va_arg(ap, void *), 16, 0, 8, '0', 0);
            break;
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0; while (s[len] && (prec < 0 || len < prec)) len++;
            if (!left) for (int w = len; w < width; w++) put(&o, ' ');
            for (int k = 0; k < len; k++) put(&o, s[k]);
            if (left) for (int w = len; w < width; w++) put(&o, ' ');
            break;
        }
        case '%': put(&o, '%'); break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* kernel log ring (dmesg / /dev/kmsg) */
#define KLOG_SIZE 32768
static char klog[KLOG_SIZE];
static size_t klog_len;                /* total bytes ever written */

static void klog_put(const char *s) {
    for (; *s; s++) klog[klog_len++ % KLOG_SIZE] = *s;
}

size_t klog_size(void) { return klog_len < KLOG_SIZE ? klog_len : KLOG_SIZE; }

size_t klog_read(char *buf, size_t off, size_t n) {
    size_t avail = klog_size(), start = klog_len - avail;
    if (off >= avail) return 0;
    if (n > avail - off) n = avail - off;
    for (size_t i = 0; i < n; i++) buf[i] = klog[(start + off + i) % KLOG_SIZE];
    return n;
}

/* kprintf goes to serial always, and to the boot console until the
 * desktop takes over the screen. */
void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uint32_t f = irq_save();
    for (char *p = buf; *p; p++) {
        if (*p == '\n') serial_putc('\r');
        serial_putc(*p);
    }
    klog_put(buf);
    bootcon_write(buf);
    irq_restore(f);
}

NORETURN void panic(const char *fmt, ...) {
    char buf[256];
    cli();
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    bootcon_panic(buf, 0);
    for (;;) hlt();
}
