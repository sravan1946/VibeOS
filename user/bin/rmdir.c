/* rmdir dir... */
#include <vutil.h>

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "rmdir: missing operand\n"); return 1; }
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) { fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}
