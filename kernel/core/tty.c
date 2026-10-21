/* tty.c - canonical/raw line discipline */
#include "tty.h"
#include "proc.h"

static inline uint32_t in_count(struct tty *t) { return t->in_head - t->in_tail; }

static void in_put(struct tty *t, char c) {
    if (in_count(t) < TTY_BUF) t->in[t->in_head++ % TTY_BUF] = c;
}

static void echo(struct tty *t, const char *s, int n) {
    if (t->lflag & ECHO) tty_output(t, s, n);
}

void tty_output(struct tty *t, const char *s, int n) {
    if (t->dead || !t->out) return;
    int start = 0;
    for (int i = 0; i < n; i++)
        if (s[i] == '\n') {                       /* ONLCR */
            if (i > start) t->out(t->ctx, s + start, i - start);
            t->out(t->ctx, "\r\n", 2);
            start = i + 1;
        }
    if (n > start) t->out(t->ctx, s + start, n - start);
}

void tty_input(struct tty *t, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if ((t->lflag & ISIG) && c == 3) {        /* ^C */
            echo(t, "^C\n", 3);
            t->line_len = 0;
            if (t->fg_pid > 0) proc_kill(t->fg_pid, SIGINT);
            continue;
        }
        if (!(t->lflag & ICANON)) { in_put(t, c); continue; }
        if (c == '\r') c = '\n';
        if (c == 8 || c == 127) {                 /* backspace */
            if (t->line_len) { t->line_len--; echo(t, "\b \b", 3); }
            continue;
        }
        if (c == 21) {                            /* ^U kill line */
            while (t->line_len) { t->line_len--; echo(t, "\b \b", 3); }
            continue;
        }
        if (c == 4) {                             /* ^D */
            if (!t->line_len) t->eof = 1;
            for (int k = 0; k < t->line_len; k++) in_put(t, t->line[k]);
            t->line_len = 0;
            continue;
        }
        if (c == 27) continue;                    /* escape sequences are noise when cooked */
        if (c == '\n' || t->line_len < TTY_LINE - 1) {
            if (c != '\n') { t->line[t->line_len++] = c; echo(t, &c, 1); continue; }
            echo(t, "\n", 1);
            for (int k = 0; k < t->line_len; k++) in_put(t, t->line[k]);
            in_put(t, '\n');
            t->line_len = 0;
        }
    }
    wakeup(t);
}

void tty_key(struct tty *t, const struct gui_event *ev) {
    if (!ev->pressed) return;
    const char *seq = 0;
    switch (ev->key) {
    case KEY_UP: seq = "\x1b[A"; break;
    case KEY_DOWN: seq = "\x1b[B"; break;
    case KEY_RIGHT: seq = "\x1b[C"; break;
    case KEY_LEFT: seq = "\x1b[D"; break;
    case KEY_HOME: seq = "\x1b[H"; break;
    case KEY_END: seq = "\x1b[F"; break;
    case KEY_PGUP: seq = "\x1b[5~"; break;
    case KEY_PGDN: seq = "\x1b[6~"; break;
    case KEY_INSERT: seq = "\x1b[2~"; break;
    case KEY_DELETE: seq = "\x1b[3~"; break;
    case KEY_F1: seq = "\x1bOP"; break;
    case KEY_F2: seq = "\x1bOQ"; break;
    case KEY_F3: seq = "\x1bOR"; break;
    case KEY_F4: seq = "\x1bOS"; break;
    }
    if (seq) { tty_input(t, seq, strlen(seq)); return; }
    if (ev->ch) {
        char c = ev->ch;
        if (ev->mods & MOD_ALT) { char e[2] = { 27, c }; tty_input(t, e, 2); }
        else tty_input(t, &c, 1);
    }
}

/* ---- vnode ops ---- */
static int tty_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) {
    struct tty *t = v->priv;
    char *b = buf;
    uint32_t f = irq_save();
    for (;;) {
        if (t->dead) { irq_restore(f); return 0; }
        if (in_count(t)) break;
        if (t->eof) { t->eof = 0; irq_restore(f); return 0; }
        if (sleep_on(t, 0) < 0) { irq_restore(f); return -EINTR; }
    }
    uint32_t got = 0;
    while (got < n && in_count(t)) {
        char c = t->in[t->in_tail++ % TTY_BUF];
        b[got++] = c;
        if ((t->lflag & ICANON) && c == '\n') break;
    }
    irq_restore(f);
    return got;
}

static int tty_write(struct vnode *v, uint32_t off, const void *buf, uint32_t n) {
    struct tty *t = v->priv;
    if (t->dead) return -EIO;
    tty_output(t, buf, n);
    return n;
}

static int tty_ioctl(struct vnode *v, int req, void *arg) {
    struct tty *t = v->priv;
    switch (req) {
    case TCGETS: ((struct termios *)arg)->c_lflag = t->lflag; return 0;
    case TCSETS:
        t->lflag = ((struct termios *)arg)->c_lflag;
        if (!(t->lflag & ICANON) && t->line_len) {     /* flush pending edit */
            for (int k = 0; k < t->line_len; k++) in_put(t, t->line[k]);
            t->line_len = 0;
        }
        return 0;
    case TIOCGWINSZ: *(struct winsize *)arg = t->ws; return 0;
    case TIOCSPGRP: t->fg_pid = *(int *)arg; return 0;
    case FIONREAD: *(int *)arg = in_count(t); return 0;
    }
    return -ENOTTY;
}

static int tty_poll(struct vnode *v, int events) {
    struct tty *t = v->priv;
    int r = POLLOUT;
    if (in_count(t) || t->eof || t->dead) r |= POLLIN;
    if (t->dead) r |= POLLHUP;
    return r & (events | POLLHUP);
}

static void tty_release(struct vnode *v) {
    struct tty *t = v->priv;
    if (t->dead) kfree(t);         /* last reference after hangup */
}

static const struct vnode_ops tty_ops = {
    .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .poll = tty_poll, .release = tty_release,
};

struct tty *tty_create(void (*out)(void *ctx, const char *s, int n), void *ctx, int cols, int rows) {
    struct tty *t = kzalloc(sizeof *t);
    if (!t) return 0;
    t->vn.mode = S_IFCHR | 0666;
    t->vn.ops = &tty_ops;
    t->vn.priv = t;
    t->vn.refs = 1;                /* held by the terminal until hangup */
    t->lflag = ECHO | ICANON | ISIG;
    t->out = out;
    t->ctx = ctx;
    t->ws.ws_col = cols;
    t->ws.ws_row = rows;
    return t;
}

void tty_set_winsize(struct tty *t, int cols, int rows, int pw, int ph) {
    t->ws = (struct winsize){ rows, cols, pw, ph };
}

void tty_hangup(struct tty *t) {
    t->dead = 1;
    t->out = 0;
    for (struct task *p = task_list(); p; p = p->next)
        if (p->tty == t && p->user) { proc_kill(p->pid, SIGKILL); p->tty = 0; }
    wakeup(t);
    vnode_put(&t->vn);
}

void tty_proc_exited(struct tty *t, int pid) {
    if (t->fg_pid == pid) t->fg_pid = 0;
}
