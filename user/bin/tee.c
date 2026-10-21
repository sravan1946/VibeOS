/* tee [-a] file... : copy stdin to stdout and every file */
#include <vutil.h>

int main(int argc, char **argv) {
    int i = 1, app = 0, rc = 0, nf = 0, fds[32];
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-a")) app = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "usage: tee [-a] file...\n"); return 2; }
    }
    for (; i < argc && nf < 32; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { WARN("tee", argv[i]); rc = 1; }
        else fds[nf++] = fd;
    }
    char buf[2048];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        write_all(1, buf, n);
        for (int k = 0; k < nf; k++) write_all(fds[k], buf, n);
    }
    for (int k = 0; k < nf; k++) close(fds[k]);
    return rc;
}
