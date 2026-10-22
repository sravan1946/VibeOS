/* memtest - checks demand paging (lazy brk, stack growth), mmap/munmap/mprotect, file mappings,
 * SIGSEGV from guard pages, big malloc going back to the system, and thread stack guards.
 * The x-* arguments are helper images it runs to watch them crash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <syscall.h>

#define PG 4096
static int pass, fail;
static void check(int ok, const char *what) {
    if (ok) pass++; else { fail++; printf("FAIL: %s\n", what); }
}

static int run(const char *mode) {
    char *argv[] = { "memtest", (char *)mode, 0 };
    int st = -1, pid = spawnv("/bin/memtest", argv);
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return pid > 0 ? WEXITSTATUS(st) : -1;
}

static uint32_t my_kb(void) {                       /* resident user pages, as ps shows them */
    static struct procinfo p[128];
    int n = proclist(p, 128), me = getpid();
    for (int i = 0; i < n; i++) if (p[i].pid == me) return p[i].mem_kb;
    return 0;
}
static uint32_t free_kb(void) { struct sysinfo si; sysinfo(&si); return si.mem_free_kb; }

/* touch a byte, report whether it faulted (SIGSEGV caught, the mask restored) */
static jmp_buf jb;
static volatile int segvs;
static void on_segv(int s) { segvs++; longjmp(jb, 1); }
static int faults(volatile char *p, int write) {
    sigset_t s;
    sigemptyset(&s); sigaddset(&s, SIGSEGV);
    if (setjmp(jb)) { sigprocmask(SIG_UNBLOCK, &s, 0); return 1; }
    if (write) *p = 1; else (void)*p;
    return 0;
}

static int recurse(int n) {                          /* ~4 KiB of stack per level */
    volatile char buf[4000];
    buf[0] = buf[3999] = 1;
    return n ? recurse(n - 1) + buf[0] + buf[3999] - 2 : 0;
}
static void *thr_recurse(void *a) { return (void *)(intptr_t)recurse((intptr_t)a); }

static int helper(const char *m) {
    if (!strcmp(m, "x-stack")) return recurse(100000);                     /* 400 MB: past the 8 MiB limit */
    if (!strcmp(m, "x-guard")) {                                           /* no handler: killed with 139 */
        char *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(p, PG, PROT_NONE);
        p[PG] = 1;
        p[0] = 1;
        return 0;
    }
    if (!strcmp(m, "x-thrstack")) {                                        /* 64 KiB thread stack, 2 MB deep */
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 * 1024);
        pthread_create(&t, &a, thr_recurse, (void *)500);
        pthread_join(t, 0);
        return 0;
    }
    return 64;
}

int main(int argc, char **argv) {
    if (argc > 1) return helper(argv[1]);
    signal(SIGSEGV, on_segv);

    /* ---- lazy brk ---- */
    uint32_t k0 = my_kb();
    char *b = sbrk(16 << 20);
    check(b != (char *)-1, "sbrk 16 MiB");
    uint32_t k1 = my_kb();
    check(k1 - k0 < 64, "sbrk doesn't make pages resident");
    for (int i = 0; i < 256; i++) b[i * PG] = i;                          /* 1 MiB of it */
    uint32_t k2 = my_kb();
    check(k2 - k1 >= 1024 && k2 - k1 < 1024 + 64, "touched brk pages become resident (1 MiB)");
    int ok = 1;
    for (int i = 0; i < 256; i++) ok &= b[i * PG] == (char)i && b[i * PG + 1] == 0;
    check(ok, "brk pages are zero-filled and keep their data");
    sbrk(-(16 << 20));
    check(my_kb() <= k0 + 8, "sbrk shrink frees them");

    /* ---- stack ---- */
    k0 = my_kb();
    check(recurse(1000) == 0, "main stack grows to 4 MB on demand");
    check(my_kb() - k0 >= 3900, "...and the pages are resident");
    check(run("x-stack") == 128 + SIGSEGV, "past the 8 MiB stack limit: SIGSEGV (139)");

    /* ---- mmap / munmap ---- */
    errno = 0;
    check(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL, "mmap of 0 bytes: EINVAL");
    check(mmap(NULL, PG, PROT_READ, MAP_PRIVATE, 99, 0) == MAP_FAILED && errno == EBADF, "mmap of a bad fd: EBADF");
    check(mmap(NULL, PG, PROT_READ, MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL, "mmap without SHARED/PRIVATE: EINVAL");
    check(munmap((void *)0x50000001, PG) < 0 && errno == EINVAL, "munmap of an unaligned address: EINVAL");
    k0 = my_kb();
    char *m = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m != MAP_FAILED && ((uintptr_t)m & (PG - 1)) == 0, "mmap 4 MiB anonymous");
    check(my_kb() - k0 < 16, "...lazily");
    for (int i = 0; i < 512; i++) m[i * PG + 7] = i;
    ok = 1;
    for (int i = 0; i < 1024; i++) ok &= m[i * PG + 7] == (i < 512 ? (char)i : 0);
    check(ok && my_kb() - k0 >= 4096, "mapped pages read back, untouched ones are zero");
    uint32_t f0 = free_kb();
    check(munmap(m, 4 << 20) == 0 && free_kb() - f0 >= 4000 && my_kb() <= k0 + 8, "munmap gives the frames back");
    check(faults(m + 5, 0), "touching an unmapped range: SIGSEGV");
    char *m2 = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m2 == m && m2[7] == 0, "the range is reused, zeroed");
    check(munmap(m2 + PG, PG) == 0 && faults(m2 + PG, 0) && !faults(m2, 1) && !faults(m2 + 2 * PG, 1), "munmap of one page in the middle");
    munmap(m2, 4 << 20);

    /* MAP_SHARED anonymous: one memory for all threads */
    int *sh = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check(sh != MAP_FAILED, "MAP_SHARED anonymous");

    /* ---- mprotect ---- */
    char *pr = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    strcpy(pr + PG, "kept");
    check(mprotect(pr + PG, PG, PROT_READ) == 0, "mprotect read-only");
    check(faults(pr + PG, 1) && !faults(pr + PG, 0) && !strcmp(pr + PG, "kept"), "...writes fault, reads work");
    check(!faults(pr, 1) && !faults(pr + 2 * PG, 1), "...the neighbours stay writable");
    check(mprotect(pr, 3 * PG, PROT_NONE) == 0 && faults(pr + PG, 0) && faults(pr + 2 * PG, 0), "PROT_NONE: reads fault");
    check(mprotect(pr, 3 * PG, PROT_READ | PROT_WRITE) == 0 && !strcmp(pr + PG, "kept") && !faults(pr + PG, 1), "back to read-write, contents kept");
    check(mprotect(pr, PG, PROT_READ) == 0 && read(open("/home/welcome.txt", O_RDONLY), pr, 4) < 0 && errno == EFAULT,
          "read() into a read-only page: EFAULT");
    check(mprotect((void *)0x50000000, PG, PROT_READ) < 0 && errno == ENOMEM, "mprotect of an unmapped range: ENOMEM");
    char *heap = malloc(100);
    check(mprotect((void *)((uintptr_t)heap & ~(PG - 1)), PG, PROT_READ | PROT_WRITE | PROT_EXEC) == 0, "mprotect RWX on the heap (tcc -run)");
    char *bk = sbrk(0);
    sbrk(2 * PG);
    bk = (char *)(((uintptr_t)bk + PG - 1) & ~(PG - 1));
    check(mprotect(bk, PG, PROT_NONE) == 0 && faults(bk, 0), "mprotect PROT_NONE on a lazy brk page");
    mprotect(bk, PG, PROT_READ | PROT_WRITE);
    munmap(pr, 3 * PG);

    /* ---- file mappings ---- */
    const char *path = "/home/.memtest.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    static char pat[3 * PG + 100];
    for (int i = 0; i < (int)sizeof pat; i++) pat[i] = i * 7 + i / PG;
    check(fd >= 0 && write(fd, pat, sizeof pat) == (int)sizeof pat, "write the data file");
    char *fm = mmap(NULL, 3 * PG, PROT_READ, MAP_PRIVATE, fd, PG);
    check(fm != MAP_FAILED, "mmap a file at offset 4096");
    check(fm != MAP_FAILED && !memcmp(fm, pat + PG, 2 * PG + 100), "...its pages hold the file's bytes");
    ok = 1;
    for (int i = 2 * PG + 100; i < 3 * PG; i++) ok &= fm[i] == 0;
    check(ok, "...and zeros past the end of the file");
    check(faults(fm, 1), "...writing a PROT_READ file mapping: SIGSEGV");
    char *fw = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    fw[0] = ~pat[0];
    char c0;
    check(lseek(fd, 0, SEEK_SET) == 0 && read(fd, &c0, 1) == 1 && c0 == pat[0] && fw[1] == pat[1], "MAP_PRIVATE writes stay private");
    check(mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == ENODEV, "MAP_SHARED writable file: ENODEV");
    char *fs = mmap(NULL, PG, PROT_READ, MAP_SHARED, fd, 0);
    check(fs != MAP_FAILED && fs[5] == pat[5] && mprotect(fs, PG, PROT_READ | PROT_WRITE) < 0 && errno == EACCES,
          "MAP_SHARED read-only file mapping; no write permission later");
    close(fd);
    check(fm[PG] == pat[2 * PG], "the mapping outlives close()");
    char *rb = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fd = open(path, O_RDONLY);
    check(read(fd, rb, 2 * PG) == 2 * PG && !memcmp(rb, pat, 2 * PG), "read() straight into untouched mmap pages");
    close(fd);
    munmap(fm, 3 * PG); munmap(fw, PG); munmap(fs, PG); munmap(rb, 2 * PG);
    unlink(path);

    /* ---- malloc ---- */
    f0 = free_kb();
    char *big = malloc(8 << 20);
    memset(big, 1, 8 << 20);
    uint32_t f1 = free_kb();
    free(big);
    check(f0 - f1 >= 8000 && free_kb() + 64 >= f0, "free() of a big malloc returns its memory");
    int *z = calloc(1 << 20, 4);
    ok = z != 0;
    for (int i = 0; ok && i < 1 << 20; i += 1024) ok &= z[i] == 0;
    check(ok, "big calloc is zero");
    free(z);
    char *rs = malloc(200000);
    memset(rs, 3, 200000);
    rs = realloc(rs, 1 << 20);
    check(rs && rs[0] == 3 && rs[199999] == 3, "realloc of a big block keeps its data");
    rs = realloc(rs, 10);
    check(rs && rs[9] == 3, "...and back down into the heap");
    free(rs);

    /* ---- guard pages ---- */
    check(run("x-guard") == 128 + SIGSEGV, "touching a PROT_NONE guard without a handler: 139");
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);
    void *rv = (void *)1;
    check(pthread_create(&t, &a, thr_recurse, (void *)200) == 0 && pthread_join(t, &rv) == 0 && rv == 0,
          "a thread recurses 800 KB into a 1 MiB stack");
    check(run("x-thrstack") == 128 + SIGSEGV, "thread stack overflow hits its guard page: 139");
    check(segvs > 0, "the SIGSEGV handler ran");

    printf("%d passed, %d failed\n", pass, fail);
    return fail != 0;
}
