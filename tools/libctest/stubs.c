/* stubs.c - stands in for user/lib/syscall.c when the libc runs on the Linux host
 * (32-bit freestanding binary, Linux int 0x80 numbers). Only what the tested files reference. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <syscall.h>

static int lx(int n, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static int ret(int r) { if (r < 0 && r > -4096) { errno = -r; return -1; } return r; }

void _exit(int code) { lx(1, code, 0, 0); for (;;) {} }
ssize_t read(int fd, void *b, size_t n) { return ret(lx(3, fd, (int)b, n)); }
ssize_t write(int fd, const void *b, size_t n) { return ret(lx(4, fd, (int)b, n)); }
int open(const char *p, int fl, ...) { return ret(lx(5, (int)p, fl, 0644)); }
int close(int fd) { return ret(lx(6, fd, 0, 0)); }
off_t lseek(int fd, off_t o, int w) { return ret(lx(19, fd, o, w)); }
int unlink(const char *p) { return ret(lx(10, (int)p, 0, 0)); }
int rmdir(const char *p) { return ret(lx(40, (int)p, 0, 0)); }
int rename(const char *a, const char *b) { return ret(lx(38, (int)a, (int)b, 0)); }
int dup2(int a, int b) { return ret(lx(63, a, b, 0)); }
int isatty(int fd) { return 0; }

#define ARENA (16 << 20)
static char arena[ARENA];
static size_t brk_off;
void *sbrk(int incr) {
    if (brk_off + incr > ARENA) { errno = ENOMEM; return (void *)-1; }
    void *p = arena + brk_off;
    brk_off += incr;
    return p;
}

uint32_t uptime_ms(void) { return 1234; }
time_t time(time_t *t) { if (t) *t = 1700000000; return 1700000000; }

#define NOSYS(type, name, args) type name args { errno = ENOSYS; return (type)-1; }
NOSYS(int, stat, (const char *p, struct stat *s))
NOSYS(int, spawnve, (const char *p, char *const a[], char *const e[], const struct spawn_opts *o))
NOSYS(pid_t, waitpid, (pid_t p, int *s, int f))
NOSYS(int, kill, (pid_t p, int s))
pid_t getpid(void) { return 42; }
char *getcwd(char *b, size_t n) { errno = ENOSYS; return NULL; }
