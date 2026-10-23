/* termwin.c - terminal windows: window + tabs, each tab a terminal emulator + tty + shell.
 * Ctrl+Shift+T/W new/close tab, Ctrl+PgUp/PgDn (or Ctrl+Tab) switch, Ctrl+Shift+F find, Ctrl+=/-/0 zoom,
 * Ctrl+Shift+C/V copy/paste, Ctrl+click on a URL opens /bin/browser. */
#include "wm.h"
#include "tty.h"
#include "term.h"
#include "font.h"
#include "design.h"
#include "icons.h"

#define PADX SP_4                 /* content inset 8 12 (docs/DESIGN.md §9 Terminal) */
#define PADY SP_3
#define TAB_W 152
#define MAXTABS 8
#define FIND_H 40                 /* the find strip: a 32px field, match count, prev/next/close */
#define FIND_W 320

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
    int hov, hov_x, hov_f;        /* tab under the pointer (-1 none, n = the + button), its close box hot, find button hot */
    int ul_from;                  /* signal line x when it started sliding to the current tab */
    uint32_t ul_born, sb_until;   /* tab underline slide start; scrollbar shown until */
    int sb_y, sb_h;               /* scrollbar thumb as last drawn (sb_h 0: hidden) */
};

#define TH cur_theme                                         /* the chrome follows the desktop theme */
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static struct tab *cur_tab(struct termwin *tw) { return tw->tabs[tw->cur]; }
static int bar_h(struct termwin *tw) { return tw->n > 1 || tw->shell ? H_TAB : 0; }
static int find_h(struct termwin *tw) { return tw->find ? FIND_H : 0; }
static int term_y(struct termwin *tw) { return bar_h(tw) + find_h(tw) + PADY; }

static void icon(struct canvas *c, int id, int x, int y, color_t col) {
    const struct icon *i = icon_get(id, ICON_SM);
    if (i) gfx_mask(c, x, y, i->a8, i->w, i->h, i->w, col);
}

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
    gfx_fill(&tw->w->canvas, 0, 0, tw->w->w, tw->w->h, TH.term.bg);
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
    term_size_for_pixels(cur_tab(tw)->t, tw->pw - 2 * PADX, tw->ph - 2 * PADY - bar_h(tw) - find_h(tw), cols, rows);
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
    tw->ul_from = tw->cur; tw->ul_born = ticks_ms;
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

/* ---- drawing: tab strip, find strip, scrollbar ---- */

static int tab_w(struct termwin *tw) { return imin(TAB_W, imax(72, (tw->pw - 2 * SP_2 - H_CTL - SP_2) / tw->n)); }
static int tab_x(struct termwin *tw, int i) { return SP_2 + i * tab_w(tw); }
#define TAB_X_BOX(tw, i) (tab_x(tw, i) + tab_w(tw) - 6 - 20)        /* the 20px close box */
#define PLUS_X(tw) (tab_x(tw, (tw)->n) + SP_2)

static void draw_bar(struct termwin *tw) {
    struct canvas *c = &tw->w->canvas;
    int tw_ = tab_w(tw);
    gfx_fill(c, 0, 0, tw->pw, H_TAB - 1, tw->focused ? TH.window : TH.base);
    gfx_hline(c, 0, H_TAB - 1, tw->pw, TH.line);
    for (int i = 0; i < tw->n; i++) {
        struct tab *tb = tw->tabs[i];
        int x = tab_x(tw, i);
        bool on = i == tw->cur;
        const char *name = term_title(tb->t), *at = strchr(name, '@'), *col = at ? strchr(at, ':') : 0;
        if (col && col[1] == ' ') name = col + 2;                  /* "vibe@vibeos: ~" -> "~" */
        if (!*name) name = "Terminal";
        int lx = x + SP_4, lw = tw_ - SP_4 - SP_3 - 20 - 6;
        if (tb->activity && !on) { gfx_round_fill(c, lx, (H_TAB - 6) / 2, 6, 6, 3, TH.text2); lx += 6 + SP_2; lw -= 6 + SP_2; }
        const struct font *f = on ? T_BODY_B : T_BODY;
        gfx_text_ell(c, f, lx, (H_TAB - f->height) / 2, lw, name, on ? TH.text : TH.text2);
        if (on || tw->hov == i) {
            int bx = TAB_X_BOX(tw, i), by = (H_TAB - 20) / 2;
            bool hot = tw->hov == i && tw->hov_x;
            if (hot) gfx_round_fill(c, bx, by, 20, 20, R_2, TH.hover_ov);
            icon(c, ICON_CLOSE, bx + 2, by + 2, hot ? TH.text : TH.text3);
        }
    }
    /* the signal line: under the active tab, sliding from the last one over D_NORMAL */
    int d = theme_anim ? D_NORMAL : 0, age = ticks_ms - tw->ul_born, x1 = tab_x(tw, tw->cur);
    int x0 = tw->ul_from >= 0 && tw->ul_from < tw->n ? tab_x(tw, tw->ul_from) : x1;
    if (age < d) { int t = age * 256 / d, e = 256 - (256 - t) * (256 - t) / 256; x1 = x0 + (x1 - x0) * e / 256; tw->bar_dirty = true; }
    gfx_round_fill(c, x1 + SP_4, H_TAB - SIGNAL, tw_ - 2 * SP_4, SIGNAL, R_1, tw->focused ? TH.accent_line : TH.text3);
    if (tw->n < MAXTABS) {
        int px = PLUS_X(tw), py = (H_TAB - H_CTL) / 2;
        if (tw->hov == tw->n) gfx_round_fill(c, px, py, H_CTL, H_CTL, R_2, TH.hover_ov);
        icon(c, ICON_PLUS, px + (H_CTL - ICON_SM) / 2, py + (H_CTL - ICON_SM) / 2, tw->hov == tw->n ? TH.text : TH.text2);
    }
    wm_damage_client(tw->w, 0, 0, tw->pw, H_TAB);
}

/* find strip buttons: 0 previous, 1 next, 2 close */
static int find_bx(struct termwin *tw, int i) { return SP_3 + imin(FIND_W, tw->pw - 2 * SP_3 - 3 * (H_CTL + SP_2)) + SP_3 + i * (H_CTL + SP_2); }

static void draw_find(struct termwin *tw) {
    struct canvas *c = &tw->w->canvas;
    int y0 = bar_h(tw), fw = imin(FIND_W, tw->pw - 2 * SP_3 - 3 * (H_CTL + SP_2)), x = SP_3, y = y0 + (FIND_H - H_SEARCH) / 2;
    gfx_fill(c, 0, y0, tw->pw, FIND_H - 1, TH.window);
    gfx_hline(c, 0, y0 + FIND_H - 1, tw->pw, TH.line);
    gfx_round_fill(c, x, y, fw, H_SEARCH, R_2, TH.raised);
    gfx_rrect_ring(c, x - 1, y - 1, fw + 2, H_SEARCH + 2, RADII(R_2 + 1, R_2 + 1, R_2 + 1, R_2 + 1), FOCUS_W, TH.accent_text);   /* it has the keyboard */
    icon(c, ICON_SEARCH, x + SP_3, y + (H_SEARCH - ICON_SM) / 2, TH.text2);
    int cur, total;
    term_search_info(cur_tab(tw)->t, &cur, &total);
    char cnt[24] = "";
    if (tw->q[0] && !total) strlcpy(cnt, "No matches", sizeof cnt);
    else if (tw->q[0]) snprintf(cnt, sizeof cnt, "%d of %d", cur, total);
    int cw = gfx_text_width(T_CAPTION, cnt), ty = y + (H_SEARCH - T_BODY->height) / 2;
    gfx_text(c, T_CAPTION, x + fw - SP_3 - cw, y + (H_SEARCH - T_CAPTION->height) / 2, cnt, total || !tw->q[0] ? TH.text2 : TH.danger);
    int qx = x + SP_3 + ICON_SM + SP_3, qmax = x + fw - SP_3 - cw - SP_3 - qx;
    const char *q = tw->q;
    while (*q && gfx_text_width(T_BODY, q) > qmax - 2) q++;      /* keep the end of a long query visible */
    int w = *q ? gfx_text(c, T_BODY, qx, ty, q, TH.text) : 0;
    if (!tw->q[0]) gfx_text(c, T_BODY, qx + 2, ty, "Find in scrollback", TH.text3);
    if (tw->blink_on) gfx_fill(c, qx + w, y + (H_SEARCH - 16) / 2, 1, 16, TH.text);
    static const short ids[3] = { ICON_CHEVRON_UP, ICON_CHEVRON_DOWN, ICON_CLOSE };
    for (int i = 0; i < 3; i++) {
        int bx = find_bx(tw, i), by = y0 + (FIND_H - H_CTL) / 2;
        if (tw->hov_f == i + 1) gfx_round_fill(c, bx, by, H_CTL, H_CTL, R_2, TH.hover_ov);
        icon(c, ids[i], bx + (H_CTL - ICON_SM) / 2, by + (H_CTL - ICON_SM) / 2, tw->hov_f == i + 1 ? TH.text : TH.text2);
    }
    wm_damage_client(tw->w, 0, y0, tw->pw, FIND_H);
}

/* overlay scrollbar in the right padding: while scrolled back, and for D_SCROLL_FADE after a scroll */
static void draw_sb(struct termwin *tw) {
    struct term *t = cur_tab(tw)->t;
    int off = term_view_offset(t), rows = term_rows(t), total = term_scrollback_lines(t) + rows;
    int y0 = bar_h(tw) + find_h(tw) + SP_1, track = tw->ph - y0 - SP_1, th = 0, ty = 0;
    if ((off > 0 || ticks_ms < tw->sb_until) && total > rows && !term_on_alt_screen(t)) {
        th = imax(SCROLL_MIN, track * rows / total);
        ty = y0 + (track - th) * (total - rows - off) / (total - rows);
    }
    if (th == tw->sb_h && ty == tw->sb_y) return;
    struct canvas *c = &tw->w->canvas;
    int x = tw->pw - SP_1 - SCROLL_W;
    gfx_fill(c, x, y0, SCROLL_W, track, TH.term.bg);
    if (th) gfx_round_fill(c, x, ty, SCROLL_W, th, R_F(SCROLL_W), TH_A(TH.text, 89));
    tw->sb_h = th; tw->sb_y = ty;
    wm_damage_client(tw->w, x, y0, SCROLL_W, track);
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
    const char *title = term_title(ct->t);                    /* the window says what it is: "vibe@vibeos: ~ — Terminal" */
    if (*title && strcmp(title, tw->title)) {
        char t[64];
        strlcpy(tw->title, title, sizeof tw->title);
        snprintf(t, sizeof t, "%s \xe2\x80\x94 Terminal", title);
        wm_set_title(w, t);
    }
    if (tw->bar_dirty) {
        tw->bar_dirty = false;
        if (bar_h(tw)) draw_bar(tw);
    }
    draw_sb(tw);
    if (!tw->dirty) return;
    tw->dirty = false;
    struct rect d;
    if (tw->full) tw->sb_h = -1;                                 /* the repaint wipes it */
    term_render(ct->t, &w->canvas, PADX, term_y(tw), tw->full, &d);
    tw->full = false;
    if (d.w) wm_damage_client(w, d.x, d.y, d.w, d.h);
    if (tw->find) draw_find(tw);
    draw_sb(tw);
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
    if (tw->find) term_select_clear(cur_tab(tw)->t);
    else term_search_set(cur_tab(tw)->t, "");
    tw_relayout(tw);
    if (tw->find) tw_find_apply(tw);
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

/* tab strip clicks: switch, close box, middle-click close, "+" */
static void tw_bar_click(struct termwin *tw, int x, int y, int button) {
    for (int i = 0; i < tw->n; i++) {
        int tx = tab_x(tw, i);
        if (x < tx || x >= tx + tab_w(tw)) continue;
        if (button == 4 || (x >= TAB_X_BOX(tw, i) && x < TAB_X_BOX(tw, i) + 20)) tw_close_tab(tw, i);
        else tw_switch(tw, i);
        return;
    }
    if (tw->n < MAXTABS && x >= PLUS_X(tw) && x < PLUS_X(tw) + H_CTL) tw_new_tab(tw, 0, false);
}

/* pointer over the strips: which tab / close box / button is hot */
static void tw_bar_hover(struct termwin *tw, int x, int y) {
    int hov = -1, hx = 0, hf = 0;
    if (y >= 0 && y < bar_h(tw)) {
        for (int i = 0; i < tw->n; i++) if (x >= tab_x(tw, i) && x < tab_x(tw, i) + tab_w(tw)) { hov = i; hx = x >= TAB_X_BOX(tw, i) && x < TAB_X_BOX(tw, i) + 20; }
        if (tw->n < MAXTABS && x >= PLUS_X(tw) && x < PLUS_X(tw) + H_CTL) hov = tw->n;
    } else if (tw->find && y >= bar_h(tw) && y < bar_h(tw) + FIND_H) {
        for (int i = 0; i < 3; i++) if (x >= find_bx(tw, i) && x < find_bx(tw, i) + H_CTL) hf = i + 1;
    }
    if (hov != tw->hov || hx != tw->hov_x) { tw->hov = hov; tw->hov_x = hx; tw->bar_dirty = true; }
    if (hf != tw->hov_f) { tw->hov_f = hf; tw->dirty = true; }
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
    int col = (ev->x - PADX) / term_cell_w(t), row = (ev->y - term_y(tw)) / term_cell_h(t);
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->y < bar_h(tw)) { tw_bar_click(tw, ev->x, ev->y, ev->button); break; }
        if (ev->y < bar_h(tw) + find_h(tw)) {                    /* find strip: previous, next, close */
            if (ev->button != 1 || !tw->hov_f) break;
            if (tw->hov_f == 3) tw_find_toggle(tw);
            else { term_search_step(t, tw->hov_f == 1 ? -1 : 1); tw->dirty = true; }
            break;
        }
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
        tw_bar_hover(tw, ev->x, ev->y);
        if (!(ev->button & 1) || !tw->selecting) {
            if (ev->y >= term_y(tw)) tw_hover(tw, col, row, keyboard_mods() & MOD_CTRL);
            break;
        }
        if (col == tw->sc && row == tw->sr) break;
        term_select(t, tw->sc, tw->sr, col, row);
        tw->dirty = true;
        break;
    case EV_MOUSE_LEAVE:
        tw_bar_hover(tw, -1, -1);
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
            tw->sb_until = ticks_ms + D_SCROLL_FADE;
            tw->dirty = tw->full = true;
            return;
        }
        if (tw->find) return;                                   /* the find box owns the keyboard */
        if (cur_tab(tw)->tty) tty_key(cur_tab(tw)->tty, ev);
        break;
    case EV_SCROLL:
        term_scroll_view(t, -ev->dz * 3);
        tw->sb_until = ticks_ms + D_SCROLL_FADE;
        tw->dirty = tw->full = true;
        break;
    case EV_RESIZE:
        tw->pw = ev->w; tw->ph = ev->h;
        tw_relayout(tw);
        break;
    case EV_FOCUS:
        tw->focused = ev->pressed;
        term_set_focused(t, ev->pressed);
        tw->dirty = tw->bar_dirty = true;
        break;
    case EV_CLOSE:
        while (tw->n) tw_close_tab(tw, tw->n - 1);
        break;
    case EV_THEME:                                               /* new colours (and maybe text size): repaint everything */
        if (ev->x) tw_zoom(tw, theme_zoom);
        tw_redraw_all(tw);
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
    tw->zoom = theme_zoom;
    tw->shell = !argv_in;
    tw->n = 1;
    tw->hov = -1; tw->ul_from = -1;
    tw->tabs[0] = tab_make(tw, cols, rows);
    struct tab *tb = tw->tabs[0];
    struct window *w = 0;
    if (tb) {
        tw->pw = cols * term_cell_w(tb->t) + 2 * PADX;
        tw->ph = rows * term_cell_h(tb->t) + 2 * PADY + bar_h(tw);
        w = wm_create("Terminal", -1, -1, tw->pw, tw->ph, 0);
    }
    if (!w) { if (tb) tab_free(tb); kfree(tw); return 0; }
    tw->w = w;
    w->icon = ICON_TERMINAL;
    if (w->w != tw->pw || w->h != tw->ph) { tw->pw = w->w; tw->ph = w->h; tw_relayout(tw); }   /* clamped to the screen: refit the grid */
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
