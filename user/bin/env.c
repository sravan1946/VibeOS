/* env [NAME=VALUE]... [command [args...]] : print the environment, or run command with it changed */
#include <vutil.h>

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++) {
        char *eq = strchr(argv[i], '=');
        *eq = 0;
        setenv(argv[i], eq + 1, 1);
    }
    if (i >= argc) {
        for (char **e = environ; e && *e; e++) puts(*e);
        return 0;
    }
    fflush(stdout);
    int pid = spawnvp(argv[i], argv + i);
    if (pid < 0) { fprintf(stderr, "env: '%s': %s\n", argv[i], strerror(errno)); return errno == ENOENT ? 127 : 126; }
    int st = 0;
    waitpid(pid, &st, 0);
    return st & 0xff;
}
