/* display - list or switch screen modes through /dev/display (a tiny xrandr).
 *   display            list the modes, * = current
 *   display WxH [-s]   switch now; -s also saves it to /etc/display for the next boot */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int put(const char *path, const char *s, int flags) {
    int fd = open(path, O_WRONLY | flags, 0644);
    if (fd < 0) return -1;
    int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
    close(fd);
    return r;
}

int main(int argc, char **argv) {
    const char *mode = 0;
    int save = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--save")) save = 1;
        else if (argv[i][0] != '-' && !mode) mode = argv[i];
        else { fprintf(stderr, "usage: display [WxH [-s]]\n"); return 2; }
    }
    if (!mode) {
        char buf[512];
        int fd = open("/dev/display", O_RDONLY), n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        if (n <= 0) { perror("display: /dev/display"); return 1; }
        buf[n] = 0;
        for (char *l = strtok(buf, "\n"); l; l = strtok(0, "\n")) {
            int cur = strchr(l, '*') != 0;
            if (cur) l[strcspn(l, " ")] = 0;
            printf("%s %s\n", cur ? "*" : " ", l);
        }
        return 0;
    }
    char m[32];
    snprintf(m, sizeof m, "%s\n", mode);
    if (put("/dev/display", m, 0) < 0) {
        fprintf(stderr, "display: can't set %s: %s\n", mode, errno == EINVAL ? "unsupported mode" : strerror(errno));
        return 1;
    }
    if (save && put("/etc/display", m, O_CREAT | O_TRUNC) < 0) { perror("display: /etc/display"); return 1; }
    return 0;
}
