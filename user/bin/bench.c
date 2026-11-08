/* bench - app speed probes; results go to /dev/kmsg (serial). Run in the VM: sh /home/bench.sh > /dev/kmsg (tools: docs/PERF notes in CHANGELOG)
 *   bench launch APP [ARG...]  spawn APP, let it settle 1.5 s, report cpu ms used; with the first-frame probe in gui.c (PERF_PROBE)
 *                              the app logs "FF" when it first flips, the delta to "SPAWN" is exec -> first frame
 *   bench idle SECS            cpu ms used by every process over SECS (idle apps should be ~0)
 *   bench kill NAME            kill every process called NAME */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syscall.h>
#include "benchutil.h"
#include "image.h"

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void say(const char *fmt, ...) {
    char b[200]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    int fd = open("/dev/kmsg", O_WRONLY); if (fd >= 0) { write(fd, b, n); close(fd); }
}
static int cpu_of(int pid, struct procinfo *o) {
    struct procinfo p[64]; int n = proclist(p, 64);
    for (int i = 0; i < n; i++) if (p[i].pid == pid) { *o = p[i]; return 1; }
    return 0;
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "launch")) {
        char *av[8] = { 0 }; for (int i = 2; i < argc && i < 7; i++) av[i - 2] = argv[i];
        uint32_t t0 = uptime_ms();
        say("SPAWN %s t=%u\n", argv[2], t0);
        int pid = spawnv(argv[2], av);
        sleep(4);
        struct procinfo p;
        if (cpu_of(pid, &p)) say("LAUNCHED %s cpu_ms=%u mem_kb=%u\n", argv[2], p.cpu_ms, p.mem_kb);
        else say("LAUNCHED %s gone\n", argv[2]);
        kill(pid, SIGKILL); waitpid(pid, 0, 0);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "run")) {          /* run CMD to completion: wall ms (stdout of CMD discarded) */
        int nul = open("/dev/null", O_WRONLY); struct spawn_opts o = { { 0, nul, nul }, 0, 0 };
        for (int pass = 0; pass < 3; pass++) {
            uint64_t t = tsc(); int pid = spawnve(argv[2], argv + 2, 0, &o), st = 0; waitpid(pid, &st, 0);
            say("RUN %s pass %d: %u us (exit %d)\n", argv[2], pass, us(tsc() - t), st);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "idle")) {
        struct procinfo a[64], b[64]; int na = proclist(a, 64);
        sleep(atoi(argv[2]));
        int nb = proclist(b, 64);
        for (int i = 0; i < nb; i++) for (int j = 0; j < na; j++)
            if (a[j].pid == b[i].pid && b[i].cpu_ms - a[j].cpu_ms >= 5) say("IDLE %s pid=%d cpu_ms=%u over %ss\n", b[i].name, b[i].pid, b[i].cpu_ms - a[j].cpu_ms, argv[2]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "kill")) {
        struct procinfo p[64]; int n = proclist(p, 64);
        for (int i = 0; i < n; i++) if (!strcmp(p[i].name, argv[2])) kill(p[i].pid, SIGKILL);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "read")) {        /* read FILE in 64 KiB chunks: KB/s */
        static char buf[65536];
        for (int pass = 0; pass < 3; pass++) {
            int fd = open(argv[2], O_RDONLY); long n = 0, r; uint64_t t = tsc();
            while ((r = read(fd, buf, sizeof buf)) > 0) n += r;
            close(fd); uint32_t u = us(tsc() - t);
            say("READ %s pass %d: %ld bytes in %u us = %u KB/s\n", argv[2], pass, n, u, (uint32_t)((uint64_t)n * 1000000 / 1024 / (u ? u : 1)));
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "image")) {       /* decode each FILE, then fit it to 880x560 and sample 64x64 */
        for (int i = 2; i < argc; i++) {
            struct image_anim a; uint64_t t = tsc();
            if (image_load_anim(argv[i], &a) < 0) { say("IMAGE %s failed\n", argv[i]); continue; }
            uint32_t d = us(tsc() - t);
            float k = 880.0f / a.w; if (560.0f / a.h < k) k = 560.0f / a.h; if (k > 1) k = 1;
            int dw = a.w * k, dh = a.h * k; uint32_t *dst = malloc((size_t)880 * 560 * 4);
            struct image im = { a.w, a.h, a.px[0] };
            t = tsc(); image_sample(&im, dst, dw, dh, dw, 0, 0, k, 1); uint32_t f = us(tsc() - t);
            t = tsc(); image_sample(&im, dst, 64, 64, 64, 0, 0, 64.0f / (a.w > a.h ? a.w : a.h), 1); uint32_t th = us(tsc() - t);
            say("IMAGE %s %dx%d frames=%d decode %u us, fit-sample %u us, thumb-sample %u us\n", argv[i], a.w, a.h, a.n, d, f, th);
            free(dst); image_anim_free(&a);
        }
        return 0;
    }
    fprintf(stderr, "usage: bench launch APP | idle SECS | run CMD ARGS | kill NAME | read FILE\n");
    return 1;
}
