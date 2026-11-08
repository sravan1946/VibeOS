/* compat.c - small POSIX leftovers: mmap/munmap/mprotect */
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <syscall.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    int r = __syscall5(SYS_mmap, (int)addr, len, prot | flags << 8, fd, off);
    if (r < 0 && r > -4096) { errno = -r; return MAP_FAILED; }
    return (void *)r;
}
int munmap(void *addr, size_t len) { return __sysret(__syscall2(SYS_munmap, (int)addr, len)); }
int mprotect(void *addr, size_t len, int prot) { return __sysret(__syscall3(SYS_mprotect, (int)addr, len, prot)); }
