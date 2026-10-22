/* compat.c - small POSIX leftovers: gettimeofday, mmap/munmap/mprotect */
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <syscall.h>

int gettimeofday(struct timeval *tv, void *tz) {
    if (tv) {
        unsigned ms = __syscall0(SYS_uptime);
        static time_t t0; static unsigned ms0;    /* RTC second + the uptime at that moment: ms resolution afterwards */
        if (!t0) { t0 = __syscall0(SYS_time); ms0 = ms; }
        tv->tv_sec = t0 + (ms - ms0) / 1000;
        tv->tv_usec = (ms - ms0) % 1000 * 1000;
    }
    return 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int r = __syscall5(SYS_mmap, (int)addr, len, prot | flags << 8, fd, off);
    if (r < 0 && r > -4096) { errno = -r; return MAP_FAILED; }
    return (void *)r;
}
int munmap(void *addr, size_t len) { return __sysret(__syscall2(SYS_munmap, (int)addr, len)); }
int mprotect(void *addr, size_t len, int prot) { return __sysret(__syscall3(SYS_mprotect, (int)addr, len, prot)); }
