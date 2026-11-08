/* sysbench - kernel/libc microbenchmarks and real-workload timings.
 *   sysbench [-k] [-w] [name...]  names: sys clock spawn pingpong pipe fsmall f4m dir fault str malloc stdio kmsg
 *                           -w: also the workloads (make, tcc, python, qjs, lua, sh); with -w only: just those
 * Run in the VM:  sysbench -k   (or > /dev/kmsg; lines land on the serial log; BENCH-DONE ends the run).
 * Times come from rdtsc, calibrated against the 1 kHz uptime; "cold" = after reading ~20 MB of /bin
 * through the block cache. Files go in /tmp/bench.* and are removed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <syscall.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>

typedef unsigned long long u64;
static u64 khz;                         /* TSC kHz */
static inline u64 tsc(void) { unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static u64 us_of(u64 c) { return c * 1000 / khz; }
static u64 ns_of(u64 c) { return c * 1000000 / khz; }
static u64 kbps(u64 bytes, u64 c) { return c ? bytes * khz * 1000 / c / 1024 : 0; }   /* KB/s */

static void rep(const char *name, const char *fmt, u64 v, const char *unit) {
    char b[64];
    snprintf(b, sizeof b, fmt, (unsigned long)v);
    printf("%-26s %10s %s\n", name, b, unit);
}
#define NS(name, c, n) rep(name, "%lu", ns_of((c) / (n)), "ns/op")
#define US(name, c, n) rep(name, "%lu", us_of((c) / (n)), "us/op")
#define KBS(name, bytes, c) rep(name, "%lu", kbps(bytes, c), "KB/s")

static void calibrate(void) {
    u64 t0 = tsc(); uint32_t m0 = uptime_ms();
    while (uptime_ms() < m0 + 1) {}
    m0 = uptime_ms(); t0 = tsc();
    while (uptime_ms() < m0 + 300) {}
    khz = (tsc() - t0) / (uptime_ms() - m0);
    printf("tsc %lu kHz\n", (unsigned long)khz);
}

static int wait_pid(int pid) { int st = 0; while (waitpid(pid, &st, 0) < 0) {} return st; }

/* ---- syscalls, processes ---- */
static void b_sys(void) {
    enum { N = 200000 };
    u64 t = tsc();
    for (int i = 0; i < N; i++) getpid();
    NS("null syscall (getpid)", tsc() - t, N);
    struct stat st; t = tsc();
    for (int i = 0; i < 5000; i++) stat("/bin/true", &st);
    US("stat /bin/true", tsc() - t, 5000);
    t = tsc();
    for (int i = 0; i < 5000; i++) { int fd = open("/bin/true", O_RDONLY); close(fd); }
    US("open+close /bin/true", tsc() - t, 5000);
}

static u64 mono_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }

static void b_clock(void) {         /* monotonic, finer than a tick, in step with uptime and the TSC */
    enum { N = 100000 };
    u64 t = tsc(), prev = mono_us(), back = 0, distinct = 0, minstep = ~0ull;
    for (int i = 0; i < N; i++) {
        u64 v = mono_us();
        if (v < prev) back++;
        else if (v > prev) { distinct++; if (v - prev < minstep) minstep = v - prev; }
        prev = v;
    }
    NS("clock_gettime (syscall)", tsc() - t, N);
    rep("  went backwards", "%lu", back, "times");
    rep("  distinct values", "%lu", distinct, "of 100000 calls");
    rep("  smallest step", "%lu", minstep, "us");
    u64 m0 = mono_us(), c0 = tsc(); uint32_t u0 = uptime_ms();
    while (uptime_ms() < u0 + 500) {}
    u64 dm = mono_us() - m0, dc = tsc() - c0;
    rep("  500 ms of uptime =", "%lu", dm, "us on the clock");
    rep("  same span by TSC =", "%lu", dc * 1000 / khz, "us");
    struct timeval tv; gettimeofday(&tv, 0);
    rep("  gettimeofday vs time()", "%lu", (unsigned long)(tv.tv_sec - time(0) + 100), "(100 = same second)");
}

/* ping-pong again with 8 processes asleep in poll() on idle pipes: every wakeup() used to wake all of them */
static void b_pingpong(void);
static void b_pollnoise(void) {
    int w[8], pid[8];
    for (int i = 0; i < 8; i++) {
        int p[2]; pipe(p);
        struct spawn_opts o = { { p[0], 1, 2 }, 0, 0 };
        char *av[] = { "sysbench", "--poller", 0 };
        pid[i] = spawnve("/bin/sysbench", av, 0, &o);
        close(p[0]); w[i] = p[1];
    }
    b_pingpong();
    for (int i = 0; i < 8; i++) { close(w[i]); wait_pid(pid[i]); }
}

static void b_spawn(void) {
    char *av[] = { "true", 0 };
    enum { N = 60 };
    u64 t = tsc();
    for (int i = 0; i < N; i++) wait_pid(spawnv("/bin/true", av));
    rep("spawn+wait /bin/true", "%lu", us_of((tsc() - t) / N), "us");
}

static void b_pingpong(void) {      /* parent <-> cat through two pipes: every round trip is two process switches */
    int a[2], b[2];
    pipe(a); pipe(b);
    struct spawn_opts o = { { a[0], b[1], 2 }, 0, 0 };
    char *av[] = { "cat", 0 };
    int pid = spawnve("/bin/cat", av, 0, &o);
    close(a[0]); close(b[1]);
    char c = 'x';
    enum { N = 2000 };
    u64 d = ~0ull;                                          /* best of 7 rounds: the host adds noise */
    for (int i = 0; i < 50; i++) { write(a[1], &c, 1); read(b[0], &c, 1); }
    for (int k = 0; k < 7; k++) {
        u64 t = tsc();
        for (int i = 0; i < N; i++) { write(a[1], &c, 1); read(b[0], &c, 1); }
        if (tsc() - t < d) d = tsc() - t;
    }
    close(a[1]); close(b[0]);
    wait_pid(pid);
    NS("pipe ping-pong roundtrip", d, N);
    NS("  = per context switch", d, N * 2);
}

static int pfd[2];
static void *writer(void *a) {
    static char buf[4096];
    int total = (int)(intptr_t)a;
    for (int i = 0; i < total; i += sizeof buf) write(pfd[1], buf, sizeof buf);
    close(pfd[1]);
    return 0;
}
static void b_pipe(void) {
    static char buf[4096];
    int total = 4 << 20;
    pipe(pfd);
    pthread_t th;
    u64 t = tsc();
    pthread_create(&th, 0, writer, (void *)(intptr_t)total);
    long got = 0, r;
    while ((r = read(pfd[0], buf, sizeof buf)) > 0) got += r;
    u64 d = tsc() - t;
    pthread_join(th, 0);
    close(pfd[0]);
    KBS("pipe throughput 4K chunks", got, d);
}

/* ---- files ---- */
static void thrash(void) {          /* push everything cached out: read ~20 MB of /bin */
    static char buf[65536];
    DIR *d = opendir("/bin");
    struct dirent *e;
    long total = 0;
    while (total < (20 << 20) && (e = readdir(d))) {
        char p[64];
        snprintf(p, sizeof p, "/bin/%s", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        long r;
        while ((r = read(fd, buf, sizeof buf)) > 0) total += r;
        close(fd);
    }
    closedir(d);
}

static void b_fsmall(void) {
    enum { N = 200 };
    char p[64], buf[1024];
    memset(buf, 'a', sizeof buf);
    mkdir("/tmp/bench.s", 0755);
    u64 t = tsc();
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "/tmp/bench.s/f%d", i);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
        write(fd, buf, sizeof buf);
        close(fd);
    }
    US("create+write 1K file", tsc() - t, N);
    t = tsc();
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "/tmp/bench.s/f%d", i);
        int fd = open(p, O_RDONLY);
        read(fd, buf, sizeof buf);
        close(fd);
    }
    US("open+read 1K file (warm)", tsc() - t, N);
    thrash();
    t = tsc();
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "/tmp/bench.s/f%d", i);
        int fd = open(p, O_RDONLY);
        read(fd, buf, sizeof buf);
        close(fd);
    }
    US("open+read 1K file (cold)", tsc() - t, N);
    t = tsc();
    for (int i = 0; i < N; i++) { snprintf(p, sizeof p, "/tmp/bench.s/f%d", i); unlink(p); }
    US("unlink 1K file", tsc() - t, N);
    rmdir("/tmp/bench.s");
}

static void b_f4m(void) {
    enum { SZ = 4 << 20 };
    static char buf[65536];
    memset(buf, 'b', sizeof buf);
    for (int ch = 4096; ch <= 65536; ch *= 16) {
        char nm[48];
        snprintf(nm, sizeof nm, "write 4MB (%dK chunks)", ch >> 10);
        u64 t = tsc();
        int fd = open("/tmp/bench.4m", O_WRONLY | O_CREAT | O_TRUNC);
        for (int i = 0; i < SZ; i += ch) write(fd, buf, ch);
        close(fd);
        KBS(nm, SZ, tsc() - t);
    }
    u64 t = tsc();
    int fd = open("/tmp/bench.4m", O_RDONLY);
    while (read(fd, buf, sizeof buf) > 0) {}
    close(fd);
    KBS("read 4MB (warm, 64K)", SZ, tsc() - t);
    thrash();
    t = tsc();
    fd = open("/tmp/bench.4m", O_RDONLY);
    while (read(fd, buf, sizeof buf) > 0) {}
    close(fd);
    KBS("read 4MB (cold, 64K)", SZ, tsc() - t);
    thrash();
    t = tsc();
    fd = open("/tmp/bench.4m", O_RDONLY);
    while (read(fd, buf, 4096) > 0) {}
    close(fd);
    KBS("read 4MB (cold, 4K)", SZ, tsc() - t);
    t = tsc();
    unlink("/tmp/bench.4m");
    rep("unlink 4MB", "%lu", us_of(tsc() - t), "us");
}

static void b_dir(void) {
    enum { N = 1000 };
    char p[64];
    mkdir("/tmp/bench.d", 0755);
    u64 t = tsc();
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "/tmp/bench.d/file-with-a-longish-name-%d", i);
        close(open(p, O_WRONLY | O_CREAT));
    }
    US("create empty file in big dir", tsc() - t, N);
    t = tsc();
    DIR *d = opendir("/tmp/bench.d");
    while (readdir(d)) {}
    closedir(d);
    rep("readdir 1000 files", "%lu", us_of(tsc() - t), "us");
    t = tsc();
    struct stat st;
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "/tmp/bench.d/file-with-a-longish-name-%d", i);
        stat(p, &st);
    }
    US("stat in big dir", tsc() - t, N);
    t = tsc();
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "/tmp/bench.d/file-with-a-longish-name-%d", i);
        unlink(p);
    }
    US("unlink in big dir", tsc() - t, N);
    rmdir("/tmp/bench.d");
}

/* ---- memory ---- */
static void b_fault(void) {
    enum { PAGES = 1024 };
    u64 t = tsc();
    char *m = mmap(0, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < PAGES; i++) m[i * 4096] = 1;
    NS("page fault (anon mmap)", tsc() - t, PAGES);
    t = tsc();
    munmap(m, PAGES * 4096);
    NS("munmap per page", tsc() - t, PAGES);
    t = tsc();                      /* same frames again: the host has them backed already, so this is the guest's own cost */
    m = mmap(0, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < PAGES; i++) m[i * 4096] = 1;
    NS("page fault (reused frames)", tsc() - t, PAGES);
    munmap(m, PAGES * 4096);
    static char big[4 << 20];
    t = tsc();
    for (int i = 0; i < 1024; i++) *(volatile char *)&big[i * 4096] = 1;
    NS("page fault (bss)", tsc() - t, 1024);
}

static volatile size_t sink;
static void b_str(void) {
    enum { SZ = 65536 };
    char *a = malloc(SZ + 64), *b = malloc(SZ + 64);
    memset(a, 'x', SZ + 64); a[SZ + 63] = 0;
    for (int al = 0; al <= 1; al++) {
        char nm[48];
        u64 t = tsc();
        for (int i = 0; i < 400; i++) memcpy(b + al, a + 1 - al, SZ);
        snprintf(nm, sizeof nm, "memcpy 64K %s", al ? "unaligned" : "aligned");
        KBS(nm, 400ull * SZ, tsc() - t);
    }
    u64 t = tsc();
    for (int i = 0; i < 400; i++) memset(b, i, SZ);
    KBS("memset 64K", 400ull * SZ, tsc() - t);
    t = tsc();
    for (int i = 0; i < 400; i++) memmove(b + 3, b, SZ);
    KBS("memmove overlap 64K", 400ull * SZ, tsc() - t);
    t = tsc();
    for (int i = 0; i < 400; i++) sink += strlen(a);
    KBS("strlen 64K", 400ull * SZ, tsc() - t);
    t = tsc();
    for (int i = 0; i < 100; i++) sink += memcmp(a, b, SZ) + (strcmp(a, b) != 0);
    KBS("memcmp+strcmp 64K", 100ull * SZ * 2, tsc() - t);
    {   /* short ones: the common case in strings and structs (fixed-size loops, noinline through volatile pointers) */
        void *(*volatile cpy)(void *, const void *, size_t) = memcpy;
        void *(*volatile set)(void *, int, size_t) = memset;
        size_t (*volatile len)(const char *) = strlen;
        char s20[32] = "twenty chars of text";
        for (int k = 0; k < 3; k++) {
            static const int sz[3] = { 8, 24, 100 };
            char nm[40];
            t = tsc();
            for (int i = 0; i < 200000; i++) cpy(b, a, sz[k]);
            snprintf(nm, sizeof nm, "memcpy %d B", sz[k]);
            NS(nm, tsc() - t, 200000);
        }
        t = tsc();
        for (int i = 0; i < 200000; i++) set(b, 0, 24);
        NS("memset 24 B", tsc() - t, 200000);
        t = tsc();
        for (int i = 0; i < 200000; i++) sink += len(s20);
        NS("strlen 20 chars", tsc() - t, 200000);
    }
    /* kernel side: read(/dev/zero) is the kernel memset, a warm file read is the cache memcpy */
    static char buf[65536];
    int fd = open("/dev/zero", O_RDONLY);
    t = tsc();
    for (int i = 0; i < 400; i++) read(fd, buf, sizeof buf);
    close(fd);
    KBS("kernel memset (/dev/zero)", 400ull * 65536, tsc() - t);
    free(a); free(b);
}

static void b_malloc(void) {
    enum { N = 200000 };
    static void *p[1000];
    u64 t = tsc();
    for (int i = 0; i < N; i++) free(malloc(24));
    NS("malloc+free 24B", tsc() - t, N);
    t = tsc();
    for (int r = 0; r < N / 1000; r++) {
        for (int i = 0; i < 1000; i++) p[i] = malloc(16 + (i * 37) % 400);
        for (int i = 0; i < 1000; i++) free(p[i]);
    }
    NS("malloc+free 1000 mixed", tsc() - t, N);
    t = tsc();
    for (int r = 0; r < N / 1000; r++) {
        for (int i = 0; i < 1000; i++) p[i] = malloc(16 + (i * 37) % 400);
        for (int i = 0; i < 1000; i += 2) free(p[i]);
        for (int i = 0; i < 1000; i += 2) p[i] = malloc(16 + (i * 53) % 700);
        for (int i = 0; i < 1000; i++) free(p[i]);
    }
    NS("malloc fragmented", tsc() - t, N * 3 / 2);
    t = tsc();
    for (int i = 0; i < 400; i++) free(malloc(64 << 10));
    NS("malloc+free 64K", tsc() - t, 400);
    t = tsc();
    for (int i = 0; i < 200; i++) { char *q = malloc(1 << 20); q[0] = 1; free(q); }
    NS("malloc+free 1M (mmap)", tsc() - t, 200);
}

static void b_stdio(void) {
    enum { N = 100000 };
    u64 t = tsc();
    FILE *f = fopen("/tmp/bench.io", "w");
    for (int i = 0; i < N; i++) fputs("a line of text for the benchmark, 48 bytes!\n", f);
    fclose(f);
    KBS("fputs lines to file", N * 45ull, tsc() - t);
    t = tsc();
    f = fopen("/tmp/bench.io", "r");
    char ln[128];
    while (fgets(ln, sizeof ln, f)) {}
    fclose(f);
    KBS("fgets lines from file", N * 45ull, tsc() - t);
    t = tsc();
    f = fopen("/tmp/bench.io", "r");
    long c, k = 0;
    while ((c = fgetc(f)) != EOF) k += c;
    fclose(f);
    sink += k;
    KBS("fgetc bytes from file", N * 45ull, tsc() - t);
    t = tsc();
    f = fopen("/tmp/bench.io", "w");
    for (int i = 0; i < 20000; i++) fprintf(f, "%d: %s %5.1f\n", i, "text", i * 0.5);
    fclose(f);
    US("fprintf line", tsc() - t, 20000);
    unlink("/tmp/bench.io");
}

static void b_kmsg(void) {          /* the serial log: what tests and [proc] lines go through */
    int fd = open("/dev/kmsg", O_WRONLY);
    char line[64];
    memset(line, 'k', sizeof line);
    u64 t = tsc();
    for (int i = 0; i < 200; i++) { line[0] = 'B'; line[62] = '\n'; write(fd, line, 63); }
    u64 d = tsc() - t;
    close(fd);
    NS("kmsg write 63 B", d, 200);
    KBS("kmsg throughput", 200 * 63ull, d);
}

/* ---- real workloads (wall ms through the 1 kHz uptime) ---- */
static void work(const char *name, const char *cmd) {
    char *av[] = { "sh", "-c", (char *)cmd, 0 };
    int nul = open("/dev/null", O_RDWR);
    struct spawn_opts o = { { nul, nul, nul }, 0, 0 };
    uint32_t t = uptime_ms();
    int st = wait_pid(spawnve("/bin/sh", av, 0, &o));
    uint32_t d = uptime_ms() - t;
    close(nul);
    printf("%-26s %10u ms%s\n", name, d, st ? "  (exit != 0)" : "");
}
static void b_work(void) {
    work("sh -c true", "true");
    work("python -c pass", "python -c pass");
    work("qjs -e 0", "qjs -e 0");
    work("lua -e ''", "lua -e ''");
    work("tcc hello.c", "tcc -o /tmp/bench.hello /home/projects/hello.c && rm /tmp/bench.hello");
    work("make wordfreq", "rm -rf /tmp/bench.wf && cp -r /home/projects/wordfreq /tmp/bench.wf && cd /tmp/bench.wf && make && cd / && rm -rf /tmp/bench.wf");
    work("sh shtest.sh", "sh /home/shtest.sh");
}

int main(int argc, char **argv) {
    static const struct { const char *n; void (*f)(void); } T[] = {
        { "sys", b_sys }, { "clock", b_clock }, { "spawn", b_spawn }, { "pingpong", b_pingpong }, { "pollnoise", b_pollnoise }, { "pipe", b_pipe },
        { "fsmall", b_fsmall }, { "f4m", b_f4m }, { "dir", b_dir }, { "fault", b_fault },
        { "str", b_str }, { "malloc", b_malloc }, { "stdio", b_stdio }, { "kmsg", b_kmsg },
    };
    if (argc > 1 && !strcmp(argv[1], "--poller")) { struct pollfd pf = { 0, POLLIN, 0 }; poll(&pf, 1, 60000); return 0; }
    int wl = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) wl = 1;
        else if (!strcmp(argv[i], "-k")) { if (!freopen("/dev/kmsg", "w", stdout)) return 1; }   /* -k: lines to the serial log (for --autostart) */
        else any = 1;
    }
    calibrate();
    for (unsigned k = 0; k < sizeof T / sizeof *T; k++) {
        int run = !any && !wl;
        for (int i = 1; i < argc; i++) if (!strcmp(argv[i], T[k].n)) run = 1;
        if (run) T[k].f();
    }
    if (wl) b_work();
    printf("BENCH-DONE\n");
    return 0;
}
