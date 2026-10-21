/* stat file... */
#include <vutil.h>
#include <time.h>

static void when(const char *label, uint32_t t) {
    time_t tt = t;
    struct tm tm;
    char b[48];
    localtime_r(&tt, &tm);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %z", &tm);
    printf("%s: %s\n", label, b);
}

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "stat: missing operand\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) { fprintf(stderr, "stat: cannot stat '%s': %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        unsigned m = st.st_mode;
        const char *type = S_ISDIR(m) ? "directory" : S_ISCHR(m) ? "character special file" : S_ISFIFO(m) ? "fifo" : "regular file";
        char perm[11] = "----------";
        perm[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISFIFO(m) ? 'p' : '-';
        for (int k = 0; k < 9; k++) if (m & (0400 >> k)) perm[k + 1] = "rwx"[k % 3];
        printf("  File: %s\n  Size: %-10u Blocks: %-8u %s\nInode: %-10u Links: %u\nAccess: (%04o/%s)\n",
               argv[i], (unsigned)st.st_size, (unsigned)st.st_blocks, type, (unsigned)st.st_ino, (unsigned)st.st_nlink, m & 07777, perm);
        when("Access", st.st_atime);
        when("Modify", st.st_mtime);
        when("Change", st.st_ctime);
    }
    return rc;
}
