/* abi.h - the VibeOS kernel/userland contract. Shared verbatim by both sides.
 *
 * Syscalls: int 0x80, EAX = number, args in EBX ECX EDX ESI EDI,
 * result in EAX; negative values are -errno. */
#pragma once
#include <stdint.h>

enum {
    SYS_exit = 1,     /* (int code) */
    SYS_read,         /* (int fd, void *buf, uint32 n) -> bytes */
    SYS_write,        /* (int fd, const void *buf, uint32 n) -> bytes */
    SYS_open,         /* (const char *path, int flags) -> fd */
    SYS_close,        /* (int fd) */
    SYS_lseek,        /* (int fd, int32 off, int whence) -> new offset */
    SYS_stat,         /* (const char *path, struct stat *st) */
    SYS_fstat,        /* (int fd, struct stat *st) */
    SYS_readdir,      /* (int fd, struct dirent *d) -> 1 entry, 0 end */
    SYS_mkdir,        /* (const char *path) */
    SYS_unlink,       /* (const char *path) - files only */
    SYS_rmdir,        /* (const char *path) - empty dirs only */
    SYS_rename,       /* (const char *old, const char *new) */
    SYS_chdir,        /* (const char *path) */
    SYS_getcwd,       /* (char *buf, uint32 n) -> length */
    SYS_spawn,        /* (const char *path, char *const argv[], char *const envp[], const struct spawn_opts *o) -> pid */
    SYS_waitpid,      /* (int pid, int *status, int flags) -> pid, 0 if WNOHANG and none */
    SYS_getpid,       /* () */
    SYS_sbrk,         /* (int32 incr) -> old break */
    SYS_sleep,        /* (uint32 ms) */
    SYS_uptime,       /* () -> ms since boot */
    SYS_time,         /* () -> unix seconds (from the RTC) */
    SYS_kill,         /* (int pid, int sig) */
    SYS_pipe,         /* (int fds[2]) */
    SYS_dup2,         /* (int old, int new) -> new */
    SYS_ioctl,        /* (int fd, int req, void *arg) */
    SYS_proclist,     /* (struct procinfo *buf, int max) -> count */
    SYS_sysinfo,      /* (struct sysinfo *si) */
    SYS_yield,        /* () */
    SYS_poll,         /* (struct pollfd *fds, int n, int timeout_ms) -> ready count, 0 on timeout */
    SYS_ftruncate,    /* (int fd, uint32 len) */
    SYS_dup,          /* (int fd) -> new fd */
    SYS_getppid,      /* () */
    SYS_win_create = 40, /* (struct win_info *wi) -> fd; fills wi->pixels */
    SYS_win_flip,     /* (int fd, const struct win_rect *damage or NULL=all) */
    SYS_win_set,      /* (int fd, int what, uintptr arg) WIN_SET_* */
    SYS_beep = 50,    /* (uint32 hz, uint32 ms) */
    SYS_shutdown,     /* (int reboot) */
    SYS_klog,         /* (char *buf, uint32 n) -> bytes of kernel log copied */
    SYS_socket = 60,  /* (int domain, int type, int proto) -> fd; AF_INET, SOCK_STREAM/DGRAM/RAW(proto 1 = ICMP) */
    SYS_connect,      /* (int fd, const struct sockaddr_in *a, uint32 len): TCP blocks until established */
    SYS_bind,         /* (int fd, const struct sockaddr_in *a, uint32 len) */
    SYS_sendto,       /* (int fd, const void *buf, uint32 n, int flags, const struct sockaddr_in *to or NULL) */
    SYS_recvfrom,     /* (int fd, void *buf, uint32 n, int flags, struct sockaddr_in *from or NULL); MSG_DONTWAIT */
    SYS_resolve,      /* (const char *name, uint32 *ip_be) -> 0 / -errno; also accepts dotted quads */
    SYS_netinfo,      /* (struct netinfo *ni) */
    SYS_listen,       /* (int fd, int backlog): TCP passive open; autobinds if unbound */
    SYS_accept,       /* (int fd, struct sockaddr_in *peer or NULL, int flags) -> new fd; MSG_DONTWAIT */
    SYS_sigaction = 70, /* (int sig, uintptr handler, uintptr restorer) -> old handler (SIG_DFL 0, SIG_IGN 1) */
    SYS_sigreturn,    /* () from the restorer: resume what the handler interrupted */
    SYS_sigprocmask,  /* (int how, uint32 set) -> old mask; SIG_BLOCK 0 / SIG_UNBLOCK 1 / SIG_SETMASK 2, bit n = signal n */
    SYS_exec = 80,    /* (const char *path, char *const argv[], char *const envp[]): new image, same pid, fds (minus
                         FD_CLOEXEC), cwd, mask and ignored signals; handlers reset to default. Returns only on error */
    SYS_fcntl,        /* (int fd, int cmd, int arg) F_DUPFD / F_GETFD / F_SETFD (FD_CLOEXEC) / F_GETFL / F_SETFL */
    SYS_thread_create, /* (uintptr entry, uintptr stack, int *errno) -> tid: a thread of this process runs at entry
                         with esp = stack (no arguments in registers); errno, if given, becomes per thread */
    SYS_thread_exit,  /* (int value): the main thread waits for the others, then the process exits with 0 */
    SYS_thread_join,  /* (int tid, int *value): wait for a thread of this process and free it */
    SYS_gettid,       /* () -> this thread's id (= getpid() in the main thread) */
    SYS_futex,        /* (int *addr, int op, int val): FUTEX_WAIT sleeps if *addr == val (-EAGAIN otherwise), FUTEX_WAKE wakes waiters */
    SYS_MAX
};

/* errno */
enum {
    EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, ENXIO = 6, E2BIG = 7, ENOEXEC = 8,
    EBADF = 9, ECHILD = 10, EAGAIN = 11, ENOMEM = 12, EACCES = 13, EFAULT = 14, EBUSY = 16,
    EEXIST = 17, EXDEV = 18, ENODEV = 19, ENOTDIR = 20, EISDIR = 21, EINVAL = 22, ENFILE = 23,
    EMFILE = 24, ENOTTY = 25, EFBIG = 27, ENOSPC = 28, ESPIPE = 29, EROFS = 30, EPIPE = 32,
    ERANGE = 34, ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39,
    EMSGSIZE = 90, EAFNOSUPPORT = 97, EADDRINUSE = 98, ENETDOWN = 100, ECONNRESET = 104,
    ENOTCONN = 107, ETIMEDOUT = 110, ECONNREFUSED = 111, EHOSTUNREACH = 113, EPROTONOSUPPORT = 93,
};

/* open flags */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_NONBLOCK 0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC  0x80000

/* fcntl */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4         /* only O_APPEND and O_NONBLOCK change */
#define FD_CLOEXEC 1
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define WNOHANG 1

/* file types (st_mode high bits, ext2-compatible) */
#define S_IFMT   0xF000
#define S_IFIFO  0x1000
#define S_IFSOCK 0xC000
#define S_IFCHR  0x2000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

struct stat {
    uint32_t st_ino;
    uint32_t st_mode;     /* S_IF* | permission bits */
    uint32_t st_nlink;
    uint32_t st_size;
    uint32_t st_mtime, st_ctime, st_atime;
    uint32_t st_blocks;   /* 512-byte blocks */
};

#define NAME_MAX 255
#define PATH_MAX 512
struct dirent {
    uint32_t d_ino;
    uint32_t d_type;      /* DT_* */
    char d_name[NAME_MAX + 1];
};
#define DT_UNKNOWN 0
#define DT_REG 1
#define DT_DIR 2
#define DT_CHR 3
#define DT_FIFO 4

/* spawn: child's fds 0..2 come from the parent's fd[i] (-1 = same number as the parent's) */
struct spawn_opts {
    int fd[3];
    const char *cwd;      /* NULL = inherit */
    int flags;            /* SPAWN_* */
};
#define SPAWN_NEWTTY 1    /* reserved */

/* signals: the default action terminates (exit code 128 + n) except for SIGCHLD and SIGWINCH;
 * a handler runs on the user stack and returns through its restorer (SYS_sigreturn) */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGWINCH 28
#define NSIG     32

/* poll */
#define POLLIN  1
#define POLLOUT 4
#define POLLHUP 16
struct pollfd { int fd; short events, revents; };

/* ioctl */
#define TCGETS     0x5401   /* struct termios * */
#define TCSETS     0x5402   /* struct termios * */
#define TIOCGWINSZ 0x5413   /* struct winsize * */
#define TIOCSPGRP  0x5410   /* int *pid : foreground process (gets SIGINT on ^C) */
#define FIONREAD   0x541B   /* int * bytes readable */
/* /dev/dsp: write() takes AUDIO_RATE stereo s16le frames and blocks while the DMA ring is full */
#define AUDIO_RATE   48000
#define AUDIO_GETQ   0x4101   /* int * bytes queued in the ring (play latency) */
#define AUDIO_SETVOL 0x4102   /* int * master volume 0..100 */
#define AUDIO_GETVOL 0x4103   /* int * */
struct termios { uint32_t c_lflag; };
#define ECHO   0x0008
#define ICANON 0x0002
#define ISIG   0x0001
struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

/* process list */
#define PROC_NAME_MAX 32
struct procinfo {
    int pid, ppid;
    int state;            /* PS_* */
    uint32_t mem_kb;      /* user pages */
    uint32_t cpu_ms;      /* time spent running */
    char name[PROC_NAME_MAX];
};
enum { PS_RUNNING = 0, PS_READY, PS_BLOCKED, PS_ZOMBIE };

struct sysinfo {
    uint32_t uptime_ms;
    uint32_t mem_total_kb, mem_free_kb;
    uint32_t heap_total_kb, heap_used_kb;
    uint32_t nprocs;
    uint32_t idle_ms;     /* time spent in the idle task, for CPU % */
    uint32_t screen_w, screen_h;
};

/* ---- networking: addresses and ports are in network byte order ---- */
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define IPPROTO_ICMP 1
#define MSG_DONTWAIT 1
struct sockaddr_in { uint16_t sin_family, sin_port; uint32_t sin_addr; uint8_t sin_zero[8]; };
struct netinfo {
    uint32_t ip, mask, gw, dns;       /* 0 until DHCP finishes */
    uint8_t mac[6]; uint8_t up, present;   /* up: has a lease; present: NIC found */
    uint32_t rx_pkts, tx_pkts, rx_bytes, tx_bytes;
};

/* ---- GUI ---- */
struct win_rect { int x, y, w, h; };

#define WIN_FLAG_NORESIZE 1
#define WIN_FLAG_FRAMELESS 2   /* no title bar: panels, popups */
struct win_info {
    int x, y, w, h;           /* client area; x/y = -1 for auto placement */
    int flags;
    char title[64];
    uint32_t *pixels;         /* out: w*h 0xAARRGGBB, mapped in caller */
    int stride;               /* out: pixels per row */
};

enum { WIN_SET_TITLE = 1, WIN_SET_SIZE, WIN_SET_POS, WIN_SET_VISIBLE,
       WIN_SET_PREFS };   /* PREFS: desktop re-reads /etc/wallpaper and /etc/tz */

/* reading a window fd yields struct gui_event records */
enum {
    EV_KEY = 1,        /* key, mods, ch, pressed */
    EV_MOUSE_MOVE,     /* x, y (client coords), buttons */
    EV_MOUSE_DOWN,     /* x, y, button */
    EV_MOUSE_UP,
    EV_SCROLL,         /* x, y, dz */
    EV_RESIZE,         /* w, h: buffer was reallocated, re-read pixels via WIN_SET_SIZE result */
    EV_CLOSE,          /* close button pressed */
    EV_FOCUS,          /* pressed = gained/lost */
    EV_MOUSE_LEAVE,
};

/* key codes: printable keys use their ASCII; specials below */
enum {
    KEY_ESC = 27, KEY_BACKSPACE = 8, KEY_TAB = 9, KEY_ENTER = 10, KEY_DELETE = 127,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT, KEY_SUPER, KEY_CAPSLOCK,
    KEY_MENU, KEY_PRTSC,
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_SUPER 8

struct gui_event {
    int type;
    int x, y;
    int key;            /* KEY_* or ASCII */
    int ch;             /* translated character (0 if none), already shifted/ctrl'ed */
    int mods;
    int button;         /* 1 left, 2 right, 4 middle (bitmask in MOUSE_MOVE) */
    int pressed;
    int dz;
    int w, h;
};
