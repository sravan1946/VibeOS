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
    SYS_MAX
};

/* errno */
enum {
    EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, ENXIO = 6, E2BIG = 7, ENOEXEC = 8,
    EBADF = 9, ECHILD = 10, EAGAIN = 11, ENOMEM = 12, EACCES = 13, EFAULT = 14, EBUSY = 16,
    EEXIST = 17, EXDEV = 18, ENODEV = 19, ENOTDIR = 20, EISDIR = 21, EINVAL = 22, ENFILE = 23,
    EMFILE = 24, ENOTTY = 25, EFBIG = 27, ENOSPC = 28, ESPIPE = 29, EROFS = 30, EPIPE = 32,
    ERANGE = 34, ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39,
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

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define WNOHANG 1

/* file types (st_mode high bits, ext2-compatible) */
#define S_IFMT   0xF000
#define S_IFIFO  0x1000
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

/* signals (all fatal for now except SIGCHLD) */
#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15

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

enum { WIN_SET_TITLE = 1, WIN_SET_SIZE, WIN_SET_POS, WIN_SET_VISIBLE };

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
    KEY_MENU,
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
