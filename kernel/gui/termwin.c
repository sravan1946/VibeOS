/* termwin.c - terminal windows: window + tabs, each tab a terminal emulator + tty + shell.
 * Ctrl+Shift+T/W new/close tab, Ctrl+PgUp/PgDn (or Ctrl+Tab) switch, Ctrl+Shift+F find, Ctrl+=/-/0 zoom,
 * Ctrl+Shift+C/V copy/paste, Ctrl+click on a URL opens /bin/browser. */
#include "wm.h"
#include "tty.h"
#include "term.h"
#include "font.h"

#define PAD 8
#define TAB_H 28                  /* tab bar height */
#define MAXTABS 8
#define FIND_W 340
#define FIND_H 30

struct termwin;
struct tab {
    struct termwin *tw;
    struct term *t;
    struct tty *tty;
    int pid;
    bool hold, activity;
    char title[64];
    struct tab *next;             /* closed tabs wait on tw->dying until their processes are gone */
};

struct termwin {
    struct window *w;
    struct tab *tabs[MAXTABS];
    int n, cur, zoom;
    struct tab *dying;
    int pw, ph;                   /* client size */
    bool shell;                   /* opened as an interactive shell: the tab bar is always shown */
    bool dirty, full, closing, blink_on, selecting, bar_dirty, focused;
    int sc, sr;                   /* selection anchor cell */
    uint64_t blink_at;
    char title[64];
    bool find;
    char q[64];
};

static const color_t C_BAR = RGB(0x16, 0x18, 0x24), C_TAB = RGB(0x24, 0x28, 0x3B), C_ACCENT = RGB(0x7A, 0xA2, 0xF7);

static struct tab *cur_tab(struct termwin *tw) { return tw->tabs[tw->cur]; }
static int bar_h(struct termwin *tw) { return tw->n > 1 || tw->shell ? TAB_H : 0; }
static int term_y(struct termwin *tw) { return PAD - 2 + bar_h(tw); }

static void tab_out(void *ctx, const char *s, int n) {
    struct tab *tb = ctx;
    struct termwin *tw = tb->tw;
    term_write(tb->t, s, n);
    if (tb == cur_tab(tw)) {
        tw->dirty = true;
        tw->blink_on = true;
        tw->blink_at = ticks_ms + 530;
    } else if (!tb->activity) { tb->activity = true; tw->bar_dirty = true; }
    wm_wake();
}

static void tab_reply(void *ctx, const char *s, int n) {
    struct tab *tb = ctx;
    if (tb->tty) tty_input(tb->tty, s, n);
}

static void tw_redraw_all(struct termwin *tw) {
    gfx_fill(&tw->w->canvas, 0, 0, tw->w->w, tw->w->h, TERM_DEFAULT_BG);
    wm_damage_client(tw->w, 0, 0, tw->w->w, tw->w->h);
    tw->full = tw->dirty = tw->bar_dirty = true;
}

/* new terminal + tty for a tab; the shell is started separately */
static struct tab *tab_make(struct termwin *tw, int cols, int rows) {
    struct tab *tb = kzalloc(sizeof *tb);
    if (!tb) return 0;
    tb->tw = tw;
    tb->t = term_create(cols, rows);
    if (!tb->t) { kfree(tb); return 0; }
    term_set_zoom(tb->t, tw->zoom);
    term_set_focused(tb->t, tw->focused);
    term_set_reply(tb->t, tab_reply, tb);
    tb->tty = tty_create(tab_out, tb, cols, rows);
    if (!tb->tty) { term_destroy(tb->t); kfree(tb); return 0; }
    tty_set_winsize(tb->tty, cols, rows, tw->pw, tw->ph);
    return tb;
}

static void tab_spawn(struct tab *tb, char *const argv_in[], bool hold) {
    struct file *f = vfs_open_vnode(&tb->tty->vn, O_RDWR);
    struct file *fds[3] = { f, f, f };
    char *shargv[] = { "/bin/sh", 0 };
    char *const *argv = argv_in ? argv_in : shargv;
    char *envp[] = { "PATH=/bin", "HOME=/home", "TERM=xterm-256color", "USER=vibe", 0 };
    tb->hold = hold;
    tb->pid = proc_spawn_ex(argv[0], argv, envp, "/home", fds, tb->tty);
    if (f) vfs_close(f);
    if (tb->pid < 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "\x1b[31mcan't start %s (%d)\x1b[0m\r\n", argv[0], tb->pid);
        term_write(tb->t, msg, strlen(msg));
        tb->pid = 0;
    }
}

static bool tab_gone(struct tab *tb) {
    if (tb->pid <= 0) return true;
    struct task *t = task_find(tb->pid);
    return !t || t->state == PS_ZOMBIE;
}

static void tab_free(struct tab *tb) {
    if (tb->tty) tty_hangup(tb->tty);
    term_destroy(tb->t);
    kfree(tb);
}

/* grid for the current window size and tab bar */
static void tw_grid(struct termwin *tw, int *cols, int *rows) {
    term_size_for_pixels(cur_tab(tw)->t, tw->pw - 2 * PAD, tw->ph - 2 * PAD + 4 - bar_h(tw), cols, rows);
}

/* after a change of window size, zoom or tab bar: refit every tab */
static void tw_relayout(struct termwin *tw) {
    int cols, rows;
    tw_grid(tw, &cols, &rows);
    for (int i = 0; i < tw->n; i++) {
        struct tab *tb = tw->tabs[i];
        term_resize(tb->t, cols, rows);
        term_set_hover(tb->t, -1, 0, 0);
        if (tb->tty) tty_set_winsize(tb->tty, cols, rows, tw->pw, tw->ph);
    }
    tw_redraw_all(tw);
}

static void tw_find_apply(struct termwin *tw) {
    term_search_set(cur_tab(tw)->t, tw->find ? tw->q : "");
    if (tw->find && tw->q[0]) term_search_step(cur_tab(tw)->t, 0);
    tw->dirty = true;
}

static void tw_switch(struct termwin *tw, int i) {
    if (i < 0 || i >= tw->n || i == tw->cur) return;
    if (tw->find) term_search_set(cur_tab(tw)->t, "");
    term_set_focused(cur_tab(tw)->t, false);
    term_set_hover(cur_tab(tw)->t, -1, 0, 0);
    tw->cur = i;
    cur_tab(tw)->activity = false;
    term_set_focused(cur_tab(tw)->t, tw->focused);
    tw->selecting = false;
    if (tw->find) tw_find_apply(tw);
    tw_redraw_all(tw);
}

static void tw_new_tab(struct termwin *tw, char *const argv[], bool hold) {
    if (tw->n >= MAXTABS) return;
    bool had_bar = bar_h(tw) != 0;
    int cols, rows;
    tw_grid(tw, &cols, &rows);
    struct tab *tb = tab_make(tw, cols, rows);
    if (!tb) return;
    tw->tabs[tw->n++] = tb;
    tab_spawn(tb, argv, hold);
    tw_switch(tw, tw->n - 1);
    if (!had_bar) tw_relayout(tw);
    tw->bar_dirty = true;
}

static void tw_close_tab(struct termwin *tw, int i) {
    struct tab *tb = tw->tabs[i];
    if (tb->tty) { tty_hangup(tb->tty); tb->tty = 0; }
    if (tw->find && i == tw->cur) term_search_set(tb->t, "");
    bool had_bar = bar_h(tw) != 0;
    tb->next = tw->dying; tw->dying = tb;                      /* freed once its processes have exited */
    for (int k = i; k < tw->n - 1; k++) tw->tabs[k] = tw->tabs[k + 1];
    tw->tabs[--tw->n] = 0;
    if (tw->n == 0) { tw->closing = true; return; }
    if (tw->cur > i || tw->cur >= tw->n) tw->cur--;
    if (tw->cur < 0) tw->cur = 0;
    cur_tab(tw)->activity = false;
    term_set_focused(cur_tab(tw)->t, tw->focused);
    tw->selecting = false;
    if (tw->find) tw_find_apply(tw);
    if (had_bar != (bar_h(tw) != 0)) tw_relayout(tw); else tw_redraw_all(tw);
}

static void tw_zoom(struct termwin *tw, int level) {
    level = level < 0 ? 0 : level >= TERM_ZOOMS ? TERM_ZOOMS - 1 : level;
    if (level == tw->zoom) return;
    tw->zoom = level;
    for (int i = 0; i < tw->n; i++) term_set_zoom(tw->tabs[i]->t, level);
    tw_relayout(tw);
}

/* ---- drawing: tab bar and find box ---- */

static void tab_rect(struct termwin *tw, int i, int *x, int *y, int *w, int *h, int *tabw) {
    int aw = (tw->pw - 8 - 30) / tw->n;
    *tabw = aw > 190 ? 190 : aw < 60 ? 60 : aw;
    *x = 4 + i * *tabw; *y = 4; *w = *tabw - 3; *h = TAB_H - 4;
}

static void draw_bar(struct termwin *tw) {
    struct canvas *c = &tw->w->canvas;
    gfx_fill(c, 0, 0, tw->pw, TAB_H, C_BAR);
    for (int i = 0; i < tw->n; i++) {
        int x, y, w, h, tabw;
        tab_rect(tw, i, &x, &y, &w, &h, &tabw);
        struct tab *tb = tw->tabs[i];
        bool on = i == tw->cur;
        gfx_round_fill(c, x, y, w, h, 6, on ? C_TAB : RGB(0x1D, 0x20, 0x31));
        if (on) gfx_fill(c, x + 8, y, w - 16, 2, C_ACCENT);
        const char *name = term_title(tb->t);
        if (!*name) name = "Terminal";
        int n = strlen(name), maxw = w - 40;
        bool cut = false;
        while (n > 0 && gfx_text_width_n(&font_ui, name, n) > maxw) { n--; cut = true; }
        int tx = x + 10;
        if (tb->activity && !on) { gfx_circle_fill(c, tx + 3, y + h / 2, 3, C_ACCENT); tx += 12; }
        color_t fg = on ? RGB(0xE6, 0xEA, 0xFA) : RGB(0x8D, 0x93, 0xB5);
        int tw_ = gfx_text_n(c, &font_ui, tx, y + (h - font_ui.height) / 2, name, n, fg);
        if (cut) gfx_text(c, &font_ui, tx + tw_, y + (h - font_ui.height) / 2, "\xe2\x80\xa6", fg);
        gfx_text_center(c, &font_ui, x + w - 22, y + (h - font_ui.height) / 2, 16, "\xc3\x97", on ? fg : RGB(0x6A, 0x70, 0x90));
    }
    if (tw->n < MAXTABS) {
        int x, y, w, h, tabw;
        tab_rect(tw, tw->n, &x, &y, &w, &h, &tabw);
        gfx_text_center(c, &font_ui, x, y + (h - font_ui.height) / 2, 22, "+", RGB(0x8D, 0x93, 0xB5));
    }
    wm_damage_client(tw->w, 0, 0, tw->pw, TAB_H);
}

static void draw_find(struct termwin *tw) {
    struct canvas *c = &tw->w->canvas;
    int x = tw->pw - FIND_W - 14, y = bar_h(tw) + 6;
    gfx_round_fill(c, x, y, FIND_W, FIND_H, 7, RGB(0x20, 0x24, 0x38));
    gfx_round_rect(c, x, y, FIND_W, FIND_H, 7, C_ACCENT);
    int cur, total;
    term_search_info(cur_tab(tw)->t, &cur, &total);
    char cnt[24];
    if (!tw->q[0]) cnt[0] = 0;
    else if (!total) strlcpy(cnt, "no matches", sizeof cnt);
    else snprintf(cnt, sizeof cnt, "%d/%d", cur, total);
    int cw = gfx_text_width(&font_ui, cnt), ty = y + (FIND_H - font_ui.height) / 2;
    gfx_text(c, &font_ui, x + FIND_W - 10 - cw, ty, cnt, !tw->q[0] || total ? RGB(0x8D, 0x93, 0xB5) : RGB(0xF7, 0x76, 0x8E));
    gfx_text(c, &font_ui, x + 10, ty, "Find", RGB(0x6A, 0x70, 0x90));
    int qx = x + 10 + gfx_text_width(&font_ui, "Find") + 10, qmax = x + FIND_W - 20 - cw - qx;
    const char *q = tw->q;
    while (*q && gfx_text_width(&font_ui, q) > qmax - 6) q++;      /* keep the end of a long query visible */
    int w = gfx_text(c, &font_ui, qx, ty, q, RGB(0xE6, 0xEA, 0xFA));
    if (tw->blink_on) gfx_fill(c, qx + w + 1, ty + 2, 1, font_ui.height - 4, RGB(0xE6, 0xEA, 0xFA));
    wm_damage_client(tw->w, x, y, FIND_W, FIND_H);
}

static void tw_frame(struct window *w) {
    struct termwin *tw = w->priv;
    for (int i = 0; i < tw->n; i++) {
        struct tab *tb = tw->tabs[i];
        if (tb->pid > 0 && tab_gone(tb)) {
            struct task *t = task_find(tb->pid);
            char msg[64];
            snprintf(msg, sizeof msg, "\r\n\x1b[2m[process exited with %d]\x1b[0m", t ? t->exit_code : 0);
            tb->pid = 0;
            if (tb->hold) tab_out(tb, msg, strlen(msg));
            else { tw_close_tab(tw, i--); if (tw->closing) break; }
        }
    }
    for (struct tab **pp = &tw->dying; *pp; ) {                  /* closed tabs: free once their processes are gone */
        struct tab *tb = *pp;
        if (tab_gone(tb)) { *pp = tb->next; tab_free(tb); }
        else pp = &tb->next;
    }
    if (tw->closing && !tw->dying) { wm_destroy(w); return; }
    if (tw->closing) return;
    struct tab *ct = cur_tab(tw);
    if (ticks_ms >= tw->blink_at) {
        tw->blink_on = !tw->blink_on;
        tw->blink_at = ticks_ms + 530;
        term_set_cursor_blink_phase(ct->t, tw->blink_on);
        tw->dirty = true;
    }
    /* tab titles come from the programs (OSC 0/2) */
    for (int i = 0; i < tw->n; i++) {
        const char *s = term_title(tw->tabs[i]->t);
        if (strcmp(s, tw->tabs[i]->title)) { strlcpy(tw->tabs[i]->title, s, sizeof tw->tabs[i]->title); tw->bar_dirty = true; }
    }
    const char *title = term_title(ct->t);
    if (*title && strcmp(title, tw->title)) { strlcpy(tw->title, title, sizeof tw->title); wm_set_title(w, title); }
    if (tw->bar_dirty && bar_h(tw)) draw_bar(tw);
    tw->bar_dirty = false;
    if (!tw->dirty) return;
    tw->dirty = false;
    struct rect d;
    term_render(ct->t, &w->canvas, PAD, term_y(tw), tw->full, &d);
    tw->full = false;
    if (d.w) wm_damage_client(w, d.x, d.y, d.w, d.h);
    if (tw->find) draw_find(tw);
    if (term_take_bell(ct->t)) {}
    for (int i = 0; i < tw->n; i++) if (i != tw->cur) term_take_bell(tw->tabs[i]->t);
}

/* ---- input ---- */

static void tw_copy(struct termwin *tw) {
    char *buf = kmalloc(64 * 1024);
    int n = buf ? term_selection_text(cur_tab(tw)->t, buf, 64 * 1024) : 0;
    if (n) clipboard_set(buf, n);
    kfree(buf);
}

static void tw_paste(struct termwin *tw) {
    struct tab *tb = cur_tab(tw);
    char *buf = kmalloc(64 * 1024);
    int n = buf && tb->tty ? clipboard_get(buf, 64 * 1024) : 0;
    for (int i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\r';
    if (n > 0) tty_input(tb->tty, buf, n);
    kfree(buf);
}

static void tw_find_toggle(struct termwin *tw) {
    tw->find = !tw->find;
    if (tw->find) { term_select_clear(cur_tab(tw)->t); tw_find_apply(tw); }
    else { term_search_set(cur_tab(tw)->t, ""); tw_redraw_all(tw); }
    tw->blink_on = true;
    tw->dirty = true;
}

/* keys while the find box is open; returns true if consumed */
static bool tw_find_key(struct termwin *tw, const struct gui_event *ev) {
    struct term *t = cur_tab(tw)->t;
    int len = strlen(tw->q);
    if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) return false;
    if (ev->key == KEY_ESC) { tw_find_toggle(tw); return true; }
    if (ev->key == KEY_ENTER || ev->key == KEY_DOWN || ev->key == KEY_UP) {
        bool up = ev->key == KEY_UP || (ev->key == KEY_ENTER && (ev->mods & MOD_SHIFT));
        term_search_step(t, up ? -1 : 1);
        tw->dirty = true;
        return true;
    }
    if (ev->key == KEY_BACKSPACE) {
        while (len > 0 && (tw->q[len - 1] & 0xC0) == 0x80) len--;
        if (len > 0) len--;
        tw->q[len] = 0;
    } else if (ev->ch >= 32 && ev->ch < 127 && len < (int)sizeof tw->q - 1) {
        tw->q[len] = (char)ev->ch; tw->q[len + 1] = 0;
    } else return false;
    tw_find_apply(tw);
    return true;
}

static bool tw_shortcut(struct termwin *tw, const struct gui_event *ev) {
    int m = ev->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_SUPER), k = ev->key;
    if (m == (MOD_CTRL | MOD_SHIFT) && k < 256) {
        switch (k | 0x20) {
        case 'c': tw_copy(tw); return true;
        case 'v': tw_paste(tw); return true;
        case 't': tw_new_tab(tw, 0, false); return true;
        case 'w': tw_close_tab(tw, tw->cur); return true;
        case 'f': tw_find_toggle(tw); return true;
        case '=': tw_zoom(tw, tw->zoom + 1); return true;        /* Ctrl+Shift+= is Ctrl++ */
        case '-': tw_zoom(tw, tw->zoom - 1); return true;
        }
    }
    if (m & MOD_CTRL && !(m & (MOD_ALT | MOD_SUPER))) {
        bool shift = m & MOD_SHIFT;
        if (k == KEY_PGDN && !shift) { tw_switch(tw, (tw->cur + 1) % tw->n); return true; }
        if (k == KEY_PGUP && !shift) { tw_switch(tw, (tw->cur + tw->n - 1) % tw->n); return true; }
        if (k == KEY_TAB) { tw_switch(tw, (tw->cur + (shift ? tw->n - 1 : 1)) % tw->n); return true; }
        if (!shift && k == '=') { tw_zoom(tw, tw->zoom + 1); return true; }
        if (!shift && k == '-') { tw_zoom(tw, tw->zoom - 1); return true; }
        if (!shift && k == '0') { tw_zoom(tw, TERM_ZOOM_DEF); return true; }
    }
    return false;
}

/* tab bar clicks: switch, close button, middle-click close, "+" */
static void tw_bar_click(struct termwin *tw, int x, int y, int button) {
    for (int i = 0; i < tw->n; i++) {
        int tx, ty, w, h, tabw;
        tab_rect(tw, i, &tx, &ty, &w, &h, &tabw);
        if (x < tx || x >= tx + w) continue;
        if (button == 4 || x >= tx + w - 26) tw_close_tab(tw, i);
        else tw_switch(tw, i);
        return;
    }
    int tx, ty, w, h, tabw;
    tab_rect(tw, tw->n, &tx, &ty, &w, &h, &tabw);
    if (x >= tx && x < tx + 26) tw_new_tab(tw, 0, false);
}

static void tw_hover(struct termwin *tw, int col, int row, bool ctrl) {
    struct term *t = cur_tab(tw)->t;
    char url[256];
    int c0, c1;
    if (ctrl && term_link_at(t, col, row, url, sizeof url, &c0, &c1)) term_set_hover(t, row, c0, c1);
    else term_set_hover(t, -1, 0, 0);
    tw->dirty = true;
}

static void tw_event(struct window *w, const struct gui_event *ev) {
    struct termwin *tw = w->priv;
    if (!tw->n) return;                                          /* last tab closed, window about to go */
    struct term *t = cur_tab(tw)->t;
    int col = (ev->x - PAD) / term_cell_w(t), row = (ev->y - term_y(tw)) / term_cell_h(t);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->y < bar_h(tw)) { tw_bar_click(tw, ev->x, ev->y, ev->button); break; }
        if (ev->button == 4) { tw_paste(tw); break; }
        if (ev->button != 1) break;
        if (keyboard_mods() & MOD_CTRL) {                       /* Ctrl+click opens a URL */
            char url[256];
            if (ev->y >= term_y(tw) && term_link_at(t, col, row, url, sizeof url, 0, 0)) { wm_spawn_app("/bin/browser", url); break; }
        }
        term_select_clear(t);
        tw->selecting = true; tw->sc = col; tw->sr = row;
        tw->dirty = true;
        break;
    case EV_MOUSE_MOVE:
        if (!(ev->button & 1) || !tw->selecting) {
            if (ev->y >= term_y(tw)) tw_hover(tw, col, row, keyboard_mods() & MOD_CTRL);
            break;
        }
        if (col == tw->sc && row == tw->sr) break;
        term_select(t, tw->sc, tw->sr, col, row);
        tw->dirty = true;
        break;
    case EV_MOUSE_LEAVE:
        term_set_hover(t, -1, 0, 0);
        tw->dirty = true;
        break;
    case EV_MOUSE_UP:
        if (ev->button == 1 && tw->selecting) { tw->selecting = false; tw_copy(tw); }
        break;
    case EV_KEY:
        if (!ev->pressed) return;
        if (tw->find && tw_find_key(tw, ev)) return;
        if (tw_shortcut(tw, ev)) return;
        if ((ev->mods & MOD_SHIFT) && (ev->key == KEY_PGUP || ev->key == KEY_PGDN)) {
            term_scroll_view(t, ev->key == KEY_PGUP ? term_rows(t) / 2 : -term_rows(t) / 2);
            tw->dirty = tw->full = true;
            return;
        }
        if (tw->find) return;                                   /* the find box owns the keyboard */
        if (cur_tab(tw)->tty) tty_key(cur_tab(tw)->tty, ev);
        break;
    case EV_SCROLL:
        term_scroll_view(t, -ev->dz * 3);
        tw->dirty = tw->full = true;
        break;
    case EV_RESIZE:
        tw->pw = ev->w; tw->ph = ev->h;
        tw_relayout(tw);
        break;
    case EV_FOCUS:
        tw->focused = ev->pressed;
        term_set_focused(t, ev->pressed);
        tw->dirty = true;
        break;
    case EV_CLOSE:
        while (tw->n) tw_close_tab(tw, tw->n - 1);
        break;
    }
}

static void tw_destroy(struct window *w) {
    struct termwin *tw = w->priv;
    for (int i = 0; i < tw->n; i++) tab_free(tw->tabs[i]);
    while (tw->dying) { struct tab *tb = tw->dying; tw->dying = tb->next; tab_free(tb); }
    kfree(tw);
}

/* argv == NULL: interactive /bin/sh; hold: keep the window after the program exits */
struct window *termwin_open_argv(char *const argv_in[], bool hold) {
    int cols = 96, rows = 28;
    struct termwin *tw = kzalloc(sizeof *tw);
    if (!tw) return 0;
    tw->zoom = TERM_ZOOM_DEF;
    tw->shell = !argv_in;
    tw->n = 1;
    tw->tabs[0] = tab_make(tw, cols, rows);
    struct tab *tb = tw->tabs[0];
    struct window *w = 0;
    if (tb) {
        tw->pw = cols * term_cell_w(tb->t) + 2 * PAD;
        tw->ph = rows * term_cell_h(tb->t) + 2 * PAD - 4 + bar_h(tw);
        w = wm_create("Terminal", -1, -1, tw->pw, tw->ph, 0);
    }
    if (!w) { if (tb) tab_free(tb); kfree(tw); return 0; }
    tw->w = w;
    tw->focused = wm_focused() == w;
    term_set_focused(tb->t, tw->focused);
    w->priv = tw;
    w->on_event = tw_event;
    w->on_frame = tw_frame;
    w->on_destroy = tw_destroy;
    tty_set_winsize(tb->tty, cols, rows, tw->pw, tw->ph);
    tw_redraw_all(tw);
    tab_spawn(tb, argv_in, hold);
    return w;
}

struct window *termwin_open(const char *cmd) {
    if (!cmd) return termwin_open_argv(0, false);
    char *argv[] = { "/bin/sh", "-c", (char *)cmd, 0 };
    return termwin_open_argv(argv, false);
}
