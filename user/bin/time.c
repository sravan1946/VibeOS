/* time CMD [ARGS] : run a command and report wall-clock time */
#include <vutil.h>
#include <syscall.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: time command [args...]\n"); return 2; }
    uint32_t t0 = uptime_ms();
    int pid = spawnvp(argv[1], argv + 1), st = 0;
    if (pid < 0) { fprintf(stderr, "time: %s: %s\n", argv[1], strerror(errno)); return 127; }
    waitpid(pid, &st, 0);
    uint32_t ms = uptime_ms() - t0;
    fprintf(stderr, "\nreal\t%u.%03us\n", ms / 1000, ms % 1000);
    return WEXITSTATUS(st);
}
