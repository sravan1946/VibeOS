/* xargs [-n N] [-I REPL] [-0] [-t] [-r] [command [args...]]   default command /bin/echo; no fork, so spawnvp + waitpid */
#include <vutil.h>
#include <ctype.h>

static char **items;
static int nitems, capi;

static void add(char *s) {
    if (nitems == capi) items = realloc(items, (capi = capi ? capi * 2 : 64) * sizeof *items);
    items[nitems++] = s;
}

static int trace, rc;

static void run(char **argv) {
    if (trace) {
        for (char **p = argv; *p; p++) fprintf(stderr, "%s%s", p == argv ? "" : " ", *p);
        fputc('\n', stderr);
    }
    fflush(stdout);
    int pid = spawnvp(argv[0], argv), st = 0;
    if (pid < 0) { fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(errno)); exit(127); }
    waitpid(pid, &st, 0);
    if (st) rc = 123;
}

int main(int argc, char **argv) {
    int maxn = 0, nul = 0, noempty = 0, i = 1;
    const char *repl = NULL;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        const char *f = argv[i] + 1;
        if (*f == 'n' || *f == 'I' || *f == 'L') {
            const char *a = f[1] ? f + 1 : i + 1 < argc ? argv[++i] : NULL;
            if (!a) goto usage;
            if (*f == 'I') { repl = a; maxn = 1; } else maxn = atoi(a);
            if (maxn < 1) goto usage;
        } else for (; *f; f++) {
            if (*f == '0') nul = 1; else if (*f == 't') trace = 1; else if (*f == 'r') noempty = 1; else goto usage;
        }
    }
    char *dflt[] = { "/bin/echo", NULL };
    char **cmd = i < argc ? argv + i : dflt;
    int ncmd = i < argc ? argc - i : 1;
    size_t len;
    char *b = readall(0, &len);
    if (!b) { WARN("xargs", "stdin"); return 1; }
    if (repl) {                                  /* one item per input line */
        for (size_t p = 0; p < len;) {
            char *e = memchr(b + p, '\n', len - p);
            size_t ll = e ? (size_t)(e - (b + p)) : len - p;
            b[p + ll] = 0;
            char *ln = b + p;
            p += ll + 1;
            while (*ln == ' ' || *ln == '\t') ln++;
            if (!*ln) continue;
            char **av = calloc(ncmd + 1, sizeof *av);
            for (int k = 0; k < ncmd; k++) {
                const char *s = cmd[k];
                size_t rl = strlen(repl), cap = strlen(s) + 1, n = 0;
                char *o = malloc(cap);
                for (; *s; ) {
                    if (!strncmp(s, repl, rl)) {
                        cap += strlen(ln); o = realloc(o, cap);
                        memcpy(o + n, ln, strlen(ln)); n += strlen(ln); s += rl;
                    } else o[n++] = *s++;
                }
                o[n] = 0;
                av[k] = o;
            }
            run(av);
        }
        return rc;
    }
    if (nul) {
        for (size_t p = 0; p < len; p += strlen(b + p) + 1) add(b + p);
    } else {                                     /* whitespace separated, with '..' ".." and \ quoting */
        char *r = b, *w;
        for (;;) {
            while (*r && isspace((unsigned char)*r)) r++;
            if (!*r) break;
            char *st = w = r;
            while (*r && !isspace((unsigned char)*r)) {
                if (*r == '\'' || *r == '"') { char q = *r++; while (*r && *r != q) *w++ = *r++; if (*r) r++; }
                else if (*r == '\\' && r[1]) { r++; *w++ = *r++; }
                else *w++ = *r++;
            }
            if (*r) r++;
            *w = 0;
            add(st);
        }
    }
    if (!nitems) { if (!noempty) run(cmd); return rc; }
    for (int k = 0; k < nitems;) {
        int cnt = maxn ? (nitems - k < maxn ? nitems - k : maxn) : nitems - k;
        if (!maxn && cnt > 2000) cnt = 2000;
        char **av = calloc(ncmd + cnt + 1, sizeof *av);
        memcpy(av, cmd, ncmd * sizeof *av);
        memcpy(av + ncmd, items + k, cnt * sizeof *av);
        run(av);
        free(av);
        k += cnt;
    }
    return rc;
usage:
    fputs("usage: xargs [-n N] [-I REPL] [-0] [-t] [-r] [command [args...]]\n", stderr);
    return 1;
}
