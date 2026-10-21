/* syscall.h - raw int 0x80 stubs plus the non-POSIX VibeOS calls (impl: lib/syscall.c) */
#pragma once
#include "abi.h"
#include <errno.h>
#include <sys/types.h>

static inline int __syscall0(int n) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n) : "memory"); return r;
}
static inline int __syscall1(int n, int a) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory"); return r;
}
static inline int __syscall2(int n, int a, int b) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory"); return r;
}
static inline int __syscall3(int n, int a, int b, int c) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}
static inline int __syscall4(int n, int a, int b, int c, int d) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory"); return r;
}
static inline int __syscall5(int n, int a, int b, int c, int d, int e) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory"); return r;
}
/* kernel result -> POSIX: -errno in [-4095,-1] becomes errno + -1 (other "negative" values are addresses) */
static inline int __sysret(int r) {
    if (r < 0 && r > -4096) { errno = -r; return -1; }
    return r;
}

uint32_t uptime_ms(void);
int yield(void);
int proclist(struct procinfo *buf, int max);
int sysinfo(struct sysinfo *si);
int beep(uint32_t hz, uint32_t ms);
int shutdown(int reboot);
int klog(char *buf, uint32_t n);
int win_create(struct win_info *wi);
int win_flip(int fd, const struct win_rect *damage);
int win_set(int fd, int what, uintptr_t arg);
int __readdir(int fd, struct dirent *d);
