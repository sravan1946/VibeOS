/* sh - VibeOS shell.  sh | sh -c 'cmd' | sh script [args] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>
#include <theme.h>
#include "sh.h"

#define C(s) "\x1b[" s "m"
#undef RGB                               /* gfx.h (via theme.h) has one; here it spells a truecolor SGR */
#define RGB(r, g, b) "38;2;" #r ";" #g ";" #b

/* the prompt keeps its colours on dark themes; light ones get darker shades of them */
static int prompt_dark(void) {
    struct theme_state st;
    int fd = open("/dev/theme", O_RDONLY), r = fd >= 0 ? read(fd, &st, offsetof(struct theme_state, th)) : -1;
    if (fd >= 0) close(fd);
    return r < (int)offsetof(struct theme_state, th) || theme_get(st.index)->dark;
}

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
    if (prompt_dark())
        snprintf(out, n, C("1;" RGB(189, 147, 249)) "%s" C("0") C(RGB(98, 114, 164)) "@" C("0") C("1;" RGB(139, 233, 253)) "%s" C("0")
                 " " C(RGB(241, 250, 140)) "%s" C("0") " %s>" C("0") " ",
                 user && *user ? user : "vibe", host && *host ? host : "vibeos", disp,
                 g_status ? C("1;" RGB(255, 85, 85)) : C("1;" RGB(80, 250, 123)));
    else
        snprintf(out, n, C("1;" RGB(106, 61, 216)) "%s" C("0") C(RGB(85, 82, 111)) "@" C("0") C("1;" RGB(11, 114, 133)) "%s" C("0")
                 " " C(RGB(138, 90, 0)) "%s" C("0") " %s>" C("0") " ",
                 user && *user ? user : "vibe", host && *host ? host : "vibeos", disp,
                 g_status ? C("1;" RGB(194, 31, 63)) : C("1;" RGB(17, 128, 63)));
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
