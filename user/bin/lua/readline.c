/* readline.c - the REPL's line editor: raw-mode editing, in-memory history.
 * Same key set as /bin/sh (arrows, Home/End, ^A ^E ^K ^U ^W ^L, history). ^C abandons the line,
 * ^D on an empty line ends the session. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <sys/ioctl.h>

#define LMAX 1024
#define HMAX 200

static char *hist[HMAX];
static int hn;

void vibe_saveline(const char *line) {
    if (!*line || (hn && !strcmp(hist[hn - 1], line))) return;
    if (hn == HMAX) { free(hist[0]); memmove(hist, hist + 1, (HMAX - 1) * sizeof *hist); hn--; }
    hist[hn++] = strdup(line);
}

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
    return K_NONE;
}

struct ed {
    char buf[LMAX];
    int len, pos, plen, cols, crow;
    const char *prompt;
};

static int vwidth(const char *s) {                  /* printed width: skips CSI sequences */
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

static int term_cols(void) {
    struct winsize ws;
    return ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 ? ws.ws_col : 80;
}

/* redraw prompt + line and put the cursor at pos; handles lines wrapping over several rows */
static void refresh(struct ed *e) {
    char out[LMAX + 256], tmp[24];
    int o = 0, total = e->plen + e->len, prow = (e->plen + e->pos) / e->cols, pcol = (e->plen + e->pos) % e->cols;
    if (e->crow) o += snprintf(out + o, 24, "\x1b[%dA", e->crow);
    o += snprintf(out + o, 16, "\r\x1b[J");
    o += snprintf(out + o, 200, "%s", e->prompt);
    memcpy(out + o, e->buf, e->len);
    o += e->len;
    if (e->len && total % e->cols == 0) { out[o++] = '\n'; out[o++] = '\r'; }   /* deferred wrap: force the new row */
    int erow = total / e->cols;
    if (erow > prow) { snprintf(tmp, sizeof tmp, "\x1b[%dA", erow - prow); o += snprintf(out + o, 24, "%s", tmp); }
    out[o++] = '\r';
    if (pcol) o += snprintf(out + o, 24, "\x1b[%dC", pcol);
    write(1, out, o);
    e->crow = prow;
}

static void insert(struct ed *e, const char *s, int n) {
    if (e->len + n >= LMAX) return;
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
    memcpy(e->buf + e->pos, s, n);
    e->len += n;
    e->pos += n;
}

static void erase(struct ed *e, int from, int to) {     /* delete buf[from..to) */
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    e->pos = from;
}

static void set_line(struct ed *e, const char *s) {
    e->len = e->pos = 0;
    insert(e, s, strlen(s));
}

/* malloc'd line without the newline, "" after ^C, NULL on ^D / tty gone */
char *vibe_readline(const char *prompt) {
    static struct ed e;
    struct termios orig, raw;
    char *res = NULL, *saved = NULL;
    int hidx = hn;
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
        if (k < 0) break;
        if (k == '\n' || k == '\r') { e.pos = e.len; refresh(&e); write(1, "\n", 1); res = strndup(e.buf, e.len); break; }
        if (k == 3) { e.pos = e.len; refresh(&e); write(1, "^C\n", 3); res = strdup(""); break; }
        if (k == 4 && !e.len) { write(1, "\n", 1); break; }
        switch (k) {
        case 1: case K_HOME: e.pos = 0; break;
        case 5: case K_END: e.pos = e.len; break;
        case 2: case K_LEFT: if (e.pos) e.pos--; break;
        case 6: case K_RIGHT: if (e.pos < e.len) e.pos++; break;
        case 8: case 127: if (e.pos) erase(&e, e.pos - 1, e.pos); break;
        case 4: case K_DEL: if (e.pos < e.len) erase(&e, e.pos, e.pos + 1); break;
        case 11: e.len = e.pos; break;
        case 21: erase(&e, 0, e.pos); break;
        case 23: {
            int p = e.pos;
            while (p && e.buf[p - 1] == ' ') p--;
            while (p && e.buf[p - 1] != ' ') p--;
            erase(&e, p, e.pos);
            break;
        }
        case 12: write(1, "\x1b[H\x1b[2J", 7); e.crow = 0; break;
        case 9: insert(&e, "  ", 2); break;
        case 16: case K_UP:
            if (hidx > 0) {
                if (hidx == hn) { free(saved); saved = strndup(e.buf, e.len); }
                set_line(&e, hist[--hidx]);
            }
            break;
        case 14: case K_DOWN:
            if (hidx < hn) { hidx++; set_line(&e, hidx == hn ? (saved ? saved : "") : hist[hidx]); }
            break;
        default:
            if (k >= 32 && k < 256) { char ch = k; insert(&e, &ch, 1); }
        }
        e.cols = term_cols();
        refresh(&e);
    }
    free(saved);
    tcsetattr(0, TCSANOW, &orig);
    return res;
}
