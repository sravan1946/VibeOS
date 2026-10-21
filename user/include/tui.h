/* tui.h - raw-mode keys, screen size and an output buffer for the full-screen tools (less top edit) */
#pragma once
#include <vutil.h>
#include <stdarg.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>

#define TK_TIMEOUT (-1)
#define TK_EOF     (-2)
#define TK_DEL     0x180            /* ESC [ 3 ~  (abi.h's KEY_DELETE is 127, which the tty never sends) */
#define TK_ALT     0x1000           /* | char */
#define CTRL(c)    ((c) & 0x1f)

static struct termios tui_saved;
static int tui_active, tui_kfd;

static inline void tui_leave(void) {
    if (!tui_active) return;
    tui_active = 0;
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    tcsetattr(tui_kfd, TCSANOW, &tui_saved);
}

/* alternate screen + raw (no echo, no line editing, no ^C signal: tools handle ^C themselves) */
static inline void tui_enter(int keyfd) {
    struct termios t;
    tui_kfd = keyfd;
    tcgetattr(keyfd, &tui_saved);
    t = tui_saved;
    cfmakeraw(&t);
    tcsetattr(keyfd, TCSANOW, &t);
    tui_active = 1;
    fputs("\x1b[?1049h\x1b[2J", stdout);
    fflush(stdout);
}

static inline void tui_size(int fd, int *rows, int *cols) {
    struct winsize ws = { 0 };
    if (ioctl(fd, TIOCGWINSZ, &ws) < 0 || !ws.ws_row || !ws.ws_col) { *rows = 24; *cols = 80; return; }
    *rows = ws.ws_row; *cols = ws.ws_col;
}

static inline int tui_getc(int fd, int timeout_ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0) return TK_EOF;
    if (r == 0) return TK_TIMEOUT;
    unsigned char c;
    return read(fd, &c, 1) == 1 ? c : TK_EOF;
}

/* one key: ASCII (ctrl = 1..26, backspace folded to 8, Enter to 10), KEY_* from abi.h, TK_DEL, TK_ALT|c,
 * TK_TIMEOUT when nothing arrived in timeout_ms (-1 = block), TK_EOF on error */
static inline int tui_key(int fd, int timeout_ms) {
    int c = tui_getc(fd, timeout_ms);
    if (c == 127) return 8;
    if (c == 13) return 10;
    if (c != 27) return c;
    int n = tui_getc(fd, 40);
    if (n < 0) return 27;
    if (n == 'O') {
        int f = tui_getc(fd, 40);
        return f >= 'P' && f <= 'S' ? KEY_F1 + (f - 'P') : f == 'H' ? KEY_HOME : f == 'F' ? KEY_END : 27;
    }
    if (n != '[') return TK_ALT | n;
    int num = 0, f;
    while ((f = tui_getc(fd, 40)) >= '0' && f <= ';') if (f != ';') num = num * 10 + f - '0'; else num = 0;
    switch (f) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch (num) {
        case 1: case 7: return KEY_HOME;
        case 2: return KEY_INSERT;
        case 3: return TK_DEL;
        case 4: case 8: return KEY_END;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        }
    }
    return 0;                         /* unknown sequence: swallow */
}

/* growable output buffer: build a whole frame, then one write() */
struct sb { char *p; size_t n, cap; };

static inline void sb_put(struct sb *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fputs("out of memory\n", stderr); _exit(1); }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static inline void sb_puts(struct sb *b, const char *s) { sb_put(b, s, strlen(s)); }
static inline void sb_printf(struct sb *b, const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    sb_put(b, t, n < (int)sizeof t ? n : (int)sizeof t - 1);
}
static inline void sb_flush(struct sb *b) { write_all(1, b->p, b->n); b->n = 0; }
