/* open [-w] FILE... - open files with their associated program (/etc/open.conf); -w only prints the program */
#include <fsutil.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    int i = 1, w = 0, rc = 0;
    if (i < argc && !strcmp(argv[i], "-w")) { w = 1; i++; }
    if (i >= argc) { fprintf(stderr, "usage: open [-w] FILE...\n"); return 2; }
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st)) { perror(argv[i]); rc = 1; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (w) { puts("/bin/files"); continue; }
            char *a[3] = { "files", argv[i], NULL };
            if (spawnv("/bin/files", a) < 0) { perror("open"); rc = 1; }
            continue;
        }
        char prog[PATH_MAX];
        int r = fs_assoc(argv[i], prog, sizeof prog);
        if (!r) { fprintf(stderr, "open: %s: no program for this file\n", argv[i]); rc = 1; }
        else if (w) puts(r == 2 ? argv[i] : prog);
        else if (fs_open(argv[i]) < 0) { fprintf(stderr, "open: %s: cannot start %s\n", argv[i], r == 2 ? "it" : prog); rc = 1; }
    }
    return rc;
}
