/* touch file... : create if missing; otherwise bump mtime by rewriting the size (no utimes syscall) */
#include <vutil.h>

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "touch: missing file operand\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        int fd;
        if (stat(argv[i], &st) == 0) {
            if (S_ISDIR(st.st_mode) || S_ISCHR(st.st_mode)) continue;
            fd = open(argv[i], O_WRONLY);
            if (fd >= 0) ftruncate(fd, st.st_size);
        } else fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { fprintf(stderr, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
        else close(fd);
    }
    return rc;
}
