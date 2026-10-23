/* sh - VibeOS shell.  sh | sh -c 'cmd' | sh script [args] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>
#include "sh.h"

#define C(s) "\x1b[" s "m"

/* the prompt uses the terminal's ANSI palette, so it follows the theme: user@host green, path blue, punctuation dim */
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
    snprintf(title, sizeof title, "\x1b]0;%s@%s: %s\x07", user && *user ? user : "vibe", host && *host ? host : "vibeos", disp);
    write(1, title, strlen(title));
    snprintf(out, n, C("32") "%s@%s" C("90") ":" C("34") "%s" C("%s") "$" C("0") " ",
             user && *user ? user : "vibe", host && *host ? host : "vibeos", disp, g_status ? "31" : "90");
}

static char *tty_next(void *ctx, int cont) {
    char prompt[PATH_MAX + 256];
    if (cont) return readline_(C("2") "> " C("0"));
    reap_jobs();
    build_prompt(prompt, sizeof prompt);
    char *l = readline_(prompt);
    if (l && !g_intr) {                                  /* !! !prefix !N */
        int changed;
        char *e = hist_expand(l, &changed);
        free(l);
        if (!e) { g_status = 1; return strdup(""); }
        if (changed) printf("%s\n", e), fflush(stdout);
        l = e;
    }
    return l;
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
    int i = 1, cmd = 0;
    if (!getenv("PATH")) setenv("PATH", "/bin", 1);
    if (!getenv("HOME")) setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/sh", 1);
    if (getcwd(cwd, sizeof cwd)) setenv("PWD", cwd, 1);
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {      /* -e -x -c (combinable) */
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'c') cmd = 1;
            else { char f[3] = { '-', *p, 0 }; set_options(f); }
        }
    }
    static char *none[] = { "sh", NULL };
    if (cmd && i < argc) {                                         /* sh -c text [name args...] */
        const char *text = argv[i++];
        if (i < argc) set_pos(argc - i, argv + i); else set_pos(1, none);
        return run_lines(text) & 255;
    }
    if (i < argc) {
        set_pos(argc - i, argv + i);
        run_source(argv[i], 0, NULL);
        return g_status & 255;
    }
    set_pos(1, none);
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
