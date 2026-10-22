/* compat.c - small POSIX leftovers a compiler needs: gettimeofday, mprotect, execvp */
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

int mprotect(void *addr, size_t len, int prot) { return 0; }

/* no exec in the kernel: run the program as a child, wait, and exit with its status */
int execvp(const char *file, char *const argv[]) {
    int pid = spawnvp(file, argv), st = 0;
    if (pid < 0) return -1;
    waitpid(pid, &st, 0);
    _exit(st);
}
