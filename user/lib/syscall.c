/* syscall.c - one wrapper per SYS_*; POSIX-style (-1 + errno) where POSIX has the call.
 * This is the only C file that touches int 0x80 (the host test swaps it for stubs). */
#include <syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

#define P(x) ((int)(uintptr_t)(x))

void _exit(int code) {
    __syscall1(SYS_exit, code);
    for (;;) __syscall0(SYS_yield);
}

ssize_t read(int fd, void *buf, size_t n) { return __sysret(__syscall3(SYS_read, fd, P(buf), n)); }
ssize_t write(int fd, const void *buf, size_t n) { return __sysret(__syscall3(SYS_write, fd, P(buf), n)); }
int open(const char *path, int flags, ...) { return __sysret(__syscall2(SYS_open, P(path), flags)); }
int close(int fd) { return __sysret(__syscall1(SYS_close, fd)); }
off_t lseek(int fd, off_t off, int whence) { return __sysret(__syscall3(SYS_lseek, fd, off, whence)); }
int stat(const char *path, struct stat *st) { return __sysret(__syscall2(SYS_stat, P(path), P(st))); }
int fstat(int fd, struct stat *st) { return __sysret(__syscall2(SYS_fstat, fd, P(st))); }
int __readdir(int fd, struct dirent *d) { return __sysret(__syscall2(SYS_readdir, fd, P(d))); }
int mkdir(const char *path, mode_t mode) { return __sysret(__syscall1(SYS_mkdir, P(path))); }
int unlink(const char *path) { return __sysret(__syscall1(SYS_unlink, P(path))); }
int rmdir(const char *path) { return __sysret(__syscall1(SYS_rmdir, P(path))); }
int rename(const char *from, const char *to) { return __sysret(__syscall2(SYS_rename, P(from), P(to))); }
int chdir(const char *path) { return __sysret(__syscall1(SYS_chdir, P(path))); }

char *getcwd(char *buf, size_t n) {
    return __sysret(__syscall2(SYS_getcwd, P(buf), n)) < 0 ? NULL : buf;
}

int spawnve(const char *path, char *const argv[], char *const envp[], const struct spawn_opts *o) {
    return __sysret(__syscall4(SYS_spawn, P(path), P(argv), P(envp), P(o)));
}

pid_t waitpid(pid_t pid, int *status, int flags) {
    return __sysret(__syscall3(SYS_waitpid, pid, P(status), flags));
}
pid_t getpid(void) { return __syscall0(SYS_getpid); }
pid_t getppid(void) { return __syscall0(SYS_getppid); }

void *sbrk(int incr) {
    int r = __syscall1(SYS_sbrk, incr);
    if (r < 0 && r > -4096) { errno = -r; return (void *)-1; }
    return (void *)r;
}

int usleep(unsigned us) { __syscall1(SYS_sleep, (us + 999) / 1000); return 0; }
unsigned sleep(unsigned sec) { __syscall1(SYS_sleep, sec * 1000); return 0; }
uint32_t uptime_ms(void) { return __syscall0(SYS_uptime); }
time_t time(time_t *t) {
    time_t r = (time_t)(uint32_t)__syscall0(SYS_time);
    if (t) *t = r;
    return r;
}
int kill(pid_t pid, int sig) { return __sysret(__syscall2(SYS_kill, pid, sig)); }
int raise(int sig) { return kill(getpid(), sig); }

/* handlers return here; the kernel saved the interrupted context just above */
__attribute__((naked)) static void sig_restore(void) { __asm__ volatile("mov %0, %%eax; int $0x80" :: "i"(SYS_sigreturn)); }

sighandler_t signal(int sig, sighandler_t h) {
    int r = __syscall3(SYS_sigaction, sig, P(h), P(sig_restore));
    if (r < 0 && r > -4096) { errno = -r; return SIG_ERR; }
    return (sighandler_t)(uintptr_t)r;
}

int sigaction(int sig, const struct sigaction *sa, struct sigaction *old) {
    sighandler_t prev;
    if (sa) {
        if ((prev = signal(sig, sa->sa_handler)) == SIG_ERR) return -1;
    } else {                                              /* query: set and put back */
        if ((prev = signal(sig, SIG_DFL)) == SIG_ERR) return -1;
        signal(sig, prev);
    }
    if (old) { old->sa_handler = prev; old->sa_mask = 0; old->sa_flags = 0; }
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    if (how < SIG_BLOCK || how > SIG_SETMASK) { errno = EINVAL; return -1; }
    uint32_t r = __syscall2(SYS_sigprocmask, set ? how : SIG_BLOCK, set ? *set : 0);   /* the raw mask, never an error */
    if (old) *old = r;
    return 0;
}

int sigemptyset(sigset_t *s) { *s = 0; return 0; }
int sigfillset(sigset_t *s) { *s = ~1u; return 0; }
int sigaddset(sigset_t *s, int n) { if (n < 1 || n >= NSIG) { errno = EINVAL; return -1; } *s |= 1u << n; return 0; }
int sigdelset(sigset_t *s, int n) { if (n < 1 || n >= NSIG) { errno = EINVAL; return -1; } *s &= ~(1u << n); return 0; }
int sigismember(const sigset_t *s, int n) { return n > 0 && n < NSIG && (*s >> n & 1); }
int pipe(int fds[2]) { return __sysret(__syscall1(SYS_pipe, P(fds))); }
int dup(int fd) { return __sysret(__syscall1(SYS_dup, fd)); }
int dup2(int old, int new_) { return __sysret(__syscall2(SYS_dup2, old, new_)); }

int ioctl(int fd, int req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return __sysret(__syscall3(SYS_ioctl, fd, req, P(arg)));
}

int proclist(struct procinfo *buf, int max) { return __sysret(__syscall2(SYS_proclist, P(buf), max)); }
int sysinfo(struct sysinfo *si) { return __sysret(__syscall1(SYS_sysinfo, P(si))); }
int yield(void) { return __syscall0(SYS_yield); }
int poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
    return __sysret(__syscall3(SYS_poll, P(fds), n, timeout_ms));
}
int ftruncate(int fd, off_t len) { return __sysret(__syscall2(SYS_ftruncate, fd, len)); }
int win_create(struct win_info *wi) { return __sysret(__syscall1(SYS_win_create, P(wi))); }
int win_flip(int fd, const struct win_rect *damage) {
    return __sysret(__syscall2(SYS_win_flip, fd, P(damage)));
}
int win_set(int fd, int what, uintptr_t arg) { return __sysret(__syscall3(SYS_win_set, fd, what, P(arg))); }
int beep(uint32_t hz, uint32_t ms) { return __sysret(__syscall2(SYS_beep, hz, ms)); }
int shutdown(int reboot) { return __sysret(__syscall1(SYS_shutdown, reboot)); }
int klog(char *buf, uint32_t n) { return __sysret(__syscall2(SYS_klog, P(buf), n)); }
