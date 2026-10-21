/* sh - VibeOS shell.  sh | sh -c 'cmd' | sh script [args] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sh.h"

#define C(s) "\x1b[" s "m"
#define RGB(r, g, b) "38;2;" #r ";" #g ";" #b

static void build_prompt(char *out, size_t n) {
    char cwd[PATH_MAX], title[PATH_MAX + 16];
    const char *home = getenv("HOME"), *user = getenv("USER"), *disp = cwd, *host = getenv("HOSTNAME");
    char tilde[PATH_MAX + 2];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(cwd, home, hl) && (!cwd[hl] || cwd[hl] == '/')) {
        snprintf(tilde, sizeof tilde, "~%s", cwd + hl);
        disp = tilde;
    }
    snprintf(title, sizeof title, "\x1b]0;sh: %s\x07", disp);
    write(1, title, strlen(title));
    snprintf(out, n, C("1;" RGB(189, 147, 249)) "%s" C("0") C(RGB(98, 114, 164)) "@" C("0") C("1;" RGB(139, 233, 253)) "%s" C("0")
             " " C(RGB(241, 250, 140)) "%s" C("0") " %s>" C("0") " ",
             user && *user ? user : "vibe", host && *host ? host : "vibeos", disp,
             g_status ? C("1;" RGB(255, 85, 85)) : C("1;" RGB(80, 250, 123)));
}

static char *tty_next(void *ctx, int cont) {
    char prompt[PATH_MAX + 256];
    if (cont) return readline_(C("2") "> " C("0"));
    reap_jobs();
    build_prompt(prompt, sizeof prompt);
    return readline_(prompt);
}

struct lrd { char *buf; size_t cap; };
static char *stdin_next(void *ctx, int cont) {
    struct lrd *r = ctx;
    ssize_t n = getline(&r->buf, &r->cap, stdin);
    if (n < 0) return NULL;
    while (n > 0 && (r->buf[n - 1] == '\n' || r->buf[n - 1] == '\r')) r->buf[--n] = 0;
    return strdup(r->buf);
}

static void source_if_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) run_source(path, 0, NULL);
}

int main(int argc, char **argv) {
    char cwd[PATH_MAX];
    if (!getenv("PATH")) setenv("PATH", "/bin", 1);
    if (!getenv("HOME")) setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/sh", 1);
    if (getcwd(cwd, sizeof cwd)) setenv("PWD", cwd, 1);
    static char *none[] = { "sh", NULL };
    g_sargc = 1; g_sargv = none;

    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        if (argc > 3) { g_sargc = argc - 3; g_sargv = argv + 3; }
        return run_lines(argv[2]) & 255;
    }
    if (argc >= 2) {
        run_source(argv[1], argc - 1, argv + 1);
        return g_status & 255;
    }
    if (!isatty(0)) {
        struct lrd r = { 0 };
        run_reader(stdin_next, &r);
        return g_status & 255;
    }
    g_interactive = 1;
    hist_load();
    source_if_exists("/etc/profile");
    char rc[PATH_MAX];
    snprintf(rc, sizeof rc, "%s/.shrc", getenv("HOME"));
    source_if_exists(rc);
    g_status = 0;
    run_reader(tty_next, NULL);
    return g_status & 255;
}
