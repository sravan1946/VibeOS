/* benchutil.h - rdtsc spans for the apps' --bench modes; results go to /dev/kmsg (serial). Use: uint64_t t = tsc(); ...; blog("x %u us", us(tsc() - t)); */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscall.h>
static inline uint64_t tsc(void) { uint32_t a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return (uint64_t)d << 32 | a; }
static inline uint32_t tsc_khz(void) {            /* calibrated once against the 1 kHz timer (50 ms) */
    static uint32_t k;
    if (!k) { uint32_t t0 = uptime_ms(); while (uptime_ms() == t0); t0 = uptime_ms(); uint64_t c0 = tsc(); while (uptime_ms() - t0 < 50); k = (uint32_t)((tsc() - c0) / (uptime_ms() - t0)); }
    return k;
}
static inline uint32_t us(uint64_t cycles) { return (uint32_t)(cycles * 1000 / tsc_khz()); }
static inline void blog(const char *fmt, ...) {
    char b[200]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > 0 && n < (int)sizeof b - 1) b[n++] = '\n';
    int fd = open("/dev/kmsg", O_WRONLY); if (fd >= 0) { write(fd, b, n); close(fd); }
}
