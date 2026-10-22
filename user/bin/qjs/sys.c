/* sys.c - the qjs_* functions declared in compat/vibe_qjs.h: POSIX calls quickjs-libc (the std and os modules) makes
 * that our libc doesn't have in that shape. stat and termios convert to and from the kernel's structs; popen runs
 * `sh -c` with a pipe; the rest (fork, exec, links, utimes, uid changes) fail with ENOSYS. */
#include "compat/vibe_qjs.h"
#include <string.h>
#undef stat
#undef fstat
#undef termios
#undef tcgetattr
#undef tcsetattr

static void conv(const struct stat *k, struct qjs_stat *o) {
    memset(o, 0, sizeof *o);
    o->st_ino = k->st_ino; o->st_mode = k->st_mode; o->st_nlink = k->st_nlink;
    o->st_size = k->st_size; o->st_blocks = k->st_blocks;
    o->st_atim.tv_sec = k->st_atime; o->st_mtim.tv_sec = k->st_mtime; o->st_ctim.tv_sec = k->st_ctime;
}

int qjs_stat(const char *path, struct qjs_stat *st) {
    struct stat k;
    if (stat(path, &k) < 0) return -1;
    conv(&k, st);
    return 0;
}

int qjs_fstat(int fd, struct qjs_stat *st) {
    struct stat k;
    if (fstat(fd, &k) < 0) return -1;
    conv(&k, st);
    return 0;
}

int qjs_tcgetattr(int fd, struct qjs_termios *t) {
    struct termios k;
    if (tcgetattr(fd, &k) < 0) return -1;
    memset(t, 0, sizeof *t);
    t->c_lflag = k.c_lflag;
    t->c_iflag = ICRNL | IXON; t->c_oflag = OPOST; t->c_cflag = CS8;
    return 0;
}

int qjs_tcsetattr(int fd, int when, const struct qjs_termios *t) {
    struct termios k = { t->c_lflag & (ECHO | ICANON | ISIG) };
    return tcsetattr(fd, when, &k);
}

/* ---------- popen: sh -c CMD with its stdin or stdout on a pipe ---------- */
static struct { FILE *f; int pid; } po[8];

FILE *qjs_popen(const char *cmd, const char *mode) {
    int p[2], rd = mode[0] == 'r', slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (!po[i].f) slot = i;
    if (slot < 0) { errno = EMFILE; return NULL; }
    if (pipe(p) < 0) return NULL;
    char *argv[] = { "sh", "-c", (char *)cmd, 0 };
    struct spawn_opts o = { { rd ? -1 : p[0], rd ? p[1] : -1, -1 }, 0, 0 };
    int pid = spawnve("/bin/sh", argv, environ, &o);
    close(rd ? p[1] : p[0]);
    if (pid < 0) { close(rd ? p[0] : p[1]); return NULL; }
    FILE *f = fdopen(rd ? p[0] : p[1], mode);
    if (!f) { close(rd ? p[0] : p[1]); return NULL; }
    po[slot].f = f; po[slot].pid = pid;
    return f;
}

int qjs_pclose(FILE *f) {
    for (int i = 0; i < 8; i++) {
        if (po[i].f != f) continue;
        int st = 0, pid = po[i].pid;
        po[i].f = 0;
        fclose(f);
        return waitpid(pid, &st, 0) < 0 ? -1 : st;
    }
    errno = EINVAL;
    return -1;
}

/* ---------- not in VibeOS ---------- */
static int nosys(void) { errno = ENOSYS; return -1; }
pid_t qjs_fork(void) { return nosys(); }
int qjs_execve(const char *path, char *const argv[], char *const envp[]) { return nosys(); }
int qjs_setuid(int id) { return nosys(); }
int qjs_setgid(int id) { return nosys(); }
int qjs_symlink(const char *a, const char *b) { return nosys(); }
ssize_t qjs_readlink(const char *p, char *buf, size_t n) { errno = EINVAL; return -1; }   /* nothing is a link */
int qjs_utimes(const char *p, const struct timeval tv[2]) { return nosys(); }
long qjs_sysconf(int name) { return name == _SC_OPEN_MAX ? 64 : -1; }
