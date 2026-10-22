/* posix.c - things built from the raw wrappers: spawn helpers, wait, isatty, termios, dirent */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <limits.h>
#include <syscall.h>
#include <stdarg.h>

int spawnv(const char *path, char *const argv[]) { return spawnve(path, argv, environ, NULL); }

/* run(path, argv) for file in each $PATH entry (default "/bin") until one doesn't fail with ENOENT/ENOTDIR */
static int path_search(const char *file, char *const argv[], int (*run)(const char *, char *const[])) {
    if (strchr(file, '/')) return run(file, argv);
    const char *p = getenv("PATH");
    if (!p || !*p) p = "/bin";
    size_t fl = strlen(file);
    int err = ENOENT;
    while (*p) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        char path[PATH_MAX];
        if (dl + fl + 2 <= sizeof path) {
            memcpy(path, p, dl);
            path[dl] = '/';
            memcpy(path + dl + 1, file, fl + 1);
            int r = run(dl ? path : path + 1, argv);            /* empty entry = cwd */
            if (r >= 0) return r;
            if (errno != ENOENT && errno != ENOTDIR) err = errno;
        }
        if (!e) break;
        p = e + 1;
    }
    errno = err;
    return -1;
}

int spawnvp(const char *file, char *const argv[]) { return path_search(file, argv, spawnv); }

int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }
int execvp(const char *file, char *const argv[]) { return path_search(file, argv, execv); }

/* execl & co: the argument list (up to the NULL) becomes argv; execle's envp follows the NULL */
#define LIST_ARGV(av, arg0, ap)                                              \
    int n_ = 1;                                                              \
    va_start(ap, arg0);                                                      \
    while (va_arg(ap, char *)) n_++;                                         \
    va_end(ap);                                                              \
    char *av[n_ + 1];                                                        \
    av[0] = (char *)arg0;                                                    \
    va_start(ap, arg0);                                                      \
    for (int i_ = 1; i_ <= n_; i_++) av[i_] = va_arg(ap, char *)

int execl(const char *path, const char *arg0, ...) {
    va_list ap;
    LIST_ARGV(av, arg0, ap);
    va_end(ap);
    return execv(path, av);
}
int execlp(const char *file, const char *arg0, ...) {
    va_list ap;
    LIST_ARGV(av, arg0, ap);
    va_end(ap);
    return execvp(file, av);
}
int execle(const char *path, const char *arg0, ...) {
    va_list ap;
    LIST_ARGV(av, arg0, ap);
    char **envp = va_arg(ap, char **);
    va_end(ap);
    return execve(path, av, envp);
}

pid_t wait(int *status) { return waitpid(-1, status, 0); }

int isatty(int fd) {
    struct termios t;
    return ioctl(fd, TCGETS, &t) == 0;
}

int tcgetattr(int fd, struct termios *t) { return ioctl(fd, TCGETS, t); }
int tcsetattr(int fd, int when, const struct termios *t) { return ioctl(fd, TCSETS, (void *)t); }
void cfmakeraw(struct termios *t) { t->c_lflag &= ~(uint32_t)(ECHO | ICANON | ISIG); }

struct DIR {
    int fd;
    struct dirent ent;
};

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof *d);
    if (!d) { close(fd); return NULL; }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d) {
    return __readdir(d->fd, &d->ent) == 1 ? &d->ent : NULL;
}

void rewinddir(DIR *d) { lseek(d->fd, 0, SEEK_SET); }

int closedir(DIR *d) {
    int r = close(d->fd);
    free(d);
    return r;
}
