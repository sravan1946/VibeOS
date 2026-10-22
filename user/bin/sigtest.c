/* sigtest - checks signal delivery: handlers, masks, ignore, nesting, FPU state, EINTR, SIGCHLD */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <syscall.h>

static volatile int got[NSIG], order[8], norder;
static int pass, fail;

static void check(int ok, const char *what) {
    if (ok) pass++; else { fail++; printf("FAIL: %s\n", what); }
}

static void on(int sig) { got[sig]++; if (norder < 8) order[norder++] = sig; }
static void nest(int sig) { on(sig); raise(SIGUSR2); on(sig); }
static void fpu_mess(int sig) {                    /* clobber every x87 register the main code might hold */
    volatile double a = 0;
    for (int i = 1; i < 50; i++) a += 1.0 / i * (i % 3 ? 1.7 : -0.3);
    (void)a;
    on(sig);
}

static int run(const char *arg) {
    char *argv[] = { "/bin/sigtest", (char *)arg, 0 };
    int pid = spawnv(argv[0], argv), st = -1;
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {}   /* SIGCHLD may interrupt it */
    return WEXITSTATUS(st);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "die")) { raise(SIGUSR2); return 0; }
    if (argc > 1 && !strcmp(argv[1], "kick")) { usleep(100000); kill(getppid(), SIGUSR1); return 0; }
    if (argc > 1 && !strcmp(argv[1], "winch")) { raise(SIGWINCH); raise(SIGCHLD); return 7; }

    check(signal(SIGUSR1, on) == SIG_DFL, "first signal() returns SIG_DFL");
    raise(SIGUSR1);
    check(got[SIGUSR1] == 1, "handler runs on raise");

    sigset_t s, old;
    sigemptyset(&s); sigaddset(&s, SIGUSR1);
    sigprocmask(SIG_BLOCK, &s, &old);
    raise(SIGUSR1);
    check(got[SIGUSR1] == 1, "blocked signal stays pending");
    sigprocmask(SIG_SETMASK, &old, 0);
    check(got[SIGUSR1] == 2, "delivered once unblocked");

    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    check(got[SIGUSR1] == 2, "SIG_IGN drops it");

    signal(SIGUSR1, nest); signal(SIGUSR2, on);
    norder = 0;
    raise(SIGUSR1);
    check(norder == 3 && order[0] == SIGUSR1 && order[1] == SIGUSR2 && order[2] == SIGUSR1, "nested handler runs inside");

    signal(SIGUSR1, fpu_mess);
    volatile double x = 1.25, y = 3.5;
    double z = x * y;
    raise(SIGUSR1);
    z = z * 2 + x;
    check(z == 1.25 * 3.5 * 2 + 1.25, "FPU state survives a handler");

    signal(SIGCHLD, on);
    int before = got[SIGCHLD];
    check(run("winch") == 7, "SIGWINCH/SIGCHLD are ignored by default");
    check(got[SIGCHLD] > before, "SIGCHLD reaches the parent's handler");
    check(run("die") == 128 + SIGUSR2, "default action terminates with 128+n");

    signal(SIGUSR1, on);
    int fds[2];
    pipe(fds);
    int n0 = got[SIGUSR1];
    char *kargv[] = { "/bin/sigtest", "kick", 0 };
    int kid = spawnv(kargv[0], kargv);
    char c;
    errno = 0;
    int r = read(fds[0], &c, 1);
    check(r == -1 && errno == EINTR, "blocking read fails with EINTR");
    check(got[SIGUSR1] == n0 + 1, "handler ran during the blocked read");
    while (waitpid(kid, 0, 0) < 0 && errno == EINTR) {}

    check(signal(SIGKILL, on) == SIG_ERR && errno == EINVAL, "SIGKILL can't be caught");
    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
