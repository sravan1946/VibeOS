/* termwin.c - terminal windows: window + terminal emulator + tty + shell */
#include "wm.h"
#include "tty.h"
#include "term.h"

#define PAD 8
struct termwin {
    struct window *w;
    struct term *t;
    struct tty *tty;
    int pid;
    bool dirty, full, closing, blink_on, hold, selecting;
    int sc, sr;                       /* selection anchor cell */
    uint64_t blink_at;
    char title[64];
};

static void tw_out(void *ctx, const char *s, int n) {
    struct termwin *tw = ctx;
    term_write(tw->t, s, n);
    term_scroll_view(tw->t, 0);
    tw->dirty = true;
    tw->blink_on = true;
    tw->blink_at = ticks_ms + 530;
    wm_wake();
}

static void tw_reply(void *ctx, const char *s, int n) {
    struct termwin *tw = ctx;
    if (tw->tty) tty_input(tw->tty, s, n);
}

static void tw_redraw_all(struct termwin *tw) {
    gfx_fill(&tw->w->canvas, 0, 0, tw->w->w, tw->w->h, TERM_DEFAULT_BG);
    tw->full = tw->dirty = true;
}

static void tw_frame(struct window *w) {
    struct termwin *tw = w->priv;
    if (tw->pid > 0) {
        struct task *t = task_find(tw->pid);
        if (!t || t->state == PS_ZOMBIE) {
            char msg[64];
            snprintf(msg, sizeof msg, "\r\n\x1b[2m[process exited with %d]\x1b[0m", t ? t->exit_code : 0);
            tw->pid = 0;
            if (tw->hold) tw_out(tw, msg, strlen(msg));
            else tw->closing = true;
        }
    }
    if (tw->closing && tw->pid <= 0) {
        if (tw->tty) { tty_hangup(tw->tty); tw->tty = 0; }
        wm_destroy(w);
        return;
    }
    if (ticks_ms >= tw->blink_at) {
        tw->blink_on = !tw->blink_on;
        tw->blink_at = ticks_ms + 530;
        term_set_cursor_blink_phase(tw->t, tw->blink_on);
        tw->dirty = true;
    }
    if (!tw->dirty) return;
    tw->dirty = false;
    struct rect d;
    term_render(tw->t, &w->canvas, PAD, PAD - 2, tw->full, &d);
    tw->full = false;
    if (d.w) wm_damage_client(w, d.x, d.y, d.w, d.h);
    const char *t = term_title(tw->t);
    if (t && *t && strcmp(t, tw->title)) { strlcpy(tw->title, t, sizeof tw->title); wm_set_title(w, t); }
    if (term_take_bell(tw->t)) {}
}

static void tw_copy(struct termwin *tw) {
    char *buf = kmalloc(64 * 1024);
    int n = buf ? term_selection_text(tw->t, buf, 64 * 1024) : 0;
    if (n) clipboard_set(buf, n);
    kfree(buf);
}

static void tw_paste(struct termwin *tw) {
    char *buf = kmalloc(64 * 1024);
    int n = buf && tw->tty ? clipboard_get(buf, 64 * 1024) : 0;
    for (int i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\r';
    if (n > 0) tty_input(tw->tty, buf, n);
    kfree(buf);
}

static void tw_event(struct window *w, const struct gui_event *ev) {
    struct termwin *tw = w->priv;
    int col = (ev->x - PAD) / term_cell_w(), row = (ev->y - PAD + 2) / term_cell_h();
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->button == 4) { tw_paste(tw); break; }
        if (ev->button != 1) break;
        term_select_clear(tw->t);
        tw->selecting = true; tw->sc = col; tw->sr = row;
        tw->dirty = true;
        break;
    case EV_MOUSE_MOVE:
        if (!tw->selecting || !(ev->button & 1) || (col == tw->sc && row == tw->sr)) break;
        term_select(tw->t, tw->sc, tw->sr, col, row);
        tw->dirty = true;
        break;
    case EV_MOUSE_UP:
        if (ev->button == 1 && tw->selecting) { tw->selecting = false; tw_copy(tw); }
        break;
    case EV_KEY:
        if (!ev->pressed) return;
        if ((ev->mods & (MOD_CTRL | MOD_SHIFT)) == (MOD_CTRL | MOD_SHIFT) && ev->key < 256) {
            if ((ev->key | 0x20) == 'c') { tw_copy(tw); return; }
            if ((ev->key | 0x20) == 'v') { tw_paste(tw); return; }
        }
        if ((ev->mods & MOD_SHIFT) && (ev->key == KEY_PGUP || ev->key == KEY_PGDN)) {
            term_scroll_view(tw->t, ev->key == KEY_PGUP ? term_rows(tw->t) / 2 : -term_rows(tw->t) / 2);
            tw->dirty = tw->full = true;
            return;
        }
        if (tw->tty) tty_key(tw->tty, ev);
        break;
    case EV_SCROLL:
        term_scroll_view(tw->t, -ev->dz * 3);
        tw->dirty = tw->full = true;
        break;
    case EV_RESIZE: {
        int cols, rows;
        term_size_for_pixels(ev->w - 2 * PAD, ev->h - 2 * PAD + 4, &cols, &rows);
        term_resize(tw->t, cols, rows);
        if (tw->tty) tty_set_winsize(tw->tty, cols, rows, ev->w, ev->h);
        tw_redraw_all(tw);
        break;
    }
    case EV_FOCUS:
        term_set_focused(tw->t, ev->pressed);
        tw->dirty = true;
        break;
    case EV_CLOSE:
        tw->closing = true;
        if (tw->tty) { tty_hangup(tw->tty); tw->tty = 0; }
        break;
    }
}

static void tw_destroy(struct window *w) {
    struct termwin *tw = w->priv;
    if (tw->tty) tty_hangup(tw->tty);
    term_destroy(tw->t);
    kfree(tw);
}

/* argv == NULL: interactive /bin/sh; hold: keep the window after the program exits */
struct window *termwin_open_argv(char *const argv_in[], bool hold) {
    int cols = 96, rows = 28;
    int pw = cols * term_cell_w() + 2 * PAD, ph = rows * term_cell_h() + 2 * PAD - 4;
    struct termwin *tw = kzalloc(sizeof *tw);
    if (!tw) return 0;
    tw->t = term_create(cols, rows);
    struct window *w = tw->t ? wm_create("Terminal", -1, -1, pw, ph, 0) : 0;
    if (!w) { if (tw->t) term_destroy(tw->t); kfree(tw); return 0; }
    tw->w = w;
    w->priv = tw;
    w->on_event = tw_event;
    w->on_frame = tw_frame;
    w->on_destroy = tw_destroy;
    term_set_reply(tw->t, tw_reply, tw);
    term_set_focused(tw->t, wm_focused() == w);
    tw->tty = tty_create(tw_out, tw, cols, rows);
    tty_set_winsize(tw->tty, cols, rows, pw, ph);
    tw_redraw_all(tw);

    tw->hold = hold;
    struct file *f = vfs_open_vnode(&tw->tty->vn, O_RDWR);
    struct file *fds[3] = { f, f, f };
    char *shargv[] = { "/bin/sh", 0 };
    char *const *argv = argv_in ? argv_in : shargv;
    char *envp[] = { "PATH=/bin", "HOME=/home", "TERM=xterm-256color", "USER=vibe", 0 };
    tw->pid = proc_spawn_ex(argv[0], argv, envp, "/home", fds, tw->tty);
    if (f) vfs_close(f);
    if (tw->pid < 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "\x1b[31mcan't start %s (%d)\x1b[0m\r\n", argv[0], tw->pid);
        term_write(tw->t, msg, strlen(msg));
        tw->pid = 0;
    }
    return w;
}

struct window *termwin_open(const char *cmd) {
    if (!cmd) return termwin_open_argv(0, false);
    char *argv[] = { "/bin/sh", "-c", (char *)cmd, 0 };
    return termwin_open_argv(argv, false);
}
