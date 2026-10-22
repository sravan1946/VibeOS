/* proctest - checks exec (pid, fds, FD_CLOEXEC, argv/env, errors, signals, #!, sh's exec) and threads
 * (mutex, join values, errno, condvars, stdio/malloc from threads, exit/kill/exec with live threads).
 * The x-* arguments are the helper images it runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>
#include <syscall.h>

static int pass, fail;
static void check(int ok, const char *what) {
    if (ok) pass++; else { fail++; printf("FAIL: %s\n", what); }
}

static int wait_for(int pid) {
    int st = -1;
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return pid > 0 ? WEXITSTATUS(st) : -1;
}

static int run(char *const argv[]) { return wait_for(spawnv("/bin/proctest", argv)); }

static volatile int winches;
static void sigwinch_count(int s) { winches++; }
static volatile int got_term;
static void on_term(int s) { got_term++; }

static char sp_pid[16];
static void spin(void) { for (volatile int i = 0;; i++) if (!(i & 0xFFFF)) yield(); }
static void *spin_thread(void *a) { spin(); return 0; }
static int block_pipe[2];
static void *pipe_reader(void *a) { char c; read(block_pipe[0], &c, 1); return 0; }   /* never gets data */
static void *cond_sleeper(void *a) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&m);
    for (;;) pthread_cond_wait(&c, &m);
    return 0;
}

/* ---- helper images: argv[1] says which; exit 0 = all good, else a bit per failed check ---- */
static int helper(int argc, char **argv) {
    const char *m = argv[1];
    if (!strcmp(m, "x-pid"))                    /* exec into x-same with our pid */
        { snprintf(sp_pid, sizeof sp_pid, "%d", getpid()); execl("/bin/proctest", "proctest", "x-same", sp_pid, (char *)0); return 99; }
    if (!strcmp(m, "x-same")) return argc > 2 && getpid() == atoi(argv[2]) ? 0 : 1;
    if (!strcmp(m, "x-fds")) {                  /* A stays, B (O_CLOEXEC) and C (F_SETFD) close */
        int a = open("/dev/null", O_RDWR), b = open("/dev/null", O_RDWR | O_CLOEXEC), c = open("/dev/null", O_RDWR);
        fcntl(c, F_SETFD, FD_CLOEXEC);
        char s[48];
        snprintf(s, sizeof s, "%d %d %d %d", a, b, c, fcntl(b, F_GETFD) == FD_CLOEXEC && fcntl(a, F_GETFD) == 0);
        execl("/bin/proctest", "proctest", "x-fds2", s, (char *)0);
        return 99;
    }
    if (!strcmp(m, "x-fds2")) {
        int a, b, c, flags;
        if (argc < 3 || sscanf(argv[2], "%d %d %d %d", &a, &b, &c, &flags) != 4) return 64;
        int r = 0;
        if (!flags) r |= 1;
        if (fcntl(a, F_GETFD) != 0 || write(a, "x", 1) != 1) r |= 2;
        errno = 0;
        if (fcntl(b, F_GETFD) != -1 || errno != EBADF) r |= 4;
        if (fcntl(c, F_GETFD) != -1) r |= 8;
        if (write(1, "fds-ok", 6) != 6) r |= 16;     /* stdout (a pipe from the test) survived */
        return r;
    }
    if (!strcmp(m, "x-args")) {
        char *env[] = { "VIBE_T=42", "OTHER=a b", 0 };
        execle("/bin/proctest", "proctest", "x-args2", "one", "", "three four", (char *)0, env);
        return 99;
    }
    if (!strcmp(m, "x-args2")) {
        int r = 0;
        if (argc != 5 || strcmp(argv[2], "one") || strcmp(argv[3], "") || strcmp(argv[4], "three four")) r |= 1;
        if (!getenv("VIBE_T") || strcmp(getenv("VIBE_T"), "42") || !getenv("OTHER") || strcmp(getenv("OTHER"), "a b")) r |= 2;
        if (getenv("PATH")) r |= 4;                    /* only what execle passed */
        return r;
    }
    if (!strcmp(m, "x-sig")) {                  /* handlers, a blocked pending signal, an ignored one, SIGWINCH spam */
        signal(SIGUSR1, on_term);
        signal(SIGUSR2, SIG_IGN);
        signal(SIGWINCH, sigwinch_count);
        for (int i = 0; i < 100; i++) raise(SIGUSR1);
        sigset_t s;
        sigemptyset(&s); sigaddset(&s, SIGTERM);
        sigprocmask(SIG_BLOCK, &s, 0);
        raise(SIGTERM);
        snprintf(sp_pid, sizeof sp_pid, "%d", getpid());
        char *kick[] = { "/bin/proctest", "x-spam", sp_pid, 0 };
        spawnv(kick[0], kick);
        usleep(30000);                                 /* exec in the middle of the spam */
        execl("/bin/proctest", "proctest", "x-sig2", sp_pid, (char *)0);
        return 99;
    }
    if (!strcmp(m, "x-spam")) {                 /* SIGWINCH at argv[2] for ~150 ms */
        int pid = atoi(argv[2]);
        for (uint32_t t0 = uptime_ms(); uptime_ms() - t0 < 150; ) kill(pid, SIGWINCH);
        return 0;
    }
    if (!strcmp(m, "x-sig2")) {
        int r = 0;
        if (getpid() != atoi(argv[2])) r |= 1;
        if (signal(SIGUSR1, SIG_DFL) != SIG_DFL) r |= 2;   /* handled -> default */
        if (signal(SIGUSR2, SIG_DFL) != SIG_IGN) r |= 4;   /* ignored stays ignored */
        sigset_t cur;
        sigprocmask(SIG_BLOCK, 0, &cur);
        if (!sigismember(&cur, SIGTERM)) r |= 8;           /* mask kept */
        signal(SIGTERM, on_term);
        sigprocmask(SIG_UNBLOCK, &cur, 0);
        if (got_term != 1) r |= 16;                        /* and the pending SIGTERM with it */
        usleep(200000);                                    /* outlive the spam (SIGWINCH: default ignore) */
        return r;
    }
    if (!strcmp(m, "x-script")) return argc == 3 && !strcmp(argv[2], "/home/.proctest-script") ? 0 : 1;
    if (!strcmp(m, "x-scriptexec")) { execl("/home/.proctest-script", "ignored", (char *)0); return 99; }
    if (!strcmp(m, "x-shexec")) return argc > 2 && getpid() == atoi(argv[2]) && getppid() > 0 ? 0 : 1;

    /* threads */
    if (!strcmp(m, "x-exit")) {                 /* main exits while threads spin, sleep and block */
        pthread_t t;
        for (int i = 0; i < 2; i++) pthread_create(&t, 0, spin_thread, 0);
        pipe(block_pipe);
        pthread_create(&t, 0, pipe_reader, 0);
        pthread_create(&t, 0, cond_sleeper, 0);
        pthread_create(&t, 0, (void *(*)(void *))(void *)usleep, (void *)10000000);
        usleep(50000);
        exit(5);
    }
    if (!strcmp(m, "x-threxit")) {              /* a non-main thread calls exit() while main waits in join */
        pthread_t t, u;
        pthread_create(&u, 0, spin_thread, 0);
        pthread_create(&t, 0, (void *(*)(void *))(void *)exit, (void *)6);
        pthread_join(u, 0);
        return 1;
    }
    if (!strcmp(m, "x-spin")) {                 /* threads that never end, to be killed from outside */
        pthread_t t;
        for (int i = 0; i < 4; i++) pthread_create(&t, 0, spin_thread, 0);
        spin();
    }
    if (!strcmp(m, "x-mainexit")) {             /* pthread_exit in main: the process waits for its thread */
        pthread_t t;
        pthread_create(&t, 0, (void *(*)(void *))(void *)usleep, (void *)100000);
        pthread_exit(0);
    }
    if (!strcmp(m, "x-execthr")) {              /* exec kills the other threads */
        pthread_t t;
        for (int i = 0; i < 3; i++) pthread_create(&t, 0, spin_thread, 0);
        usleep(20000);
        snprintf(sp_pid, sizeof sp_pid, "%d", getpid());
        execl("/bin/proctest", "proctest", "x-same", sp_pid, (char *)0);
        return 99;
    }
    return 100;
}

/* ---- thread tests ---- */
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int counter;
static void *add(void *arg) {
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&mx);
        int c = counter;                       /* a read-modify-write wide open to preemption */
        for (volatile int k = 0; k < 3; k++) {}
        counter = c + 1;
        pthread_mutex_unlock(&mx);
    }
    return (void *)((intptr_t)arg * 10);
}

static volatile int early_done, others_go;
static void *early(void *a) { early_done = 1; pthread_exit((void *)77); }
static void *late(void *a) {
    while (!others_go) yield();
    return (void *)(intptr_t)(early_done ? 1 : 0);
}

static volatile int errno_bad;
static void *errno_thread(void *a) {
    int mine = (int)(intptr_t)a;
    for (int i = 0; i < 2000; i++) {
        errno = mine;
        for (volatile int k = 0; k < 200; k++) {}
        if (errno != mine) errno_bad++;
        if (!(i & 63)) yield();
    }
    return 0;
}

#define QN 8
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qne = PTHREAD_COND_INITIALIZER, qnf = PTHREAD_COND_INITIALIZER;
static int q[QN], qh, qt, qsum;
static void *producer(void *a) {
    for (int i = 1; i <= 1000; i++) {
        pthread_mutex_lock(&qm);
        while (qt - qh == QN) pthread_cond_wait(&qnf, &qm);
        q[qt++ % QN] = i;
        pthread_cond_signal(&qne);
        pthread_mutex_unlock(&qm);
    }
    return 0;
}
static void *consumer(void *a) {
    for (int n = 0; n < 500; n++) {
        pthread_mutex_lock(&qm);
        while (qt == qh) pthread_cond_wait(&qne, &qm);
        qsum += q[qh++ % QN];
        pthread_cond_signal(&qnf);
        pthread_mutex_unlock(&qm);
    }
    return 0;
}

static void *churn(void *a) {
    unsigned seed = (unsigned)(intptr_t)a;
    void *keep[32] = { 0 };
    for (int i = 0; i < 3000; i++) {
        seed = seed * 1103515245 + 12345;
        int k = seed >> 16 & 31;
        free(keep[k]);
        keep[k] = malloc(1 + (seed >> 8 & 1023));
        if (keep[k]) memset(keep[k], k, 1 + (seed >> 8 & 1023) > 16 ? 16 : 1);
    }
    for (int k = 0; k < 32; k++) free(keep[k]);
    return 0;
}

static void *printer(void *a) {
    for (int i = 0; i < 50; i++) printf("line %d from thread %d\n", i, (int)(intptr_t)a);
    fflush(stdout);
    return 0;
}

static int live_threads_of(int pid) {
    struct procinfo pi[64];
    int n = proclist(pi, 64), k = 0;
    for (int i = 0; i < n; i++) if (pi[i].pid == pid || pi[i].ppid == pid) k++;
    return k;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strncmp(argv[1], "x-", 2)) {
        if (!strcmp(argv[1], "x-print")) {      /* 4 threads printf to stdout (a pipe) */
            pthread_t t[4];
            for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, printer, (void *)(intptr_t)i);
            for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
            return 0;
        }
        return helper(argc, argv);
    }

    /* ---- exec ---- */
    char *a_pid[] = { "proctest", "x-pid", 0 };
    check(run(a_pid) == 0, "exec keeps the pid (and waitpid finds it)");

    int p[2];
    pipe(p);
    struct spawn_opts o = { { -1, p[1], -1 }, 0, 0 };
    char *a_fds[] = { "proctest", "x-fds", 0 };
    int pid = spawnve("/bin/proctest", a_fds, environ, &o);
    close(p[1]);
    char buf[64] = { 0 };
    int n = read(p[0], buf, sizeof buf - 1);
    int st = wait_for(pid);
    close(p[0]);
    check(st == 0, "fds survive exec, FD_CLOEXEC / O_CLOEXEC ones close");
    if (st) printf("  x-fds2 status %d\n", st);
    check(n == 6 && !memcmp(buf, "fds-ok", 6), "inherited stdout pipe works after exec");

    char *a_args[] = { "proctest", "x-args", 0 };
    st = run(a_args);
    check(st == 0, "argv and envp reach the new image (execle)");
    if (st) printf("  x-args2 status %d\n", st);

    signal(SIGUSR1, on_term);
    errno = 0;
    check(execl("/nonexistent/prog", "prog", (char *)0) == -1 && errno == ENOENT, "exec of a missing file: -1 ENOENT");
    errno = 0;
    check(execl("/etc/profile", "profile", (char *)0) == -1 && errno == ENOEXEC, "exec of a non-ELF: -1 ENOEXEC");
    errno = 0;
    check(execvp("no-such-command-xyz", a_pid) == -1 && errno == ENOENT, "execvp searches PATH, ENOENT");
    check(fcntl(1, F_GETFD) == 0 && signal(SIGUSR1, SIG_DFL) == on_term, "failed execs leave fds and handlers alone");

    char *a_sig[] = { "proctest", "x-sig", 0 };
    st = run(a_sig);
    check(st == 0, "exec under signals: handlers reset, ignore/mask/pending kept");
    if (st) printf("  x-sig2 status %d\n", st);

    FILE *f = fopen("/home/.proctest-script", "w");
    if (f) { fputs("#!/bin/proctest x-script\n", f); fclose(f); }
    char *a_scr[] = { "proctest", "x-scriptexec", 0 };
    check(run(a_scr) == 0, "exec runs a #! script through the kernel loader");
    unlink("/home/.proctest-script");

    char *a_sh[] = { "/bin/sh", "-c", "exec /bin/proctest x-shexec $$", 0 };
    check(wait_for(spawnv("/bin/sh", a_sh)) == 0, "sh's exec builtin keeps the shell's pid");
    char *a_sh2[] = { "/bin/sh", "-c", "exec /nonexistent 2>/dev/null; exit 3", 0 };
    check(wait_for(spawnv("/bin/sh", a_sh2)) == 3, "sh's exec of a missing command returns to the shell");

    /* ---- threads ---- */
    pthread_t t[8];
    void *rv;
    int ok = 1;
    for (int i = 0; i < 8; i++) ok &= pthread_create(&t[i], 0, add, (void *)(intptr_t)i) == 0;
    check(ok, "pthread_create x8");
    ok = 1;
    for (int i = 0; i < 8; i++) ok &= pthread_join(t[i], &rv) == 0 && (intptr_t)rv == i * 10;
    check(ok, "pthread_join returns each thread's value");
    check(counter == 8 * 20000, "8 threads x 20000 increments under a mutex");
    if (counter != 8 * 20000) printf("  counter %d\n", counter);
    check(pthread_join(t[0], 0) == ESRCH, "joining twice: ESRCH");
    check(pthread_self() == getpid(), "main thread's id is the pid");

    pthread_t e, l[3];
    pthread_create(&e, 0, early, 0);
    for (int i = 0; i < 3; i++) pthread_create(&l[i], 0, late, 0);
    while (!early_done) yield();
    check(pthread_join(e, &rv) == 0 && (intptr_t)rv == 77, "a thread exits early (pthread_exit value)");
    others_go = 1;
    ok = 1;
    for (int i = 0; i < 3; i++) ok &= pthread_join(l[i], &rv) == 0 && (intptr_t)rv == 1;
    check(ok, "the others keep running after it");

    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, errno_thread, (void *)(intptr_t)(100 + i));
    errno = 7;
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    check(!errno_bad && errno == 7, "errno is per thread");

    pthread_create(&t[0], 0, producer, 0);
    pthread_create(&t[1], 0, consumer, 0);
    pthread_create(&t[2], 0, consumer, 0);
    for (int i = 0; i < 3; i++) pthread_join(t[i], 0);
    check(qsum == 1000 * 1001 / 2, "condition variables: bounded queue, 1 producer 2 consumers");

    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, churn, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    void *big = malloc(100000);
    check(big != 0, "malloc from 4 threads at once (heap intact)");
    free(big);

    pipe(p);
    struct spawn_opts po = { { -1, p[1], -1 }, 0, 0 };
    char *a_pr[] = { "proctest", "x-print", 0 };
    pid = spawnve("/bin/proctest", a_pr, environ, &po);
    close(p[1]);
    int lines = 0, bad = 0, r;
    char lb[128]; int ll = 0;
    while ((r = read(p[0], buf, sizeof buf)) > 0)
        for (int i = 0; i < r; i++) {
            if (buf[i] != '\n') { if (ll < 127) lb[ll++] = buf[i]; continue; }
            lb[ll] = 0; ll = 0; lines++;
            int a, b;
            if (sscanf(lb, "line %d from thread %d", &a, &b) != 2) bad++;
        }
    close(p[0]);
    check(wait_for(pid) == 0 && lines == 200 && !bad, "printf from 4 threads: 200 whole lines");

    char *a_ex[] = { "proctest", "x-exit", 0 };
    pid = spawnv("/bin/proctest", a_ex);
    check(wait_for(pid) == 5, "exit() from main with live threads: status kept");
    usleep(20000);
    check(live_threads_of(pid) == 0, "...and every thread is gone");
    char *a_te[] = { "proctest", "x-threxit", 0 };
    check(run(a_te) == 6, "exit() from a non-main thread ends the process");
    char *a_sp[] = { "proctest", "x-spin", 0 };
    pid = spawnv("/bin/proctest", a_sp);
    usleep(50000);
    check(live_threads_of(pid) == 5, "a 5-thread process shows 5 tasks");
    kill(pid, SIGTERM);
    check(wait_for(pid) == 128 + SIGTERM, "kill() of a threaded process");
    usleep(20000);
    check(live_threads_of(pid) == 0, "...and its threads are gone");
    char *a_me[] = { "proctest", "x-mainexit", 0 };
    uint32_t t0 = uptime_ms();
    check(run(a_me) == 0 && uptime_ms() - t0 >= 90, "pthread_exit in main waits for the last thread, exits 0");
    char *a_et[] = { "proctest", "x-execthr", 0 };
    check(run(a_et) == 0, "exec from a threaded process");

    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
