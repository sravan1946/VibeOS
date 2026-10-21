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

int spawnv(const char *path, char *const argv[]) { return spawnve(path, argv, environ, NULL); }

int spawnvp(const char *file, char *const argv[]) {
    if (strchr(file, '/')) return spawnv(file, argv);
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
            int pid = spawnv(dl ? path : path + 1, argv);       /* empty entry = cwd */
            if (pid >= 0) return pid;
            if (errno != ENOENT && errno != ENOTDIR) err = errno;
        }
        if (!e) break;
        p = e + 1;
    }
    errno = err;
    return -1;
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
    int fd = open(path, O_RDONLY | O_DIRECTORY);
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
