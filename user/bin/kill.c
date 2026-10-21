/* kill [-SIG | -s SIG | -n NUM] pid...    kill -l lists names */
#include <vutil.h>
#include <signal.h>
#include <ctype.h>

static const struct { const char *n; int s; } sigs[] = {
    { "HUP", 1 }, { "INT", SIGINT }, { "QUIT", 3 }, { "KILL", SIGKILL }, { "USR1", 10 },
    { "USR2", 12 }, { "PIPE", 13 }, { "ALRM", 14 }, { "TERM", SIGTERM }, { "CHLD", 17 },
};

static int signo(const char *s) {
    if (isdigit((unsigned char)*s)) return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++)
        if (!strcasecmp(sigs[i].n, s)) return sigs[i].s;
    return -1;
}

int main(int argc, char **argv) {
    int sig = SIGTERM, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-l")) {
        for (unsigned k = 0; k < sizeof sigs / sizeof *sigs; k++) printf("%2d) SIG%s\n", sigs[k].s, sigs[k].n);
        return 0;
    }
    if (i < argc && (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-n")) && i + 1 < argc) {
        if ((sig = signo(argv[i + 1])) < 0) { fprintf(stderr, "kill: %s: invalid signal specification\n", argv[i + 1]); return 1; }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if ((sig = signo(argv[i] + 1)) < 0) { fprintf(stderr, "kill: %s: invalid signal specification\n", argv[i] + 1); return 1; }
        i++;
    }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i >= argc) { fprintf(stderr, "usage: kill [-9 | -SIGNAME] pid...\n"); return 2; }
    for (; i < argc; i++) {
        char *e;
        long pid = strtol(argv[i], &e, 10);
        if (*e || e == argv[i]) { fprintf(stderr, "kill: %s: arguments must be process IDs\n", argv[i]); rc = 1; continue; }
        if (kill(pid, sig) < 0) { fprintf(stderr, "kill: (%ld): %s\n", pid, strerror(errno)); rc = 1; }
    }
    return rc;
}
