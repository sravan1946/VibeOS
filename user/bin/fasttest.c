/* fasttest - checks for the performance fast paths: ext2 block cache (eviction, read-ahead, delayed write-back,
 * fsync), lazy FPU switching (threads, processes, signal handlers), the pipe copy loop and wrap-around, malloc's
 * small-block cache, and the libc lock dropping back after threads end.  `fasttest x-fp N` is the helper image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <syscall.h>

static int pass, fail;
static void check(int ok, const char *what) {
    if (ok) pass++; else { fail++; printf("FAIL: %s\n", what); }
}

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static unsigned char pat(unsigned long pos) { return (unsigned char)(pos * 2654435761u >> 13) ^ (unsigned char)(pos >> 11); }

/* ---- floating point: every task's x87 registers must survive switches, signals and exec ---- */
/* no calls and nothing volatile: the accumulator lives in an x87 register for the whole loop (~20 ms), so the
 * timer preempts it mid-computation again and again and every switch has live FPU state to keep */
static double fpwork(int seed) {
    double a = 1.0 + seed, s = 0;
    for (long i = 0; i < 1500000; i++) s = s * 0.9999999 + a / (3.0 + (i & 7));
    volatile double r = s;                          /* rounded to a double, so results compare exactly */
    return r;
}
static double fpexpect[8];
static volatile int fpbad;
static void *fpthread(void *a) {
    int seed = (int)(intptr_t)a;
    for (int k = 0; k < 3; k++) if (fpwork(seed) != fpexpect[seed]) fpbad++;
    return 0;
}
static volatile double sig_result;
static void on_usr1(int s) {                        /* uses the FPU inside a handler that interrupted FP code */
    volatile double x = 1.5;
    for (int i = 0; i < 100; i++) x = x * 1.01 + 0.5;
    sig_result = x;
}

static int helper(int argc, char **argv) {
    int seed = atoi(argv[2]);
    for (int k = 0; k < 4; k++) if (fpwork(seed) != fpexpect[seed]) return 1;
    return 0;
}

static void test_fpu(void) {
    for (int i = 0; i < 8; i++) fpexpect[i] = fpwork(i);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], 0, fpthread, (void *)(intptr_t)(i + 1));
    char sd[8][4], *av[4];
    int pid[2];
    for (int i = 0; i < 2; i++) {
        snprintf(sd[i], sizeof sd[i], "%d", i + 5);
        av[0] = "fasttest"; av[1] = "x-fp"; av[2] = sd[i]; av[3] = 0;
        pid[i] = spawnv("/bin/fasttest", av);
    }
    int ok = 1;
    for (int k = 0; k < 3; k++) if (fpwork(0) != fpexpect[0]) ok = 0;
    for (int i = 0; i < 4; i++) pthread_join(th[i], 0);
    for (int i = 0; i < 2; i++) { int st = -1; waitpid(pid[i], &st, 0); if (WEXITSTATUS(st)) ok = 0; }
    check(ok && !fpbad, "x87 state survives switches between threads, children and the parent");
    signal(SIGUSR1, on_usr1);
    volatile double a = 3.25, b = 7.5, c = a * b;
    volatile double want = 1.5;
    for (int i = 0; i < 100; i++) want = want * 1.01 + 0.5;
    int sok = 1, me = getpid();
    for (int i = 0; i < 200; i++) {
        volatile double out = 0;
        /* a*b stays on the x87 stack across kill(): the handler runs on the way out of the syscall and uses the FPU */
        __asm__ volatile("fldl %1; fldl %2; fmulp; int $0x80; fstpl %0"
                         : "=m"(out) : "m"(a), "m"(b), "a"(SYS_kill), "b"(me), "c"(SIGUSR1) : "memory");
        if (out != c) sok = 0;
        if (i % 16 == 0) yield();
    }
    check(sok && sig_result == want, "x87 state survives a signal handler that uses the FPU");
}

/* ---- files ---- */
static int fill_file(const char *path, unsigned long size, int seedchunk) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    static unsigned char buf[70000];
    unsigned long pos = 0;
    while (pos < size) {
        unsigned n = 1 + (rnd() % (seedchunk ? seedchunk : 66000));
        if (n > size - pos) n = size - pos;
        for (unsigned i = 0; i < n; i++) buf[i] = pat(pos + i);
        if (write(fd, buf, n) != (int)n) { close(fd); return -1; }
        pos += n;
    }
    return close(fd);
}
static int verify_file(const char *path, unsigned long size, int chunk) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static unsigned char buf[70000];
    unsigned long pos = 0;
    int bad = 0;
    for (;;) {
        unsigned want = chunk ? (unsigned)chunk : 1 + rnd() % 66000;
        int n = read(fd, buf, want);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) if (buf[i] != pat(pos + i)) { bad = 1; break; }
        pos += n;
    }
    close(fd);
    return bad || pos != size ? -1 : 0;
}

static void test_files(void) {
    mkdir("/tmp/ft", 0755);
    /* bigger than the 8 MiB cache: blocks are evicted (dirty ones written) while the file is still being written */
    check(fill_file("/tmp/ft/big", 11 * 1024 * 1024 + 777, 0) == 0, "write 11 MB in random chunk sizes");
    check(verify_file("/tmp/ft/big", 11 * 1024 * 1024 + 777, 0) == 0, "re-read 11 MB right after (mixed hits and misses)");
    check(fill_file("/tmp/ft/big2", 9 * 1024 * 1024, 4096) == 0, "write a second 9 MB file (evicts the first)");
    check(verify_file("/tmp/ft/big", 11 * 1024 * 1024 + 777, 3000) == 0, "first file reads back cold, 3000-byte reads");
    check(verify_file("/tmp/ft/big2", 9 * 1024 * 1024, 65536) == 0, "second file reads back, 64K reads");

    /* overwrite in the middle at odd offsets/lengths, across the direct/indirect/double-indirect boundaries */
    int fd = open("/tmp/ft/big", O_RDWR);
    static unsigned char b[5000];
    long offs[] = { 11 * 1024 - 7, 12 * 1024 + 100, 12 * 1024 + 256 * 1024 - 3, 300000, 5 * 1024 * 1024 + 1, 0 };
    int ok = 1;
    for (int k = 0; offs[k] || k == 5; k++) {
        for (int i = 0; i < 5000; i++) b[i] = pat(offs[k] + i);          /* same bytes as before: the file must not change */
        lseek(fd, offs[k], SEEK_SET);
        if (write(fd, b, 5000) != 5000) ok = 0;
        if (offs[k] == 0) break;
    }
    close(fd);
    check(ok && verify_file("/tmp/ft/big", 11 * 1024 * 1024 + 777, 0) == 0, "partial-block overwrites keep the neighbours intact");

    /* holes read as zeros; truncating then growing exposes zeros, not old data */
    fd = open("/tmp/ft/sparse", O_RDWR | O_CREAT | O_TRUNC);
    lseek(fd, 3 * 1024 * 1024 + 5, SEEK_SET);
    write(fd, "tail", 4);
    unsigned char z[4096];
    lseek(fd, 1000000, SEEK_SET);
    int n = read(fd, z, sizeof z), zok = n == 4096;
    for (int i = 0; i < n; i++) if (z[i]) zok = 0;
    check(zok, "a hole reads as zeros");
    memset(z, 0xAA, sizeof z);
    lseek(fd, 0, SEEK_SET);
    write(fd, z, sizeof z);
    ftruncate(fd, 100);
    ftruncate(fd, 5000);
    lseek(fd, 0, SEEK_SET);
    unsigned char r[5000];
    n = read(fd, r, sizeof r);
    zok = n == 5000;
    for (int i = 0; i < 100 && zok; i++) zok = r[i] == 0xAA;
    for (int i = 100; i < 5000 && zok; i++) zok = r[i] == 0;
    check(zok, "truncate down then up: the cut part reads as zeros");
    close(fd);

    /* a file just written is what exec loads and mmap maps, before anything was flushed */
    fd = open("/bin/true", O_RDONLY);
    static char img[200000];
    int len = read(fd, img, sizeof img);
    close(fd);
    fd = open("/tmp/ft/true2", O_WRONLY | O_CREAT | O_TRUNC);
    write(fd, img, len);
    close(fd);
    char *av[] = { "true2", 0 };
    int pid = spawnv("/tmp/ft/true2", av), st = -1;
    check(pid > 0 && waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0, "exec a file written a moment ago");
    fd = open("/tmp/ft/big2", O_RDONLY);
    unsigned char *m = mmap(0, 1 << 20, PROT_READ, MAP_PRIVATE, fd, 4096 * 3);
    int mok = m != MAP_FAILED;
    for (long i = 0; mok && i < (1 << 20); i += 997) if (m[i] != pat(4096 * 3 + i)) mok = 0;
    check(mok, "mmap of a file reads through the cache");
    if (m != MAP_FAILED) munmap(m, 1 << 20);
    close(fd);

    /* many small files: create, check, rename half, unlink half, count what is left */
    char p[80], q[80], buf[100];
    int made = 1;
    for (int i = 0; i < 300; i++) {
        snprintf(p, sizeof p, "/tmp/ft/f%03d", i);
        fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
        snprintf(buf, sizeof buf, "file number %d\n", i);
        if (fd < 0 || write(fd, buf, strlen(buf)) != (int)strlen(buf)) made = 0;
        close(fd);
    }
    for (int i = 0; i < 300; i += 2) {
        snprintf(p, sizeof p, "/tmp/ft/f%03d", i);
        snprintf(q, sizeof q, "/tmp/ft/g%03d", i);
        if (rename(p, q) < 0) made = 0;
    }
    for (int i = 1; i < 300; i += 4) { snprintf(p, sizeof p, "/tmp/ft/f%03d", i); if (unlink(p) < 0) made = 0; }
    int fcnt = 0, gcnt = 0, content = 1;
    DIR *d = opendir("/tmp/ft");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == 'f' && e->d_name[1] >= '0' && e->d_name[1] <= '9') fcnt++;
        if (e->d_name[0] == 'g') gcnt++;
    }
    closedir(d);
    for (int i = 0; i < 300; i++) {
        if (i % 4 == 1) continue;
        snprintf(p, sizeof p, "/tmp/ft/%c%03d", i % 2 ? 'f' : 'g', i);
        fd = open(p, O_RDONLY);
        int k = fd < 0 ? -1 : read(fd, buf, sizeof buf - 1);
        if (k < 0) { content = 0; continue; }
        buf[k] = 0; close(fd);
        char want[40];
        snprintf(want, sizeof want, "file number %d\n", i);
        if (strcmp(buf, want)) content = 0;
    }
    check(made && fcnt == 75 && gcnt == 150 && content, "300 small files: create, rename, unlink, list, read back");

    /* fsync: ok on a file, EBADF on junk */
    fd = open("/tmp/ft/big", O_RDWR);
    check(fsync(fd) == 0 && fsync(99) < 0 && errno == EBADF, "fsync works and rejects a bad descriptor");
    close(fd);

    unlink("/tmp/ft/big"); unlink("/tmp/ft/big2"); unlink("/tmp/ft/sparse"); unlink("/tmp/ft/true2");
    for (int i = 0; i < 300; i++) {
        snprintf(p, sizeof p, "/tmp/ft/f%03d", i); unlink(p);
        snprintf(p, sizeof p, "/tmp/ft/g%03d", i); unlink(p);
    }
    check(rmdir("/tmp/ft") == 0, "everything unlinked, directory removable");
}

/* ---- pipes: wrap-around and odd sizes ---- */
static int pfd[2];
static void *pipe_writer(void *a) {
    static unsigned char buf[20000];
    unsigned long pos = 0, total = 3 * 1024 * 1024;
    unsigned r = 99;
    while (pos < total) {
        r = r * 1103515245u + 12345;
        unsigned n = 1 + (r >> 8) % 19999;
        if (n > total - pos) n = total - pos;
        for (unsigned i = 0; i < n; i++) buf[i] = pat(pos + i);
        if (write(pfd[1], buf, n) != (int)n) break;
        pos += n;
    }
    close(pfd[1]);
    return 0;
}
static void test_pipe(void) {
    pipe(pfd);
    pthread_t th;
    pthread_create(&th, 0, pipe_writer, 0);
    static unsigned char buf[9000];
    unsigned long pos = 0;
    int bad = 0, n;
    unsigned r = 7;
    while ((n = read(pfd[0], buf, 1 + (r = r * 22695477u + 1) % 8999)) > 0) {
        for (int i = 0; i < n; i++) if (buf[i] != pat(pos + i)) bad = 1;
        pos += n;
    }
    pthread_join(th, 0);
    close(pfd[0]);
    check(!bad && pos == 3 * 1024 * 1024, "3 MB through an 8K pipe with random read/write sizes arrives intact");
}

/* ---- malloc: small-block cache, coalescing, calloc, realloc ---- */
static void test_malloc(void) {
    enum { N = 600 };
    static unsigned char *p[N];
    static unsigned sz[N];
    int ok = 1;
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < N; i++) {
            if (p[i]) {
                for (unsigned k = 0; k < sz[i]; k++) if (p[i][k] != (unsigned char)(i + k)) ok = 0;
                free(p[i]); p[i] = 0;
            }
            if (rnd() % 3 == 0) continue;
            unsigned s = rnd() % 4 == 0 ? 1 + rnd() % 3000 : 1 + rnd() % 140;      /* mostly the cached sizes */
            p[i] = malloc(s);
            sz[i] = s;
            if (!p[i]) { ok = 0; continue; }
            for (unsigned k = 0; k < s; k++) p[i][k] = (unsigned char)(i + k);
        }
        for (int i = 0; i < N; i += 7) if (p[i]) {              /* realloc grows and shrinks */
            unsigned s = 1 + rnd() % 400;
            unsigned char *q = realloc(p[i], s);
            if (!q) { ok = 0; continue; }
            for (unsigned k = 0; k < (s < sz[i] ? s : sz[i]); k++) if (q[k] != (unsigned char)(i + k)) ok = 0;
            p[i] = q; sz[i] = s;
            for (unsigned k = 0; k < s; k++) p[i][k] = (unsigned char)(i + k);
        }
    }
    for (int i = 0; i < N; i++) free(p[i]);
    check(ok, "randomised malloc/free/realloc keeps every block's contents");
    int zok = 1;
    for (int r = 0; r < 50; r++) {                              /* calloc of reused small blocks is really zero */
        unsigned char *a = malloc(48);
        memset(a, 0xFF, 48);
        free(a);
        unsigned char *c = calloc(1, 48);
        for (int i = 0; i < 48; i++) if (c[i]) zok = 0;
        free(c);
    }
    check(zok, "calloc returns zeroed memory from the small-block cache");
    void *big[8];                                               /* freeing a pile of small blocks lets a big one fit: they merge back */
    int mok = 1;
    for (int i = 0; i < 8; i++) { big[i] = malloc(96); if (!big[i]) mok = 0; }
    for (int i = 0; i < 8; i++) free(big[i]);
    void *whole = malloc(700);
    mok = mok && whole;
    free(whole);
    check(mok, "small blocks merge back when a larger request needs the space");
}

/* ---- threads: libc's lock switches off again after the last join ---- */
static void *churn(void *a) {
    for (int i = 0; i < 2000; i++) { void *m = malloc(40 + i % 50); free(m); }
    return a;
}
static void test_threads(void) {
    int ok = 1;
    for (int round = 0; round < 3; round++) {
        pthread_t t[3];
        for (int i = 0; i < 3; i++) pthread_create(&t[i], 0, churn, (void *)(intptr_t)i);
        for (int i = 0; i < 2000; i++) { void *m = malloc(64); free(m); }
        for (int i = 0; i < 3; i++) { void *r; pthread_join(t[i], &r); if ((intptr_t)r != i) ok = 0; }
        for (int i = 0; i < 2000; i++) { void *m = malloc(64); free(m); }       /* single-threaded again */
    }
    check(ok, "threads created and joined repeatedly: malloc stays consistent as the lock goes on and off");
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "x-fp")) {
        for (int i = 0; i < 8; i++) fpexpect[i] = fpwork(i);
        return helper(argc, argv);
    }
    test_fpu();
    test_files();
    test_pipe();
    test_malloc();
    test_threads();
    printf("fasttest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
