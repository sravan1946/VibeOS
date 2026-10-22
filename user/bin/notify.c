/* notify [-k good|warn|bad] TITLE [BODY] : show a desktop notification (writes /dev/notify) */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *kind = "";
    if (argc > 2 && !strcmp(argv[1], "-k")) { kind = argv[2]; argc -= 2; argv += 2; }
    if (argc < 2) { fprintf(stderr, "usage: notify [-k good|warn|bad] TITLE [BODY]\n"); return 2; }
    char m[256];
    int n = snprintf(m, sizeof m, "%s\n%s\n%s", argv[1], argc > 2 ? argv[2] : "", kind);
    if (n >= (int)sizeof m) n = sizeof m - 1;
    int fd = open("/dev/notify", O_WRONLY);
    if (fd < 0 || write(fd, m, n) != n) { fprintf(stderr, "notify: no /dev/notify\n"); return 1; }
    close(fd);
    return 0;
}
