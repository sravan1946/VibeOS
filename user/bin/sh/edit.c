/* edit.c - raw-mode line editor, history (persisted in ~/.sh_history) and Tab completion */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "sh.h"

#define LMAX 1024
#define HMAX 500

/* ---- history ---- */
static char *hist[HMAX];
static int hn;
static char hpath[PATH_MAX];

int hist_count(void) { return hn; }
const char *hist_get(int i) { return hist[i]; }
void hist_clear(void) {
    while (hn) free(hist[--hn]);
    if (hpath[0]) { int fd = open(hpath, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) close(fd); }
}

static void hist_push(const char *l) {
    if (hn == HMAX) { free(hist[0]); memmove(hist, hist + 1, (HMAX - 1) * sizeof *hist); hn--; }
    hist[hn++] = strdup(l);
}

void hist_load(void) {
    const char *h = getenv("HOME");
    snprintf(hpath, sizeof hpath, "%s/.sh_history", h && *h ? h : "");
    FILE *f = fopen(hpath, "r");
    if (!f) return;
    char *ln = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&ln, &cap, f)) >= 0) {
        while (n > 0 && (ln[n - 1] == '\n' || ln[n - 1] == '\r')) ln[--n] = 0;
        if (n) hist_push(ln);
    }
    free(ln);
    fclose(f);
}

void hist_add(const char *line) {
    if (!*line || (hn && !strcmp(hist[hn - 1], line))) return;
    hist_push(line);
    if (!hpath[0]) return;
    int fd = open(hpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
    close(fd);
}

/* ---- terminal helpers ---- */
int term_cols(void) {
    struct winsize ws;
    return ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 ? ws.ws_col : 80;
}

static void outs(const char *s) { write(1, s, strlen(s)); }

/* printed width of s: skips CSI sequences, counts UTF-8 lead bytes */
static int vwidth(const char *s) {
    int w = 0;
    for (; *s; s++) {
        if (*s == 27) {
            if (s[1] == '[') { for (s += 2; *s && !isalpha((unsigned char)*s); s++) {} if (!*s) break; }
            continue;
        }
        if (((unsigned char)*s & 0xC0) != 0x80) w++;
    }
    return w;
}

struct ed {
    char buf[LMAX];
    int len, pos, plen, cols, crow;
    const char *prompt;
};

/* redraw prompt + line and put the cursor at pos; handles lines wrapping over several rows */
static void refresh(struct ed *e) {
    struct sb o = { 0 };
    char tmp[24];
    int total = e->plen + e->len, prow = (e->plen + e->pos) / e->cols, pcol = (e->plen + e->pos) % e->cols;
    if (e->crow) { snprintf(tmp, sizeof tmp, "\x1b[%dA", e->crow); for (char *p = tmp; *p; p++) sb_add(&o, *p); }
    for (const char *p = "\r\x1b[J"; *p; p++) sb_add(&o, *p);
    for (const char *p = e->prompt; *p; p++) sb_add(&o, *p);
    for (int i = 0; i < e->len; i++) sb_add(&o, e->buf[i]);
    if (e->len && total % e->cols == 0) { sb_add(&o, '\n'); sb_add(&o, '\r'); }   /* deferred wrap: force the new row */
    int erow = total / e->cols;
    if (erow > prow) { snprintf(tmp, sizeof tmp, "\x1b[%dA", erow - prow); for (char *p = tmp; *p; p++) sb_add(&o, *p); }
    sb_add(&o, '\r');
    if (pcol) { snprintf(tmp, sizeof tmp, "\x1b[%dC", pcol); for (char *p = tmp; *p; p++) sb_add(&o, *p); }
    write(1, o.p, o.n);
    free(o.p);
    e->crow = prow;
}

static void insert(struct ed *e, const char *s, int n) {
    if (e->len + n >= LMAX) return;
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
    memcpy(e->buf + e->pos, s, n);
    e->len += n; e->pos += n;
}

static void erase(struct ed *e, int from, int to) {      /* delete buf[from..to) */
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    e->pos = from;
}

static void set_line(struct ed *e, const char *s) {
    e->len = e->pos = 0;
    insert(e, s, strlen(s));
}

/* ---- Tab completion ---- */
static int cmp_s(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int is_bound(char c) { return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' || c == '<' || c == '>'; }

static void add_cand(struct vec *v, const char *name, const char *dir, const char *base) {
    if (strncmp(name, base, strlen(base))) return;
    if (name[0] == '.' && base[0] != '.') return;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    struct stat st;
    char *p = malloc(strlen(dir) + strlen(name) + 2);
    sprintf(p, "%s%s", dir, name);
    int isdir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
    free(p);
    char *c = malloc(strlen(name) + 2);
    sprintf(c, "%s%s", name, isdir ? "/" : "");
    vec_add(v, c);
}

static void esc_add(struct sb *o, const char *s) {
    for (; *s; s++) {
        if (*s != '/' && strchr(" \t\"'\\*?[]()<>|;&$#!", *s)) sb_add(o, '\\');
        sb_add(o, *s);
    }
}

static void complete(struct ed *e) {
    int s = e->pos;
    while (s > 0 && !(is_bound(e->buf[s - 1]) && !(s >= 2 && e->buf[s - 2] == '\\'))) s--;
    int q = s - 1;
    while (q >= 0 && (e->buf[q] == ' ' || e->buf[q] == '\t')) q--;
    int cmdpos = q < 0 || e->buf[q] == '|' || e->buf[q] == ';' || e->buf[q] == '&';
    char w[LMAX];                                          /* typed word, unescaped */
    int wl = 0;
    for (int i = s; i < e->pos; i++) {
        if (e->buf[i] == '\\' && i + 1 < e->pos) i++;
        else if (e->buf[i] == '\'' || e->buf[i] == '"') continue;
        w[wl++] = e->buf[i];
    }
    w[wl] = 0;
    char *slash = strrchr(w, '/');
    char dir[PATH_MAX] = "", *base = w;
    if (slash) { memcpy(dir, w, slash - w + 1); dir[slash - w + 1] = 0; base = slash + 1; }
    struct vec c = { 0 };
    if (cmdpos && !slash) {
        for (int i = 0; builtin_name(i); i++)
            if (!strncmp(builtin_name(i), base, strlen(base))) vec_add(&c, strdup(builtin_name(i)));
        const char *pa = getenv("PATH");
        if (!pa || !*pa) pa = "/bin";
        while (*pa) {
            const char *colon = strchr(pa, ':');
            char d[PATH_MAX];
            size_t dl = colon ? (size_t)(colon - pa) : strlen(pa);
            if (dl && dl < sizeof d - 2) {
                memcpy(d, pa, dl); d[dl] = '/'; d[dl + 1] = 0;
                DIR *dp = opendir(d);
                if (dp) {
                    struct dirent *de;
                    while ((de = readdir(dp))) {
                        struct vec one = { 0 };
                        add_cand(&one, de->d_name, d, base);
                        if (one.n) {
                            if (one.v[0][strlen(one.v[0]) - 1] != '/') vec_add(&c, one.v[0]); else free(one.v[0]);
                        }
                        free(one.v);
                    }
                    closedir(dp);
                }
            }
            if (!colon) break;
            pa = colon + 1;
        }
    } else {
        char ld[PATH_MAX];
        const char *home = getenv("HOME");
        if (dir[0] == '~' && (dir[1] == '/' || !dir[1]) && home) snprintf(ld, sizeof ld, "%s%s", home, dir + 1);
        else snprintf(ld, sizeof ld, "%s", dir);
        DIR *dp = opendir(ld[0] ? ld : ".");
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp))) add_cand(&c, de->d_name, ld, base);
            closedir(dp);
        }
    }
    if (!c.n) { free(c.v); return; }
    qsort(c.v, c.n, sizeof(char *), cmp_s);
    int n = 0;                                             /* dedupe (same name in several PATH dirs) */
    for (int i = 0; i < c.n; i++) {
        if (n && !strcmp(c.v[n - 1], c.v[i])) free(c.v[i]); else c.v[n++] = c.v[i];
    }
    c.n = n;
    size_t common = strlen(c.v[0]);
    for (int i = 1; i < c.n; i++) {
        size_t k = 0;
        while (k < common && c.v[i][k] == c.v[0][k]) k++;
        common = k;
    }
    int added = 0;
    if (c.n == 1 || common > strlen(base)) {
        struct sb o = { 0 };
        char *cs = strndup(c.v[0], common);
        esc_add(&o, cs);
        free(cs);
        if (c.n == 1 && c.v[0][common - 1] != '/') sb_add(&o, ' ');
        int bs = e->pos;                                   /* start of the last path component in the raw buffer */
        while (bs > s && e->buf[bs - 1] != '/') bs--;
        erase(e, bs, e->pos);
        insert(e, o.p ? o.p : "", o.n);
        free(o.p);
        added = 1;
    }
    if (!added) {                                          /* ambiguous and nothing to add: list the candidates */
        size_t mx = 0;
        for (int i = 0; i < c.n; i++) if (strlen(c.v[i]) > mx) mx = strlen(c.v[i]);
        int cols = e->cols / (mx + 2), rows;
        if (cols < 1) cols = 1;
        rows = (c.n + cols - 1) / cols;
        e->pos = e->len;
        refresh(e);
        outs("\n");
        for (int r = 0; r < rows; r++) {
            for (int k = 0; k < cols; k++) {
                int i = k * rows + r;
                if (i >= c.n) break;
                int d = c.v[i][strlen(c.v[i]) - 1] == '/';
                char pad[LMAX];
                snprintf(pad, sizeof pad, "%s%s%s%*s", d ? "\x1b[1;34m" : "", c.v[i], d ? "\x1b[0m" : "", (int)(mx + 2 - strlen(c.v[i])), "");
                outs(pad);
            }
            outs("\n");
        }
        e->crow = 0;
    }
    for (int i = 0; i < c.n; i++) free(c.v[i]);
    free(c.v);
}

/* ---- keys ---- */
enum { K_UP = 256, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_DEL, K_NONE };

static int rd(void) {
    unsigned char c;
    ssize_t n;
    while ((n = read(0, &c, 1)) < 0 && errno == EINTR) {}
    return n == 1 ? c : -1;
}

static int more(void) {
    struct pollfd p = { 0, POLLIN, 0 };
    return poll(&p, 1, 50) > 0;
}

static int getkey(void) {
    int c = rd();
    if (c != 27) return c;
    if (!more()) return 27;
    c = rd();
    if (c == '[') {
        int num = 0;
        for (;;) {
            c = rd();
            if (c < 0) return -1;
            if (isdigit(c)) { num = num * 10 + c - '0'; continue; }
            if (c == ';') { num = 0; continue; }
            break;
        }
        switch (c) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~': return num == 1 || num == 7 ? K_HOME : num == 4 || num == 8 ? K_END : num == 3 ? K_DEL : K_NONE;
        }
        return K_NONE;
    }
    if (c == 'O') { c = rd(); return c == 'H' ? K_HOME : c == 'F' ? K_END : K_NONE; }
    if (c == 127 || c == 8) return 23;                     /* Alt-Backspace = kill word */
    return K_NONE;
}

char *readline_(const char *prompt) {
    static struct ed e;
    struct termios orig, raw;
    char *res = NULL;
    int hidx = hn;
    char *saved = NULL;
    e.len = e.pos = e.crow = 0;
    e.prompt = prompt;
    e.plen = vwidth(prompt);
    e.cols = term_cols();
    tcgetattr(0, &orig);
    raw = orig;
    cfmakeraw(&raw);
    tcsetattr(0, TCSANOW, &raw);
    refresh(&e);
    for (;;) {
        int k = getkey();
        if (k < 0) break;                                  /* tty gone */
        if (k == '\n' || k == '\r') { e.pos = e.len; refresh(&e); outs("\n"); res = strndup(e.buf, e.len); break; }
        if (k == 3) { e.pos = e.len; refresh(&e); outs("^C\n"); res = strdup(""); g_intr = 1; break; }
        if (k == 4 && !e.len) { outs("exit\n"); break; }
        switch (k) {
        case 1: case K_HOME: e.pos = 0; break;
        case 5: case K_END: e.pos = e.len; break;
        case 2: case K_LEFT: if (e.pos) e.pos--; break;
        case 6: case K_RIGHT: if (e.pos < e.len) e.pos++; break;
        case 8: case 127: if (e.pos) erase(&e, e.pos - 1, e.pos); break;
        case 4: case K_DEL: if (e.pos < e.len) { int p = e.pos; erase(&e, p, p + 1); } break;
        case 11: e.len = e.pos; break;
        case 21: erase(&e, 0, e.pos); break;
        case 23: {
            int p = e.pos;
            while (p && e.buf[p - 1] == ' ') p--;
            while (p && e.buf[p - 1] != ' ') p--;
            erase(&e, p, e.pos);
            break;
        }
        case 12: outs("\x1b[H\x1b[2J"); e.crow = 0; break;
        case 9: complete(&e); break;
        case 16: case K_UP:
            if (hidx > 0) {
                if (hidx == hn) { free(saved); saved = strndup(e.buf, e.len); }
                set_line(&e, hist[--hidx]);
            }
            break;
        case 14: case K_DOWN:
            if (hidx < hn) { hidx++; set_line(&e, hidx == hn ? saved ? saved : "" : hist[hidx]); }
            break;
        default:
            if (k >= 32 && k != 127 && k < 256) { char ch = k; insert(&e, &ch, 1); }
        }
        e.cols = term_cols();
        refresh(&e);
    }
    free(saved);
    tcsetattr(0, TCSANOW, &orig);
    return res;
}
