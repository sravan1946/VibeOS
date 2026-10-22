/* vibe_qjs.h - force-included (-include) into every QuickJS source file. It supplies the POSIX and C99 pieces our
 * libc lacks under qjs_* names, behind macros, so they can never collide with libc additions made later (a later
 * libc prototype for, say, expm1 is renamed along with the call sites and still binds to the static inline here).
 * The header is included before the QuickJS file includes anything, so the real headers are pulled in first and
 * only the identifiers QuickJS uses afterwards are renamed. The non-inline qjs_* functions live in
 * user/bin/qjs/sys.c, which only /bin/qjs links (the browser uses the engine without quickjs-libc). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syscall.h>

#ifndef alloca
#define alloca __builtin_alloca
#endif

/* ---------- time ---------- */
struct qjs_timespec { long tv_sec, tv_nsec; };
#define timespec qjs_timespec
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#endif
static inline int qjs_clock_gettime(int clk, struct timespec *ts) {     /* ms resolution */
    struct timeval tv;
    unsigned ms = __syscall0(SYS_uptime);
    if (clk == CLOCK_REALTIME) { gettimeofday(&tv, 0); ts->tv_sec = tv.tv_sec; ts->tv_nsec = tv.tv_usec * 1000L; }
    else { ts->tv_sec = ms / 1000; ts->tv_nsec = (long)(ms % 1000) * 1000000; }
    return 0;
}
#define clock_gettime qjs_clock_gettime
static inline int qjs_nanosleep(const struct timespec *t, struct timespec *rem) {
    (void)rem;
    return usleep(t->tv_sec * 1000000 + t->tv_nsec / 1000);
}
#define nanosleep qjs_nanosleep

/* ---------- C99 math the libc doesn't have (x87, long double intermediates) ---------- */
static inline double qjs_rint(double x) { long double r = x; __asm__("frndint" : "+t"(r)); return r; }   /* nearest-even */
static inline long qjs_lrint(double x) { return (long)qjs_rint(x); }
static inline double qjs_fmin(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a < b ? a : b; }
static inline double qjs_fmax(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a > b ? a : b; }
static inline double qjs_log1p(double x) {
    if (fabs(x) < 0.29) {                               /* fyl2xp1: ln2 * log2(1 + x), exact for small x */
        long double r = x, ln2 = 0.693147180559945309417232121458176568L;
        __asm__("fyl2xp1" : "+t"(r) : "u"(ln2) : "st(1)");
        return r;
    }
    return log(1 + x);
}
static inline double qjs_expm1(double x) {
    if (fabs(x) < 0.69) {                               /* f2xm1: 2^y - 1 for |y| <= 1 */
        long double y = x * 1.4426950408889634073599246810018921L;
        __asm__("f2xm1" : "+t"(y));
        return y;
    }
    return exp(x) - 1;
}
static inline double qjs_cbrt(double x) {
    if (x == 0 || isnan(x) || isinf(x)) return x;
    long double a = fabs(x), y = pow(a, 1.0 / 3);
    y -= (y * y * y - a) / (3 * y * y);                 /* one Newton step: exact cubes come out exact */
    return x < 0 ? -(double)y : (double)y;
}
static inline double qjs_asinh(double x) {
    double a = fabs(x), r = a < 0x1p-28 ? a : a > 0x1p28 ? log(a) + 0.693147180559945309 : qjs_log1p(a + a * a / (1 + sqrt(1 + a * a)));
    return signbit(x) ? -r : r;
}
static inline double qjs_acosh(double x) { return x < 1 ? NAN : x > 0x1p28 ? log(x) + 0.693147180559945309 : log(x + sqrt((x - 1) * (x + 1))); }
static inline double qjs_atanh(double x) {
    double a = fabs(x), r = a > 1 ? NAN : 0.5 * qjs_log1p(2 * a / (1 - a));
    return signbit(x) ? -r : r;
}
#define lrint qjs_lrint
#define rint qjs_rint
#define fmin qjs_fmin
#define fmax qjs_fmax
#define log1p qjs_log1p
#define expm1 qjs_expm1
#define cbrt qjs_cbrt
#define asinh qjs_asinh
#define acosh qjs_acosh
#define atanh qjs_atanh

/* ---------- quickjs-libc (std/os modules) ---------- */
#define ftello ftell
#define fseeko fseek
#define lstat stat                                      /* no symlinks */
#ifndef POLLERR
#define POLLERR 8
#define POLLNVAL 32
#endif

/* POSIX stat: ours is the kernel's struct (no dev/uid/gid/rdev, seconds-only times) */
struct qjs_stat { uint32_t st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid, st_rdev, st_size, st_blocks;
                  struct timespec st_atim, st_mtim, st_ctim; };
int qjs_stat(const char *path, struct qjs_stat *st);
int qjs_fstat(int fd, struct qjs_stat *st);
#define stat qjs_stat
#define fstat qjs_fstat

/* termios with the usual fields; only c_lflag (ECHO, ICANON, ISIG) reaches the kernel */
struct qjs_termios { uint32_t c_iflag, c_oflag, c_cflag, c_lflag; uint8_t c_cc[32]; };
#define IGNBRK 0x001
#define BRKINT 0x002
#define PARMRK 0x008
#define ISTRIP 0x020
#define INLCR 0x040
#define IGNCR 0x080
#define ICRNL 0x100
#define IXON 0x400
#define OPOST 0x01
#define ECHONL 0x40
#define IEXTEN 0x8000
#define CSIZE 0x30
#define CS8 0x30
#define PARENB 0x100
#define VTIME 5
#define VMIN 6
int qjs_tcgetattr(int fd, struct qjs_termios *t);
int qjs_tcsetattr(int fd, int when, const struct qjs_termios *t);
#define termios qjs_termios
#define tcgetattr qjs_tcgetattr
#define tcsetattr qjs_tcsetattr

/* no fork/exec, popen, links or utimes in VibeOS: these fail with ENOSYS (os.exec, std.popen, os.symlink...) */
FILE *qjs_popen(const char *cmd, const char *mode);
int qjs_pclose(FILE *f);
pid_t qjs_fork(void);
int qjs_execve(const char *path, char *const argv[], char *const envp[]);
int qjs_setuid(int id), qjs_setgid(int id);
int qjs_symlink(const char *a, const char *b);
ssize_t qjs_readlink(const char *p, char *buf, size_t n);
int qjs_utimes(const char *p, const struct timeval tv[2]);
long qjs_sysconf(int name);
#define popen qjs_popen
#define pclose qjs_pclose
#define fork qjs_fork
#define execve qjs_execve
#define setuid qjs_setuid
#define setgid qjs_setgid
#define symlink qjs_symlink
#define readlink qjs_readlink
#define utimes qjs_utimes
#define sysconf qjs_sysconf
#ifndef _SC_OPEN_MAX
#define _SC_OPEN_MAX 4
#endif
#ifndef SIGCONT                                        /* Linux numbers, like the rest of abi.h */
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#endif
#ifndef S_IFLNK
#define S_IFBLK 0x6000
#define S_IFLNK 0xA000
#define S_ISUID 04000
#define S_ISGID 02000
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(s) 0
#define WTERMSIG(s) 0
#endif
