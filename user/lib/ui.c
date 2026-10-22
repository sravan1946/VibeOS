/* ui.c - see ui.h */
#include "ui.h"
#include <string.h>
#include <stdlib.h>
#include <syscall.h>

static uint32_t rid(int x, int y, int w, int h) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)w * 83492791u) ^ (uint32_t)h; }
uint32_t ui_id(int x, int y, int w, int h) { return rid(x, y, w, h); }
static bool inside(struct ui *ui, int x, int y, int w, int h) { return ui->mx >= x && ui->my >= y && ui->mx < x + w && ui->my < y + h; }

/* widgets draw into a private back buffer; ui_end copies what changed into the shared window
 * buffer, so the compositor never sees (or keeps) a half-drawn frame */
static void ensure_back(struct ui *ui) {
    struct gui_window *w = ui->win;
    if (ui->back && ui->back_w == w->w && ui->back_h == w->h) return;
    free(ui->back);
    ui->back = malloc((size_t)w->w * w->h * 4);
    ui->back_w = w->w; ui->back_h = w->h;
    if (!ui->back) { ui->c = &w->cv; return; }             /* no memory: draw straight to the window */
    for (int y = 0; y < w->h; y++) memcpy(ui->back + y * w->w, w->pixels + y * w->stride, w->w * 4);
    canvas_init(&ui->bcv, ui->back, w->w, w->h, w->w);
    ui->c = &ui->bcv;
}

void ui_init(struct ui *ui, struct gui_window *win) {
    memset(ui, 0, sizeof *ui);
    ui->win = win;
    ui->mx = ui->my = -1;
    ui->focused = true;
    ensure_back(ui);
}

bool ui_poll(struct ui *ui, int timeout_ms) {
    if (ui->closed) return false;
    ui->pressed = ui->released = ui->rpressed = ui->dbl = ui->resized = ui->timeout = false;
    ui->scroll = 0;
    ui->nkeys = 0;
    ui->nups = 0;
    struct gui_event ev;
    int r = gui_next_event(ui->win, &ev, timeout_ms);
    if (r == 0) { ui->timeout = true; return true; }
    for (; r > 0; r = gui_next_event(ui->win, &ev, 0)) {
        switch (ev.type) {
        case EV_MOUSE_MOVE: ui->mx = ev.x; ui->my = ev.y; ui->buttons = ev.button; break;
        case EV_MOUSE_DOWN:
            ui->mx = ev.x; ui->my = ev.y;
            if (ev.button == 2) { ui->rpressed = true; break; }
            ui->pressed = true; ui->buttons |= 1;
            uint32_t now = (uint32_t)uptime_ms();
            ui->dbl = now - ui->last_click_ms < 400 && abs(ev.x - ui->last_click_x) < 5 && abs(ev.y - ui->last_click_y) < 5;
            ui->last_click_ms = ui->dbl ? 0 : now;
            ui->last_click_x = ev.x; ui->last_click_y = ev.y;
            break;
        case EV_MOUSE_UP: ui->mx = ev.x; ui->my = ev.y; ui->released = true; ui->buttons &= ~1; break;
        case EV_MOUSE_LEAVE: ui->mx = ui->my = -1; break;
        case EV_SCROLL: ui->scroll += ev.dz; break;
        case EV_KEY:
            if (ev.pressed && ui->nkeys < 32) ui->keys[ui->nkeys++] = ev;
            else if (!ev.pressed && ui->nups < 16) ui->ups[ui->nups++] = ev.key;
            break;
        case EV_RESIZE: ui->resized = true; break;
        case EV_FOCUS: ui->focused = ev.pressed; break;
        case EV_CLOSE: ui->closed = true; return false;
        }
        if (ui->pressed || ui->released) break;   /* one click per frame keeps widgets simple */
    }
    if (ui->pressed) ui->focus = 0;               /* widgets re-claim focus on click */
    if (ui->resized) ensure_back(ui);
    return true;
}

void ui_end(struct ui *ui) {
    if (ui->released) ui->active = 0;
    struct gui_window *w = ui->win;
    if (ui->back_w != w->w || ui->back_h != w->h) { ensure_back(ui); return; }   /* resized mid-frame */
    if (ui->c != &ui->bcv) { gui_flip(w); return; }
    int x0 = w->w, y0 = -1, x1 = -1, y1 = -1;              /* bounding box of changed pixels */
    for (int y = 0; y < w->h; y++) {
        uint32_t *a = ui->back + y * w->w, *b = w->pixels + y * w->stride;
        if (!memcmp(a, b, w->w * 4)) continue;
        int l = 0, r = w->w - 1;
        while (a[l] == b[l]) l++;
        while (a[r] == b[r]) r--;
        if (l < x0) x0 = l;
        if (r > x1) x1 = r;
        if (y0 < 0) y0 = y;
        y1 = y;
        memcpy(b + l, a + l, (r - l + 1) * 4);
    }
    if (y0 >= 0) gui_flip_rect(w, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

void ui_clear(struct ui *ui) { gfx_fill(ui->c, 0, 0, ui->win->w, ui->win->h, UI_BG); }

bool ui_hover(struct ui *ui, int x, int y, int w, int h) { return inside(ui, x, y, w, h); }
bool ui_clicked(struct ui *ui, int x, int y, int w, int h) { return ui->pressed && inside(ui, x, y, w, h); }

bool ui_key(struct ui *ui, struct gui_event *out) {
    if (!ui->nkeys) return false;
    *out = ui->keys[0];
    memmove(ui->keys, ui->keys + 1, --ui->nkeys * sizeof *out);
    return true;
}

void ui_panel(struct ui *ui, int x, int y, int w, int h) {
    gfx_round_fill(ui->c, x, y, w, h, UI_RADIUS + 2, UI_PANEL);
    gfx_round_rect(ui->c, x, y, w, h, UI_RADIUS + 2, UI_BORDER);
}

void ui_label(struct ui *ui, int x, int y, const char *s, color_t col) { gfx_text(ui->c, &font_ui, x, y, s, col); }

bool ui_button(struct ui *ui, int x, int y, int w, int h, const char *label, int style) {
    uint32_t id = rid(x, y, w, h);
    bool hov = inside(ui, x, y, w, h), clicked = false;
    if (hov && ui->pressed) ui->active = id;
    if (ui->released && ui->active == id && hov) clicked = true;
    bool down = ui->active == id && (ui->buttons & 1) && hov;
    color_t bg;
    if (style == UI_ACCENT_BTN) {
        bg = down ? RGB(0x95, 0x6C, 0xE0) : hov ? RGB(0xC4, 0xA0, 0xFF) : UI_ACCENT;
    } else if (style == UI_GHOST_BTN) {
        bg = down ? UI_PRESSED : hov ? UI_SURFACE : RGBA(0, 0, 0, 0);
    } else {
        bg = down ? UI_PRESSED : hov ? UI_HOVER : UI_SURFACE;
    }
    if (ALPHA(bg)) gfx_round_fill(ui->c, x, y, w, h, UI_RADIUS, bg);
    if (style == UI_NORMAL_BTN) gfx_round_rect(ui->c, x, y, w, h, UI_RADIUS, UI_BORDER);
    const struct font *f = style == UI_ACCENT_BTN ? &font_ui_bold : &font_ui;
    color_t tc = style == UI_ACCENT_BTN ? RGB(0x1A, 0x10, 0x2E) : UI_TEXT;
    int tw = gfx_text_width(f, label);
    gfx_text(ui->c, f, x + (w - tw) / 2, y + (h - f->height) / 2 + (down ? 1 : 0), label, tc);
    return clicked;
}

bool ui_checkbox(struct ui *ui, int x, int y, const char *label, bool *v) {
    int s = 18, tw = gfx_text_width(&font_ui, label);
    bool hov = inside(ui, x, y, s + 8 + tw, s);
    bool changed = false;
    if (hov && ui->pressed) { *v = !*v; changed = true; }
    if (*v) {
        gfx_round_fill(ui->c, x, y, s, s, 5, UI_ACCENT);
        gfx_line_aa(ui->c, x + 4, y + 9, x + 8, y + 13, 2.2f, RGB(0x1A, 0x10, 0x2E));
        gfx_line_aa(ui->c, x + 8, y + 13, x + 14, y + 5, 2.2f, RGB(0x1A, 0x10, 0x2E));
    } else {
        gfx_round_fill(ui->c, x, y, s, s, 5, hov ? UI_HOVER : UI_SURFACE);
        gfx_round_rect(ui->c, x, y, s, s, 5, RGBA(255, 255, 255, 50));
    }
    gfx_text(ui->c, &font_ui, x + s + 8, y + (s - font_ui.height) / 2, label, UI_TEXT);
    return changed;
}

bool ui_slider(struct ui *ui, int x, int y, int w, int *v, int min, int max) {
    uint32_t id = rid(x, y, w, 20);
    int h = 20, old = *v;
    if (inside(ui, x - 8, y, w + 16, h) && ui->pressed) ui->active = id;
    if (ui->active == id && (ui->buttons & 1)) {
        int t = ui->mx - x;
        if (t < 0) t = 0;
        if (t > w) t = w;
        *v = min + (max - min) * t / (w ? w : 1);
    }
    int px = x + (max > min ? (*v - min) * w / (max - min) : 0);
    gfx_round_fill(ui->c, x, y + h / 2 - 3, w, 6, 3, UI_SURFACE);
    gfx_round_fill(ui->c, x, y + h / 2 - 3, px - x + 3, 6, 3, UI_ACCENT);
    gfx_circle_fill(ui->c, px, y + h / 2, 8, ui->active == id ? RGB(255, 255, 255) : RGB(0xE8, 0xDF, 0xFF));
    return *v != old;
}

bool ui_textbox(struct ui *ui, int x, int y, int w, int h, char *buf, int cap, const char *placeholder) {
    uint32_t id = rid(x, y, w, h);
    if (ui->pressed && inside(ui, x, y, w, h)) ui->focus = id;
    bool foc = ui->focus == id, enter = false;
    if (foc) {
        struct gui_event k;
        int n = strlen(buf);
        while (ui_key(ui, &k)) {
            if (k.key == KEY_ENTER) { enter = true; break; }
            if (k.key == KEY_BACKSPACE || k.ch == 127 || k.ch == 8) { if (n) buf[--n] = 0; }
            else if (k.ch == 21) { n = 0; buf[0] = 0; }              /* ^U */
            else if (k.ch >= 32 && k.ch < 127 && n < cap - 1) { buf[n++] = k.ch; buf[n] = 0; }
        }
    }
    gfx_round_fill(ui->c, x, y, w, h, UI_RADIUS, RGB(0x12, 0x11, 0x1C));
    gfx_round_rect(ui->c, x, y, w, h, UI_RADIUS, foc ? UI_ACCENT : UI_BORDER);
    struct canvas cc = *ui->c;
    canvas_clip(&cc, x + 8, y, w - 16, h);
    int ty = y + (h - font_ui.height) / 2;
    int tw = gfx_text_width(&font_ui, buf);
    int tx = x + 10 - (tw > w - 24 ? tw - (w - 24) : 0);          /* keep the caret visible */
    if (!buf[0] && placeholder && !foc) gfx_text(&cc, &font_ui, x + 10, ty, placeholder, UI_TEXT_DIM);
    gfx_text(&cc, &font_ui, tx, ty, buf, UI_TEXT);
    if (foc) gfx_fill(&cc, tx + tw + 1, y + 7, 2, h - 14, UI_ACCENT);
    return enter;
}

int ui_list(struct ui *ui, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll) {
    const int rh = 30;
    int visible = h / rh, act = -1;
    uint32_t id = rid(x, y, w, h);
    if (ui->pressed && inside(ui, x, y, w, h)) ui->focus = id;
    if (inside(ui, x, y, w, h) && ui->scroll) *scroll += ui->scroll * 3;
    if (ui->focus == id) {
        struct gui_event k;
        while (ui_key(ui, &k)) {
            if (k.key == KEY_DOWN && *sel < n - 1) (*sel)++;
            else if (k.key == KEY_UP && *sel > 0) (*sel)--;
            else if (k.key == KEY_PGDN) *sel = *sel + visible < n ? *sel + visible : n - 1;
            else if (k.key == KEY_PGUP) *sel = *sel > visible ? *sel - visible : 0;
            else if (k.key == KEY_ENTER && *sel >= 0) act = *sel;
            else { ui->keys[ui->nkeys++] = k; break; }             /* leave it for the app */
            if (*sel < *scroll) *scroll = *sel;
            if (*sel >= *scroll + visible) *scroll = *sel - visible + 1;
        }
    }
    if (*scroll > n - visible) *scroll = n - visible;
    if (*scroll < 0) *scroll = 0;
    struct canvas cc = *ui->c;
    canvas_clip(&cc, x, y, w, h);
    for (int i = *scroll; i < n && i < *scroll + visible + 1; i++) {
        int ry = y + (i - *scroll) * rh;
        bool hov = inside(ui, x, ry, w, rh) && ry + rh <= y + h;
        if (hov && ui->pressed) { *sel = i; if (ui->dbl) act = i; }
        if (i == *sel) gfx_round_fill(&cc, x + 2, ry + 1, w - 4, rh - 2, 6, RGBA(0xB3, 0x88, 0xFF, ui->focus == id ? 90 : 50));
        else if (hov) gfx_round_fill(&cc, x + 2, ry + 1, w - 4, rh - 2, 6, RGBA(255, 255, 255, 14));
        gfx_text(&cc, &font_ui, x + 12, ry + (rh - font_ui.height) / 2, items[i], UI_TEXT);
    }
    if (n > visible) {                                             /* scrollbar */
        int th = h * visible / n, ty = y + (h - th) * *scroll / (n - visible);
        gfx_round_fill(ui->c, x + w - 5, ty, 4, th, 2, RGBA(255, 255, 255, 60));
    }
    return act;
}

void ui_text_fit(struct ui *ui, const struct font *f, int x, int y, int maxw, const char *s, color_t col) {
    if (maxw <= 0) return;
    if (gfx_text_width(f, s) <= maxw) { gfx_text(ui->c, f, x, y, s, col); return; }
    int dots = gfx_text_width(f, "...");
    int n = strlen(s);
    while (n > 0 && gfx_text_width_n(f, s, n) + dots > maxw) n--;
    int ax = gfx_text_n(ui->c, f, x, y, s, n, col);
    gfx_text(ui->c, f, x + ax, y, "...", col);
}

bool ui_tabs(struct ui *ui, int x, int y, int w, int h, const char *const *names, int n, int *sel) {
    int old = *sel, tw = (w - 8) / n;
    gfx_round_fill(ui->c, x, y, w, h, h / 2, UI_PANEL);
    gfx_round_rect(ui->c, x, y, w, h, h / 2, UI_BORDER);
    for (int i = 0; i < n; i++) {
        int tx = x + 4 + i * tw;
        bool hov = inside(ui, tx, y + 3, tw, h - 6);
        if (hov && ui->pressed) *sel = i;
        if (i == *sel) gfx_round_fill(ui->c, tx, y + 3, tw, h - 6, (h - 6) / 2, UI_ACCENT);
        else if (hov) gfx_round_fill(ui->c, tx, y + 3, tw, h - 6, (h - 6) / 2, UI_HOVER);
        const struct font *f = i == *sel ? &font_ui_bold : &font_ui;
        gfx_text_center(ui->c, f, tx, y + (h - f->height) / 2, tw, names[i], i == *sel ? RGB(0x1A, 0x10, 0x2E) : UI_TEXT);
    }
    return *sel != old;
}

bool ui_tool(struct ui *ui, int x, int y, int w, int h, const char *label, bool on) {
    uint32_t id = rid(x, y, w, h);
    bool hov = inside(ui, x, y, w, h), clicked = false;
    if (hov && ui->pressed) ui->active = id;
    if (ui->released && ui->active == id && hov) clicked = true;
    bool down = ui->active == id && (ui->buttons & 1) && hov;
    color_t bg = on ? RGBA(0xB3, 0x88, 0xFF, 90) : down ? UI_PRESSED : hov ? UI_HOVER : RGBA(0, 0, 0, 0);
    if (ALPHA(bg)) gfx_round_fill(ui->c, x, y, w, h, 6, bg);
    gfx_text_center(ui->c, &font_ui, x, y + (h - font_ui.height) / 2, w, label, UI_TEXT);
    return clicked;
}

void ui_vscroll(struct ui *ui, int x, int y, int h, int total, int visible, int *first) {
    if (total <= visible) { *first = 0; return; }
    uint32_t id = rid(x, y, 10, h);
    int th = h * visible / total;
    if (th < 24) th = 24;
    int range = total - visible, span = h - th;
    if (inside(ui, x - 4, y, 14, h) && ui->pressed) ui->active = id;
    if (ui->active == id && (ui->buttons & 1)) {
        int t = ui->my - y - th / 2;
        *first = span > 0 ? (t < 0 ? 0 : t > span ? range : t * range / span) : 0;
    }
    if (*first < 0) *first = 0;
    if (*first > range) *first = range;
    int ty = y + span * *first / range;
    gfx_round_fill(ui->c, x + 2, ty, 6, th, 3, ui->active == id ? RGBA(255, 255, 255, 110) : RGBA(255, 255, 255, 60));
}

static void dialog_box(struct ui *ui, int pw, int ph, int *ox, int *oy) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    gfx_fill(c, 0, 0, W, H, RGBA(0, 0, 0, 150));
    int px = (W - pw) / 2, py = (H - ph) / 2;
    gfx_shadow(c, px, py, pw, ph, 12, 14, 120);
    gfx_round_fill(c, px, py, pw, ph, 12, UI_PANEL);
    gfx_round_rect(c, px, py, pw, ph, 12, RGBA(255, 255, 255, 40));
    *ox = px; *oy = py;
}

int ui_prompt(struct ui *ui, const char *title, const char *okl, char *buf, int cap) {
    int W = ui->win->w, pw = W - 20 < 480 ? W - 20 : 480, ph = 150, px, py;
    dialog_box(ui, pw, ph, &px, &py);
    gfx_text(ui->c, &font_ui_bold, px + 20, py + 16, title, UI_TEXT);
    int tx = px + 20, ty = py + 46, tw = pw - 40;
    if (!ui->focus) ui->focus = rid(tx, ty, tw, 34);
    bool enter = ui_textbox(ui, tx, ty, tw, 34, buf, cap, "");
    int r = 0;
    if (ui_button(ui, px + pw - 120, py + ph - 52, 100, 34, okl, UI_ACCENT_BTN) || enter) r = 1;
    if (ui_button(ui, px + pw - 230, py + ph - 52, 100, 34, "Cancel", UI_NORMAL_BTN)) r = -1;
    struct gui_event k;
    while (ui_key(ui, &k)) if (k.key == KEY_ESC) r = -1;
    if (r) ui->focus = 0;
    return r;
}

int ui_confirm(struct ui *ui, const char *title, const char *text, const char *okl) {
    int W = ui->win->w, pw = W - 20 < 440 ? W - 20 : 440, ph = 140, px, py;
    dialog_box(ui, pw, ph, &px, &py);
    gfx_text(ui->c, &font_ui_bold, px + 20, py + 16, title, UI_TEXT);
    ui_text_fit(ui, &font_ui, px + 20, py + 46, pw - 40, text, UI_TEXT_DIM);
    int r = 0;
    if (ui_button(ui, px + pw - 120, py + ph - 52, 100, 34, okl, UI_ACCENT_BTN)) r = 1;
    if (ui_button(ui, px + pw - 230, py + ph - 52, 100, 34, "Cancel", UI_NORMAL_BTN)) r = -1;
    struct gui_event k;
    while (ui_key(ui, &k)) { if (k.key == KEY_ESC) r = -1; else if (k.key == KEY_ENTER) r = 1; }
    return r;
}
