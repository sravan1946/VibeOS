/* ui.c - see ui.h. Specs: docs/DESIGN.md §7 (focus), §8 (motion), §9 (components). */
#include "ui.h"
#include "icons.h"
#include "design.h"
#include <string.h>
#include <stdlib.h>
#include <syscall.h>

#define TH ui_th
static uint32_t rid(int x, int y, int w, int h) { return ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u) ^ ((uint32_t)w * 83492791u) ^ (uint32_t)h; }
uint32_t ui_id(int x, int y, int w, int h) { return rid(x, y, w, h); }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static bool in_rect(struct rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* overlays (menu, toast, a dialog) take the pointer from the widgets under them */
static bool blocked(struct ui *ui) {
    if (ui->in_ov) return false;
    if (ui->modal_prev) return true;
    if (ui->pop_id && in_rect(ui->pop_r, ui->mx, ui->my)) return true;
    return ui->toast_until && in_rect(ui->toast_r, ui->mx, ui->my);
}
static bool inside(struct ui *ui, int x, int y, int w, int h) {
    return ui->mx >= x && ui->my >= y && ui->mx < x + w && ui->my < y + h && !blocked(ui);
}
static void wake_at(struct ui *ui, uint32_t t) { if (!ui->wake || (int)(t - ui->wake) < 0) ui->wake = t; }
static color_t fade(color_t c, bool dis) { return dis ? TH_A(c, ALPHA(c) * 97 / 255) : c; }   /* 38% */

/* ---- keyboard ---- */
static bool take_key(struct ui *ui, int key) {
    for (int i = 0; i < ui->nkeys; i++)
        if (ui->keys[i].key == key || (key == ' ' && ui->keys[i].ch == ' ')) {
            memmove(ui->keys + i, ui->keys + i + 1, (--ui->nkeys - i) * sizeof ui->keys[0]);
            if (key >= KEY_UP && key <= KEY_PGDN) ui->kbd = true;
            return true;
        }
    return false;
}
/* register a keyboard focus stop; true if it holds focus */
static bool focusable(struct ui *ui, uint32_t id, bool dis) {
    if (dis || (ui->modal_prev && !ui->in_ov)) return false;
    if (ui->nf < 64) ui->fl[ui->nf++] = id;
    return ui->focus == id;
}
static void ring(struct ui *ui, bool foc, int x, int y, int w, int h, int r) {
    if ((foc && ui->kbd) || (ui->force & UI_F_FOCUS)) gfx_focus_ring(ui->c, x, y, w, h, r, TH.accent_text);
}

/* ---- motion: tweened values per (widget id, slot), eased out (cubic) ---- */
static int ease(int e, int dur) { int p = 256 - e * 256 / dur; return 256 - p * p / 256 * p / 256; }
static struct tw { uint32_t id; int from, to; uint32_t t0; } tws[48];
static int tween(struct ui *ui, uint32_t id, int to, int dur) {
    struct tw *t = 0;
    for (int i = 0; i < 48 && !t; i++) if (tws[i].id == id) t = &tws[i];
    if (!t) {                                                  /* new: start at rest; reuse the oldest slot */
        t = &tws[0];
        for (int i = 1; i < 48; i++) if ((int)(tws[i].t0 - t->t0) < 0) t = &tws[i];
        *t = (struct tw){ id, to, to, ui->now - 100000 };
    }
    int e = (int)(ui->now - t->t0);
    if (!ui_motion) dur = 0;
    int cur = e >= dur ? t->to : t->from + (t->to - t->from) * ease(e, dur) / 256;
    if (to != t->to) { t->from = cur; t->to = to; t->t0 = ui->now; if (!dur) return to; cur = t->from; e = 0; }
    if (e < dur) wake_at(ui, ui->now + 16);
    return cur;
}

/* ---- frame ---- */
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
    ui->pop_pick = -1;
    ui->now = uptime_ms();
    ensure_back(ui);
}

/* the open menu / toast see the press before any widget; keys for the menu never reach the app */
static void overlay_input(struct ui *ui) {
    if (ui->pop_id) {
        for (int i = 0; i < ui->nkeys; ) {
            int k = ui->keys[i].key, eat = 1;
            if (k == KEY_DOWN) ui->pop_hi = imin(ui->pop_hi + 1, ui->pop_n - 1);
            else if (k == KEY_UP) ui->pop_hi = imax(ui->pop_hi - 1, 0);
            else if (k == KEY_HOME) ui->pop_hi = 0;
            else if (k == KEY_END) ui->pop_hi = ui->pop_n - 1;
            else if (k == KEY_ENTER || k == ' ') { ui->pop_pick = ui->pop_hi; ui->focus = ui->pop_id; ui->pop_id = 0; }
            else if (k == KEY_ESC || k == KEY_TAB) { ui->focus = ui->pop_id; ui->pop_id = 0; }
            else eat = 0;
            if (eat) { memmove(ui->keys + i, ui->keys + i + 1, (--ui->nkeys - i) * sizeof ui->keys[0]); ui->kbd = true; } else i++;
        }
        if (ui->pressed) {
            if (in_rect(ui->pop_r, ui->mx, ui->my)) {
                int i = (ui->my - ui->pop_r.y - SP_2) / H_ROW;
                if (i >= 0 && i < ui->pop_n) { ui->pop_pick = i; ui->focus = ui->pop_id; ui->pop_id = 0; }
            } else ui->pop_id = 0;
            ui->pressed = false;
            return;
        }
    }
    if (ui->toast_until && ui->pressed && in_rect(ui->toast_r, ui->mx, ui->my)) {
        if (ui->toast_act[0] && in_rect(ui->toast_btn, ui->mx, ui->my)) { ui->toast_hit = true; ui->toast_until = 0; }
        ui->pressed = false;
    }
}

bool ui_poll(struct ui *ui, int timeout_ms) {
    if (ui->closed) return false;
    ui->pressed = ui->released = ui->rpressed = ui->dbl = ui->resized = ui->timeout = ui->dropped = ui->themed = false;
    ui->scroll = 0;
    ui->nkeys = 0;
    ui->nups = 0;
    struct gui_event ev;
    uint32_t t = uptime_ms();
    int tmo = timeout_ms, own = 0;                            /* own: woken for the toolkit's animation, not the app */
    if (ui->wake) {
        int d = imax((int)(ui->wake - t), 0);
        if (tmo < 0 || d < tmo) { tmo = d; own = 1; }
    }
    ui->wake = 0;
    int r = gui_next_event(ui->win, &ev, tmo);
    ui->now = uptime_ms();
    if (r == 0) { ui->timeout = !own; return true; }
    for (; r > 0; r = gui_next_event(ui->win, &ev, 0)) {
        switch (ev.type) {
        case EV_MOUSE_MOVE: ui->mx = ev.x; ui->my = ev.y; ui->buttons = ev.button; break;
        case EV_MOUSE_DOWN:
            ui->mx = ev.x; ui->my = ev.y;
            if (ev.button == 2) { ui->rpressed = true; break; }
            ui->pressed = true; ui->buttons |= 1;
            ui->dbl = ui->now - ui->last_click_ms < 400 && abs(ev.x - ui->last_click_x) < 5 && abs(ev.y - ui->last_click_y) < 5;
            ui->last_click_ms = ui->dbl ? 0 : ui->now;
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
        case EV_DROP: ui->dropped = true; ui->drop_x = ev.x; ui->drop_y = ev.y; break;
        case EV_FOCUS: ui->focused = ev.pressed; break;
        case EV_THEME: ui->themed = true; break;
        case EV_CLOSE: ui->closed = true; return false;
        }
        if (ui->pressed || ui->released) break;   /* one click per frame keeps widgets simple */
    }
    overlay_input(ui);
    if (ui->pressed) { ui->focus = 0; ui->kbd = false; }   /* widgets re-claim focus on click */
    if (ui->resized) ensure_back(ui);
    ui->nf = 0;
    ui->modal = false;
    return true;
}

static void draw_overlays(struct ui *ui);

void ui_end(struct ui *ui) {
    if (ui->released) ui->active = 0;
    for (int i = 0; i < ui->nkeys; i++)                      /* Tab nobody took moves keyboard focus */
        if (ui->keys[i].key == KEY_TAB && ui->nf && !(ui->keys[i].mods & (MOD_CTRL | MOD_ALT))) {
            int k = -1, n = ui->nf;
            for (int j = 0; j < n; j++) if (ui->fl[j] == ui->focus) k = j;
            k = ui->keys[i].mods & MOD_SHIFT ? (k < 0 ? n - 1 : (k + n - 1) % n) : (k + 1) % n;
            ui->focus = ui->fl[k]; ui->kbd = true;
            memmove(ui->keys + i, ui->keys + i + 1, (--ui->nkeys - i) * sizeof ui->keys[0]);
            wake_at(ui, ui->now);                              /* redraw with the ring */
            break;
        }
    ui->modal_prev = ui->modal;
    if (!ui->pop_seen) ui->pop_id = 0;                      /* its dropdown wasn't drawn: the items may be gone */
    ui->pop_seen = false;
    draw_overlays(ui);
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

void ui_clear(struct ui *ui) { gfx_fill(ui->c, 0, 0, ui->win->w, ui->win->h, TH.window); }

bool ui_hover(struct ui *ui, int x, int y, int w, int h) { return inside(ui, x, y, w, h); }
bool ui_clicked(struct ui *ui, int x, int y, int w, int h) { return ui->pressed && inside(ui, x, y, w, h); }

bool ui_key(struct ui *ui, struct gui_event *out) {
    if (!ui->nkeys) return false;
    *out = ui->keys[0];
    memmove(ui->keys, ui->keys + 1, --ui->nkeys * sizeof *out);
    return true;
}

/* ---- text and icons ---- */
void ui_label(struct ui *ui, int x, int y, const char *s, color_t col) { gfx_text(ui->c, T_BODY, x, y, s, col); }

void ui_text_fit(struct ui *ui, const struct font *f, int x, int y, int maxw, const char *s, color_t col) {
    if (maxw <= 0) return;
    if (gfx_text_width(f, s) <= maxw) { gfx_text(ui->c, f, x, y, s, col); return; }
    int dots = gfx_text_width(f, "…"), n = strlen(s);
    while (n > 0 && gfx_text_width_n(f, s, n) + dots > maxw) n--;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;         /* not inside a UTF-8 sequence */
    while (n > 0 && s[n - 1] == ' ') n--;
    int ax = gfx_text_n(ui->c, f, x, y, s, n, col);
    gfx_text(ui->c, f, x + ax, y, "…", col);
}

int ui_text_wrap(struct ui *ui, const struct font *f, int x, int y, int w, const char *s, color_t col) {
    int h = 0;
    while (*s) {
        int n = 0, fit = 0;                                    /* fit: bytes up to the last space that fits */
        while (s[n] && s[n] != '\n') {
            int e = n;
            while (s[e] && s[e] != ' ' && s[e] != '\n') e++;
            if (gfx_text_width_n(f, s, e) > w && fit) break;
            n = fit = e;
            if (s[n] == ' ') n++;
        }
        if (!fit) fit = n;
        if (ALPHA(col)) gfx_text_n(ui->c, f, x, y + h, s, fit, col);
        h += f->height;
        s += n > fit ? n : fit;
        if (*s == '\n') s++;
    }
    return h;
}

/* a 20px mask scaled to size (bilinear): empty states and other big symbolic icons */
static void mask_scaled(struct canvas *c, int x, int y, const struct icon *ic, int size, color_t col) {
    static uint8_t buf[64 * 64];
    if (size > 64) size = 64;
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            int fx = (i * 2 + 1) * ic->w * 128 / size - 128, fy = (j * 2 + 1) * ic->h * 128 / size - 128;   /* 8.8 fixed */
            int x0 = fx >> 8, y0 = fy >> 8, ax = fx & 255, ay = fy & 255, v = 0;
            for (int k = 0; k < 4; k++) {
                int sx = x0 + (k & 1), sy = y0 + (k >> 1);
                int wgt = ((k & 1) ? ax : 256 - ax) * ((k >> 1) ? ay : 256 - ay);
                if (sx >= 0 && sy >= 0 && sx < ic->w && sy < ic->h) v += ic->a8[sy * ic->w + sx] * wgt;
            }
            buf[j * size + i] = v >> 16;
        }
    gfx_mask(c, x, y, buf, size, size, size, col);
}

void ui_icon(struct ui *ui, int x, int y, int icon, int size, color_t col) {
    const struct icon *ic = icon >= 0 ? icon_get(icon, size) : 0;
    if (!ic) return;
    if (ic->w == size) gfx_mask(ui->c, x, y, ic->a8, ic->w, ic->h, ic->w, col);
    else mask_scaled(ui->c, x, y, ic, size, col);
}

void ui_aicon(struct ui *ui, int x, int y, const struct aicon *a) { if (a) gfx_blit_premul(ui->c, x, y, a->argb, a->w, a->h, a->w); }

/* ---- buttons ---- */
static void spinner(struct ui *ui, int x, int y, int s, color_t col) {
    static const signed char cs[16][2] = { {64,0},{59,24},{45,45},{24,59},{0,64},{-24,59},{-45,45},{-59,24},
                                           {-64,0},{-59,-24},{-45,-45},{-24,-59},{0,-64},{24,-59},{45,-45},{59,-24} };
    int ph = (int)(ui->now % 800) * 16 / 800;                  /* 0.8 s a turn, 270° arc */
    float r = (s - 2) / 2.0f, cx = x + s / 2.0f, cy = y + s / 2.0f;
    for (int k = 0; k < 12; k++) {
        int a = (ph + k) & 15, b = (ph + k + 1) & 15;
        gfx_line_aa(ui->c, cx + cs[a][0] * r / 64, cy + cs[a][1] * r / 64, cx + cs[b][0] * r / 64, cy + cs[b][1] * r / 64, 2, col);
    }
    wake_at(ui, ui->now + 33);
}

int ui_button_w(int icon, const char *label) {
    int w = 2 * SP_4;
    if (icon >= 0) w += ICON_SM + (label ? 6 : 0);
    if (label) w += gfx_text_width(T_BODY_B, label);
    return w;
}

/* shared press logic: hover / down / clicked (pointer or Enter/Space when focused) */
static bool press(struct ui *ui, uint32_t id, int x, int y, int w, int h, bool dis, bool *hov, bool *down, bool *foc) {
    bool in = !dis && inside(ui, x, y, w, h);              /* forced (gallery) states only change the look */
    *hov = in || (!dis && (ui->force & (UI_F_HOVER | UI_F_PRESS)));
    *foc = focusable(ui, id, dis);
    if (in && ui->pressed) ui->active = id;
    bool clicked = ui->released && ui->active == id && in;
    if (*foc && (take_key(ui, KEY_ENTER) || take_key(ui, ' '))) clicked = true;
    *down = (ui->active == id && (ui->buttons & 1) && in) || (!dis && (ui->force & UI_F_PRESS));
    return clicked;
}

bool ui_button_ex(struct ui *ui, int x, int y, int w, int h, int icon, const char *label, int style) {
    uint32_t id = rid(x, y, w, h);
    int kind = style & 0xFF;
    bool dis = style & UI_DISABLED, hov, down, foc;
    bool clicked = press(ui, id, x, y, w, h, dis || (style & UI_LOADING), &hov, &down, &foc);
    color_t bg = 0, fg = TH.text, ic = TH.text2;
    if (kind == UI_ACCENT_BTN) { bg = down ? TH.accent_press : hov ? TH.accent_hover : TH.accent; fg = ic = TH.on_accent; }
    else if (kind == UI_DANGER_BTN) { bg = TH.danger; fg = ic = TH.on_danger; }
    else if (kind == UI_NORMAL_BTN) bg = TH.raised;
    else if (style & UI_ON) { bg = TH.selection; ic = TH.accent_text; }
    if (hov && !label) ic = TH.text;
    if (ALPHA(bg)) gfx_round_fill(ui->c, x, y, w, h, R_2, fade(bg, dis));
    if (kind == UI_NORMAL_BTN) gfx_round_rect(ui->c, x, y, w, h, R_2, fade(TH.line_strong, dis));
    if (kind != UI_ACCENT_BTN && (down || hov)) gfx_round_fill(ui->c, x, y, w, h, R_2, down ? TH.press_ov : TH.hover_ov);
    if (dis) fg = ic = kind == UI_ACCENT_BTN || kind == UI_DANGER_BTN ? fade(fg, 1) : TH.disabled;
    const struct font *f = T_BODY_B;
    if (style & UI_LOADING) spinner(ui, x + (w - 14) / 2, y + (h - 14) / 2, 14, fg);
    else {
        int cw = ui_button_w(icon, label) - 2 * SP_4, cx = x + (w - cw) / 2;
        struct canvas cc = *ui->c;
        canvas_clip(&cc, x, y, w, h);
        if (icon >= 0) { struct canvas *o = ui->c; ui->c = &cc; ui_icon(ui, cx, y + (h - ICON_SM) / 2, icon, ICON_SM, ic); ui->c = o; cx += ICON_SM + 6; }
        if (label) gfx_text(&cc, f, cx, y + (h - f->height) / 2, label, fg);
    }
    ring(ui, foc, x, y, w, h, R_2);
    return clicked && !dis;
}

bool ui_button(struct ui *ui, int x, int y, int w, int h, const char *label, int style) {
    return ui_button_ex(ui, x, y, w, h, UI_NOICON, label, style);
}

bool ui_icon_button(struct ui *ui, int x, int y, int icon, int flags, const char *tip) {
    bool r = ui_button_ex(ui, x, y, H_CTL, H_CTL, icon, NULL, UI_GHOST_BTN | flags);
    if (tip) ui_tooltip(ui, x, y, H_CTL, H_CTL, tip);
    return r;
}

bool ui_tool(struct ui *ui, int x, int y, int w, int h, const char *label, bool on) {
    uint32_t id = rid(x, y, w, h);
    bool hov, down, foc, clicked = press(ui, id, x, y, w, h, false, &hov, &down, &foc);
    if (on) gfx_round_fill(ui->c, x, y, w, h, R_2, TH.selection);
    if (down || hov) gfx_round_fill(ui->c, x, y, w, h, R_2, down ? TH.press_ov : TH.hover_ov);
    const struct font *f = on ? T_BODY_B : T_BODY;
    struct canvas cc = *ui->c;
    canvas_clip(&cc, x, y, w, h);
    gfx_text_center(&cc, f, x, y + (h - f->height) / 2, w, label, TH.text);
    ring(ui, foc, x, y, w, h, R_2);
    return clicked;
}

/* ---- toggles ---- */
static void check_mark(struct ui *ui, int x, int y, color_t col) {     /* 12px check, 2px stroke, in a 16px box */
    gfx_line_aa(ui->c, x + 3.5f, y + 8.2f, x + 6.5f, y + 11.2f, 2, col);
    gfx_line_aa(ui->c, x + 6.5f, y + 11.2f, x + 12.5f, y + 5, 2, col);
}

/* an accent fill; on light themes text-less fills get a 1px accent_line rim (§2.3) */
static void accent_fill(struct ui *ui, int x, int y, int w, int h, int r, bool dis) {
    gfx_round_fill(ui->c, x, y, w, h, r, fade(TH.accent, dis));
    if (!TH.dark) gfx_round_rect(ui->c, x, y, w, h, r, fade(TH.accent_line, dis));
}

static color_t edge(bool hov) { return hov ? mix(TH.control, TH.text, 80) : TH.control; }

static bool toggle_row(struct ui *ui, uint32_t id, int x, int y, const char *label, int flags, bool *hov, bool *foc) {
    int w = CHECK + (label ? SP_3 + gfx_text_width(T_BODY, label) : 0);
    bool dis = flags & UI_DISABLED, down;
    bool c = press(ui, id, x, y, w, H_ROW, dis, hov, &down, foc);
    if (label) gfx_text(ui->c, T_BODY, x + CHECK + SP_3, y + (H_ROW - T_BODY->height) / 2, label, dis ? TH.disabled : TH.text);
    return c && !dis;
}

bool ui_checkbox_ex(struct ui *ui, int x, int y, const char *label, bool *v, int flags) {
    uint32_t id = rid(x, y, CHECK, 1);
    bool hov, foc, dis = flags & UI_DISABLED, c = toggle_row(ui, id, x, y, label, flags, &hov, &foc);
    if (c) *v = !*v;
    int by = y + (H_ROW - CHECK) / 2;
    if (*v || (flags & UI_MIXED)) {
        accent_fill(ui, x, by, CHECK, CHECK, R_2, dis);
        if (flags & UI_MIXED) gfx_fill(ui->c, x + 4, by + 7, 8, 2, fade(TH.on_accent, dis));
        else check_mark(ui, x, by, fade(TH.on_accent, dis));
    } else {
        gfx_round_fill(ui->c, x, by, CHECK, CHECK, R_2, fade(TH.raised, dis));
        gfx_round_rect(ui->c, x, by, CHECK, CHECK, R_2, fade(edge(hov), dis));
    }
    ring(ui, foc, x, by, CHECK, CHECK, R_2);
    return c;
}
bool ui_checkbox(struct ui *ui, int x, int y, const char *label, bool *v) { return ui_checkbox_ex(ui, x, y, label, v, 0); }

bool ui_radio(struct ui *ui, int x, int y, const char *label, int *sel, int val, int flags) {
    uint32_t id = rid(x, y, CHECK, 2);
    bool hov, foc, dis = flags & UI_DISABLED, c = toggle_row(ui, id, x, y, label, flags, &hov, &foc);
    if (c) *sel = val;
    int by = y + (H_ROW - CHECK) / 2;
    if (*sel == val) {
        accent_fill(ui, x, by, CHECK, CHECK, CHECK / 2, dis);
        gfx_round_fill(ui->c, x + 5, by + 5, 6, 6, 3, fade(TH.on_accent, dis));
    } else {
        gfx_round_fill(ui->c, x, by, CHECK, CHECK, CHECK / 2, fade(TH.raised, dis));
        gfx_round_rect(ui->c, x, by, CHECK, CHECK, CHECK / 2, fade(edge(hov), dis));
    }
    ring(ui, foc, x, by, CHECK, CHECK, CHECK / 2);
    return c;
}

/* switch and slider thumbs: E1 shadow; light thumbs get a 1px control ring */
static void thumb(struct ui *ui, int x, int y, int s, color_t col, color_t rim, bool dis) {
    if (!dis) gfx_elev(ui->c, x, y, s, s, s / 2, E1, TH.elev[E1]);
    gfx_round_fill(ui->c, x, y, s, s, s / 2, fade(col, dis));
    if (ALPHA(rim)) gfx_round_rect(ui->c, x, y, s, s, s / 2, fade(rim, dis));
}

bool ui_switch(struct ui *ui, int x, int y, bool *v, int flags) {
    uint32_t id = rid(x, y, SWITCH_W, SWITCH_H);
    bool dis = flags & UI_DISABLED, hov, down, foc;
    bool c = press(ui, id, x, y - 2, SWITCH_W, SWITCH_H + 4, dis, &hov, &down, &foc) && !dis;
    if (c) *v = !*v;
    if (*v) accent_fill(ui, x, y, SWITCH_W, SWITCH_H, SWITCH_H / 2, dis);
    else gfx_round_fill(ui->c, x, y, SWITCH_W, SWITCH_H, SWITCH_H / 2, fade(TH.control, dis));
    if (hov) gfx_round_fill(ui->c, x, y, SWITCH_W, SWITCH_H, SWITCH_H / 2, TH.hover_ov);
    int in = (SWITCH_H - SWITCH_THUMB) / 2, tx = tween(ui, id, *v ? SWITCH_W - SWITCH_H : 0, D_NORMAL);
    thumb(ui, x + in + tx, y + in, SWITCH_THUMB, TH.thumb, TH.dark ? 0 : TH.control, dis);
    ring(ui, foc, x, y, SWITCH_W, SWITCH_H, SWITCH_H / 2);
    return c;
}

bool ui_slider_ex(struct ui *ui, int x, int y, int w, int *v, int min, int max, int flags) {
    uint32_t id = rid(x, y, w, 20);
    int h = HIT_MIN, old = *v, span = max - min;
    bool dis = flags & UI_DISABLED, in = !dis && inside(ui, x - 8, y, w + 16, h), hov = in || (!dis && ui->force), foc = focusable(ui, id, dis);
    if (in && ui->pressed) ui->active = id;
    if (ui->active == id && (ui->buttons & 1)) *v = min + span * imin(imax(ui->mx - x, 0), w) / (w ? w : 1);
    if (foc) {
        int st = span >= 100 ? span / 100 : 1;
        if (take_key(ui, KEY_LEFT) || take_key(ui, KEY_DOWN)) *v -= st;
        if (take_key(ui, KEY_RIGHT) || take_key(ui, KEY_UP)) *v += st;
        if (take_key(ui, KEY_PGDN)) *v -= 10 * st;
        if (take_key(ui, KEY_PGUP)) *v += 10 * st;
        if (take_key(ui, KEY_HOME)) *v = min;
        if (take_key(ui, KEY_END)) *v = max;
        *v = imin(imax(*v, min), max);
    }
    int px = x + (span > 0 ? (*v - min) * w / span : 0), ty = y + (h - SLIDER_TRACK) / 2;
    gfx_round_fill(ui->c, x, ty, w, SLIDER_TRACK, SLIDER_TRACK / 2, fade(TH.control, dis));
    if (px > x) accent_fill(ui, x, ty, px - x, SLIDER_TRACK, SLIDER_TRACK / 2, dis);
    int s = (hov || ui->active == id) ? SLIDER_THUMB_HOT : SLIDER_THUMB;
    /* spec: a white thumb; the high-contrast theme's thumb token is black, keep it */
    color_t tc = (TH.thumb & 0xFFFFFF) < 0x808080 ? TH.thumb : 0xFFFFFFFFu;
    thumb(ui, px - s / 2, y + (h - s) / 2, s, tc, TH.line_strong, dis);
    ring(ui, foc, px - s / 2, y + (h - s) / 2, s, s, s / 2);
    return *v != old;
}
bool ui_slider(struct ui *ui, int x, int y, int w, int *v, int min, int max) { return ui_slider_ex(ui, x, y, w, v, min, max, 0); }

/* segmented control: base well, 2px padding and gaps, the selected segment a raised pill with E1 */
static bool segs(struct ui *ui, int x, int y, int h, const int *ws, const int *icons, const char *const *names, int n, int *sel) {
    int old = *sel, tw = SP_1;
    for (int i = 0; i < n; i++) tw += ws[i] + SP_1;
    uint32_t id = rid(x, y, tw, h);
    bool foc = focusable(ui, id, false);
    if (foc) {
        if (take_key(ui, KEY_LEFT) && *sel > 0) (*sel)--;
        if (take_key(ui, KEY_RIGHT) && *sel < n - 1) (*sel)++;
    }
    gfx_round_fill(ui->c, x, y, tw, h, R_2, TH.base);
    for (int i = 0, sx = x + SP_1; i < n; sx += ws[i++] + SP_1) {
        int sh = h - 2 * SP_1;
        bool hov = inside(ui, sx, y, ws[i], h), on = i == *sel;
        if (hov && ui->pressed) *sel = i;
        if (on) { gfx_elev(ui->c, sx, y + SP_1, ws[i], sh, 3, E1, TH.elev[E1]); gfx_round_fill(ui->c, sx, y + SP_1, ws[i], sh, 3, TH.raised); }
        else if (hov) gfx_round_fill(ui->c, sx, y + SP_1, ws[i], sh, 3, TH.hover_ov);
        color_t col = on || hov ? TH.text : TH.text2;
        int cw = (icons && icons[i] >= 0 ? ICON_SM : 0) + (names && names[i] ? gfx_text_width(T_BODY, names[i]) + (icons && icons[i] >= 0 ? 6 : 0) : 0);
        int cx = sx + (ws[i] - cw) / 2;
        if (icons && icons[i] >= 0) { ui_icon(ui, cx, y + (h - ICON_SM) / 2, icons[i], ICON_SM, col); cx += ICON_SM + 6; }
        if (names && names[i]) {
            struct canvas cc = *ui->c;
            canvas_clip(&cc, sx, y, ws[i], h);
            gfx_text(&cc, T_BODY, cx, y + (h - T_BODY->height) / 2, names[i], col);
        }
    }
    ring(ui, foc, x, y, tw, h, R_2);
    return *sel != old;
}

bool ui_tabs(struct ui *ui, int x, int y, int w, int h, const char *const *names, int n, int *sel) {
    int ws[8];
    n = imin(n, 8);
    for (int i = 0, left = w - SP_1; i < n; i++) { ws[i] = (left - SP_1 * (n - i)) / (n - i); left -= ws[i] + SP_1; }
    return segs(ui, x, y, h, ws, NULL, names, n, sel);
}

bool ui_segmented(struct ui *ui, int x, int y, const int *icons, const char *const *names, int n, int *sel) {
    int ws[8];
    n = imin(n, 8);
    for (int i = 0; i < n; i++)
        ws[i] = names && names[i] ? gfx_text_width(T_BODY, names[i]) + 2 * SP_4 + (icons && icons[i] >= 0 ? ICON_SM + 6 : 0) : H_CTL;
    return segs(ui, x, y, H_CTL, ws, icons, names, n, sel);
}

bool ui_tabbar(struct ui *ui, int x, int y, int w, const char *const *names, int n, int *sel, int flags) {
    int old = *sel, tx = x + SP_2;
    bool off = (flags & UI_INACTIVE) || !ui->focused;
    uint32_t id = rid(x, y, w, H_TAB);
    bool foc = focusable(ui, id, false);
    if (foc) {
        if (take_key(ui, KEY_LEFT) && *sel > 0) (*sel)--;
        if (take_key(ui, KEY_RIGHT) && *sel < n - 1) (*sel)++;
    }
    gfx_fill(ui->c, x, y, w, H_TAB, off ? TH.base : TH.window);
    gfx_hline(ui->c, x, y + H_TAB - 1, w, TH.line);
    int lx = 0, lw = 0;
    for (int i = 0; i < n; i++) {
        int tw = gfx_text_width(T_BODY_B, names[i]) + 2 * SP_4;
        bool hov = inside(ui, tx, y, tw, H_TAB), on = i == *sel;
        if (hov && ui->pressed) *sel = i;
        if (hov && !on) gfx_round_fill(ui->c, tx, y + SP_1, tw, H_TAB - 2 * SP_1 - 1, R_2, TH.hover_ov);
        gfx_text(ui->c, on ? T_BODY_B : T_BODY, tx + SP_4, y + (H_TAB - T_BODY->height) / 2, names[i], on || hov ? TH.text : TH.text2);
        if (i == *sel) { lx = tx + SP_4; lw = tw - 2 * SP_4; }
        if (foc && on) ring(ui, true, tx, y + SP_1, tw, H_TAB - 2 * SP_1 - 1, R_2);
        tx += tw;
    }
    if (lw) {                                                    /* the signal line slides between tabs */
        int ax = tween(ui, id, lx, D_NORMAL), aw = tween(ui, id + 1, lw, D_NORMAL);
        gfx_round_fill(ui->c, ax, y + H_TAB - 1 - SIGNAL, aw, SIGNAL, R_1, off ? TH.text3 : TH.accent_line);
    }
    return *sel != old;
}

bool ui_dropdown(struct ui *ui, int x, int y, int w, const char *const *items, int n, int *sel, int flags) {
    uint32_t id = rid(x, y, w, H_CTL);
    bool dis = flags & UI_DISABLED, hov, down, foc, open = ui->pop_id == id, changed = false;
    bool c = press(ui, id, x, y, w, H_CTL, dis, &hov, &down, &foc);
    if (foc && !open && (take_key(ui, KEY_DOWN) || take_key(ui, KEY_UP))) c = true;
    if (ui->pop_pick >= 0 && ui->focus == id && !ui->pop_id) {   /* a pick from the menu this frame */
        changed = ui->pop_pick != *sel; *sel = ui->pop_pick; ui->pop_pick = -1;
    }
    if (c && !dis && !open) {
        ui->pop_id = id; ui->pop_items = items; ui->pop_n = n; ui->pop_sel = *sel; ui->pop_hi = imax(*sel, 0);
        ui->pop_anchor = (struct rect){ x, y, w, H_CTL }; ui->focus = id;
        open = true;
    }
    gfx_round_fill(ui->c, x, y, w, H_CTL, R_2, fade(TH.raised, dis));
    gfx_round_rect(ui->c, x, y, w, H_CTL, R_2, fade(TH.line_strong, dis));
    if (open || down || hov) gfx_round_fill(ui->c, x, y, w, H_CTL, R_2, open || down ? TH.press_ov : TH.hover_ov);
    struct canvas cc = *ui->c;
    canvas_clip(&cc, x, y, w - SP_3 - ICON_SM, H_CTL);
    struct canvas *o = ui->c;
    ui->c = &cc;
    if (*sel >= 0 && *sel < n) ui_text_fit(ui, T_BODY, x + SP_4, y + (H_CTL - T_BODY->height) / 2, w - SP_4 - SP_3 - ICON_SM - SP_2, items[*sel], dis ? TH.disabled : TH.text);
    ui->c = o;
    ui_icon(ui, x + w - SP_3 - ICON_SM, y + (H_CTL - ICON_SM) / 2, ICON_CHEVRON_DOWN, ICON_SM, dis ? TH.disabled : TH.text2);
    if (open) { ui->pop_anchor = (struct rect){ x, y, w, H_CTL }; ui->pop_seen = true; }
    ring(ui, foc && !open, x, y, w, H_CTL, R_2);
    return changed;
}

/* ---- text entry ---- */
static int prev_cp(const char *s, int i) { do i--; while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80); return imax(i, 0); }
static int next_cp(const char *s, int i) { if (!s[i]) return i; do i++; while (s[i] && ((uint8_t)s[i] & 0xC0) == 0x80); return i; }

static void ins(char *buf, int cap, int *caret, const char *s, int k) {
    int n = strlen(buf);
    if (n + k >= cap) k = cap - 1 - n;
    if (k <= 0) return;
    memmove(buf + *caret + k, buf + *caret, n - *caret + 1);
    memcpy(buf + *caret, s, k);
    *caret += k;
}

static bool edit(struct ui *ui, char *buf, int cap) {
    int *cr = &ui->caret, n = strlen(buf);
    bool enter = false;
    if (*cr > n) *cr = n;
    for (int i = 0; i < ui->nkeys; ) {
        struct gui_event k = ui->keys[i];
        int eat = 1;
        bool ctrl = k.mods & MOD_CTRL;
        if (k.key == KEY_ENTER) enter = true;
        else if (k.key == KEY_BACKSPACE) { if (*cr) { int p = prev_cp(buf, *cr); memmove(buf + p, buf + *cr, strlen(buf + *cr) + 1); *cr = p; } }
        else if (k.key == KEY_DELETE) { int q = next_cp(buf, *cr); memmove(buf + *cr, buf + q, strlen(buf + q) + 1); }
        else if (k.key == KEY_LEFT) *cr = prev_cp(buf, *cr);
        else if (k.key == KEY_RIGHT) *cr = next_cp(buf, *cr);
        else if (k.key == KEY_HOME) *cr = 0;
        else if (k.key == KEY_END) *cr = strlen(buf);
        else if (k.ch == 21) { buf[0] = 0; *cr = 0; }                       /* ^U */
        else if (ctrl && (k.key | 0x20) == 'v') {                          /* paste the first line */
            char *c = gui_clip_get(NULL);
            for (char *p = c; p && *p && *p != '\n'; p++) if ((uint8_t)*p >= 32) ins(buf, cap, cr, p, 1);
            free(c);
        }
        else if (ctrl && (k.key | 0x20) == 'c') gui_clip_set(buf, strlen(buf));
        else if (!ctrl && k.ch >= 32 && k.ch < 127) { char ch = k.ch; ins(buf, cap, cr, &ch, 1); }
        else eat = 0;                                                      /* Tab, Esc, Up/Down... stay for others */
        if (eat) { memmove(ui->keys + i, ui->keys + i + 1, (--ui->nkeys - i) * sizeof k); ui->caret_t0 = ui->now; }
        else i++;
        if (enter) break;
    }
    return enter;
}

bool ui_input(struct ui *ui, int x, int y, int w, int h, char *buf, int cap, const char *placeholder, int icon, int flags) {
    uint32_t id = rid(x, y, w, h);
    bool dis = flags & UI_DISABLED, in = !dis && inside(ui, x, y, w, h), hov = in || (!dis && (ui->force & UI_F_HOVER));
    int tx0 = x + SP_3 + (icon >= 0 ? ICON_SM + SP_3 : 0), tw0 = w - (tx0 - x) - SP_3;
    if (ui->pressed && in) ui->focus = id;
    bool foc = focusable(ui, id, dis), enter = false;
    const struct font *f = T_BODY;
    if (foc && ui->caret_id != id) { ui->caret_id = id; ui->caret = strlen(buf); ui->scroll_x = 0; ui->caret_t0 = ui->now; }
    if (foc && ui->pressed && in) {                                /* click places the caret */
        int n = strlen(buf), i = 0, best = 0, bd = 1 << 30;
        for (; i <= n; i = next_cp(buf, i)) {
            int d = abs(tx0 - ui->scroll_x + gfx_text_width_n(f, buf, i) - ui->mx);
            if (d < bd) { bd = d; best = i; }
            if (i == n) break;
        }
        ui->caret = best; ui->caret_t0 = ui->now;
    }
    if (foc) enter = edit(ui, buf, cap);
    gfx_round_fill(ui->c, x, y, w, h, R_2, fade(TH.raised, dis));
    if (foc || (ui->force & UI_F_FOCUS)) { gfx_round_rect(ui->c, x, y, w, h, R_2, TH.accent_text); gfx_ring_out(ui->c, x, y, w, h, R_2, TH.accent_text); }
    else gfx_round_rect(ui->c, x, y, w, h, R_2, fade(flags & UI_ERROR ? TH.danger : edge(hov), dis));
    if (icon >= 0) ui_icon(ui, x + SP_3, y + (h - ICON_SM) / 2, icon, ICON_SM, dis ? TH.disabled : TH.text2);
    struct canvas cc = *ui->c;
    canvas_clip(&cc, tx0 - 1, y, tw0 + 2, h);
    int ty = y + (h - f->height) / 2;
    if (foc) {                                                     /* keep the caret in view */
        int cx = gfx_text_width_n(f, buf, ui->caret);
        if (cx - ui->scroll_x > tw0) ui->scroll_x = cx - tw0;
        if (cx - ui->scroll_x < 0) ui->scroll_x = cx;
        if (gfx_text_width(f, buf) - ui->scroll_x < tw0) ui->scroll_x = imax(gfx_text_width(f, buf) - tw0, 0);
    }
    int sx = foc ? ui->scroll_x : 0;
    if (!buf[0] && placeholder) gfx_text(&cc, f, tx0, ty, placeholder, dis ? TH.disabled : TH.text3);
    gfx_text(&cc, f, tx0 - sx, ty, buf, dis ? TH.disabled : TH.text);
    if (foc) {                                                     /* caret: 1x16, blinks D_CARET, steady after D_CARET_IDLE */
        uint32_t e = ui->now - ui->caret_t0;
        if (e >= D_CARET_IDLE || (e / D_CARET) % 2 == 0) gfx_fill(&cc, tx0 - sx + gfx_text_width_n(f, buf, ui->caret), y + (h - 16) / 2, 1, 16, TH.text);
        if (e < D_CARET_IDLE) wake_at(ui, ui->now + D_CARET - e % D_CARET);
    }
    return enter;
}

bool ui_textbox(struct ui *ui, int x, int y, int w, int h, char *buf, int cap, const char *placeholder) {
    return ui_input(ui, x, y, w, h, buf, cap, placeholder, UI_NOICON, 0);
}

bool ui_search(struct ui *ui, int x, int y, int w, char *buf, int cap, const char *placeholder) {
    int cb = x + w - SP_2 - 20;                                    /* 20px clear box */
    bool clr = buf[0] && inside(ui, cb, y + (H_SEARCH - 20) / 2, 20, 20);
    if (clr && ui->pressed) { buf[0] = 0; ui->caret = 0; ui->focus = rid(x, y, w, H_SEARCH); ui->pressed = false; }
    if (buf[0] && ui->focus == rid(x, y, w, H_SEARCH) && take_key(ui, KEY_ESC)) { buf[0] = 0; ui->caret = 0; }
    bool r = ui_input(ui, x, y, w, H_SEARCH, buf, cap, placeholder, ICON_SEARCH, 0);
    if (buf[0]) {
        if (clr) gfx_round_fill(ui->c, cb, y + (H_SEARCH - 20) / 2, 20, 20, R_2, TH.hover_ov);
        ui_icon(ui, cb + 2, y + (H_SEARCH - ICON_SM) / 2, ICON_CLOSE, ICON_SM, clr ? TH.text : TH.text3);
    }
    return r;
}

/* ---- lists and navigation ---- */
static void scrollbar(struct ui *ui, uint32_t id, int x, int y, int h, int total, int visible, int *first, bool page) {
    int range = total - visible;
    if (range <= 0) { *first = 0; return; }
    int th = imax(h * visible / total, SCROLL_MIN), span = h - th;
    bool hov = inside(ui, x, y, 12, h);
    if (*first < 0) *first = 0;
    if (*first > range) *first = range;
    int ty = y + (span > 0 ? span * *first / range : 0);
    if (hov && ui->pressed) {
        ui->active = id;
        if (ui->my < ty || ui->my >= ty + th) {                    /* the track pages */
            if (page) *first += ui->my < ty ? -visible : visible;
            ui->active = 0;
        } else ui->drag_off = ui->my - ty;
    }
    bool drag = ui->active == id && (ui->buttons & 1);
    if (drag) { int t = ui->my - y - ui->drag_off; *first = span > 0 ? imin(imax(t, 0), span) * range / span : 0; }
    *first = imin(imax(*first, 0), range);
    ty = y + (span > 0 ? span * *first / range : 0);
    struct tw *t = 0;                                              /* last time the position changed or the bar was used */
    int a = tween(ui, id + 7, *first, 0);
    (void)a;
    for (int i = 0; i < 48 && !t; i++) if (tws[i].id == id + 7) t = &tws[i];
    uint32_t seen = t ? t->t0 : 0;
    if (hov || drag) { if (t) t->t0 = ui->now; seen = ui->now; }
    int e = (int)(ui->now - seen), al = e < D_SCROLL_FADE ? 256 : e < D_SCROLL_FADE + D_SLOW ? 256 - (e - D_SCROLL_FADE) * 256 / D_SLOW : 0;
    if (!ui_motion && e >= D_SCROLL_FADE) al = 0;
    if (ui->force) al = 256;
    if (al && al < 256) wake_at(ui, ui->now + 33);
    else if (al) wake_at(ui, seen + D_SCROLL_FADE);
    if (!al) return;
    bool hot = hov || drag || (ui->force & UI_F_HOVER);
    if (hot) gfx_round_fill(ui->c, x, y, 12, h, 6, TH_A(TH.text, 15 * al / 256));
    int bw = hot ? SCROLL_W_HOT : SCROLL_W;
    gfx_round_fill(ui->c, x + 12 - 2 - bw, ty, bw, th, bw / 2, TH_A(TH.text, (drag ? 140 : 89) * al / 256));
}

void ui_vscroll(struct ui *ui, int x, int y, int h, int total, int visible, int *first) {
    scrollbar(ui, rid(x, y, 10, h), x, y, h, total, visible, first, true);
}

int ui_list(struct ui *ui, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll) {
    const int rh = H_ROW;
    int visible = h / rh, act = -1;
    uint32_t id = rid(x, y, w, h);
    bool hov = inside(ui, x, y, w, h);
    if (ui->pressed && hov) ui->focus = id;
    bool foc = focusable(ui, id, false);
    if (hov && ui->scroll) *scroll += ui->scroll * 3;
    if (foc) {
        for (int i = 0; i < ui->nkeys; ) {
            int k = ui->keys[i].key, s0 = *sel, eat = 1;
            if (k == KEY_DOWN && *sel < n - 1) (*sel)++;
            else if (k == KEY_UP && *sel > 0) (*sel)--;
            else if (k == KEY_PGDN) *sel = imin(*sel + visible, n - 1);
            else if (k == KEY_PGUP) *sel = imax(*sel - visible, 0);
            else if (k == KEY_HOME) *sel = 0;
            else if (k == KEY_END) *sel = n - 1;
            else if (k == KEY_ENTER && *sel >= 0) act = *sel;
            else if (k != KEY_DOWN && k != KEY_UP) eat = 0;              /* leave it for the app */
            if (!eat) { i++; continue; }
            memmove(ui->keys + i, ui->keys + i + 1, (--ui->nkeys - i) * sizeof ui->keys[0]);
            if (*sel != s0) ui->kbd = true;
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
        bool rh_ = inside(ui, x, ry, w - 12, rh) && ry + rh <= y + h;
        if (rh_ && ui->pressed) { *sel = i; if (ui->dbl) act = i; }
        if (i == *sel) gfx_round_fill(&cc, x + SP_2, ry, w - 2 * SP_2, rh, R_2, TH.selection);
        else if (rh_) gfx_round_fill(&cc, x + SP_2, ry, w - 2 * SP_2, rh, R_2, TH.hover_ov);
        if (i == *sel && foc && ui->kbd) gfx_rrect_ring(&cc, x + SP_2, ry, w - 2 * SP_2, rh, RADII(R_2, R_2, R_2, R_2), 2, TH.accent_text);
        struct canvas *o = ui->c;
        ui->c = &cc;
        ui_text_fit(ui, i == *sel ? T_BODY_B : T_BODY, x + SP_4, ry + (rh - T_BODY->height) / 2, w - 2 * SP_4 - 8, items[i], TH.text);
        ui->c = o;
    }
    int first = *scroll;
    scrollbar(ui, id + 3, x + w - 12, y, h, n, visible, &first, true);
    *scroll = first;
    return act;
}

bool ui_row(struct ui *ui, int x, int y, int w, int icon, const struct aicon *ai, const char *title, const char *sub,
            const char *trail, int flags) {
    int h = sub ? H_ROW2 : H_ROW, is = sub && ai ? ICON_XL : ICON_SM, gap = sub && ai ? SP_4 : SP_3;
    bool sel = flags & UI_SELECTED, in = inside(ui, x, y, w, h), hov = in || (ui->force & UI_F_HOVER);
    if (sel) gfx_round_fill(ui->c, x, y, w, h, R_2, TH.selection);
    else if (hov) gfx_round_fill(ui->c, x, y, w, h, R_2, TH.hover_ov);
    int tx = x + SP_4, tr = trail ? gfx_text_width(T_CAPTION, trail) + SP_3 : 0;
    if (ai) { ui_aicon(ui, tx, y + (h - ai->h) / 2, ai); tx += ai->w + gap; }
    else if (icon >= 0) { ui_icon(ui, tx, y + (h - is) / 2, icon, ICON_SM, sel ? TH.accent_text : TH.text2); tx += ICON_SM + gap; }
    int tw = x + w - SP_4 - tr - tx;
    if (sub) {
        int ty = y + (h - T_BODY->height - T_CAPTION->height) / 2;
        ui_text_fit(ui, sel ? T_BODY_B : T_BODY, tx, ty, tw, title, TH.text);
        ui_text_fit(ui, T_CAPTION, tx, ty + T_BODY->height, tw, sub, TH.text2);
    } else ui_text_fit(ui, sel ? T_BODY_B : T_BODY, tx, y + (h - T_BODY->height) / 2, tw, title, TH.text);
    if (trail) gfx_text(ui->c, T_CAPTION, x + w - SP_4 - tr + SP_3, y + (h - T_CAPTION->height) / 2, trail, TH.text3);
    return in && ui->pressed;
}

void ui_sidebar(struct ui *ui, int x, int y, int w, int h) { gfx_fill(ui->c, x, y, w, h, TH.base); }

void ui_side_label(struct ui *ui, int x, int y, const char *s) { gfx_text(ui->c, T_CAPTION_B, x + SP_5, y, s, TH.text2); }

bool ui_side_item(struct ui *ui, int x, int y, int w, int icon, const char *label, const char *trail, bool sel) {
    int bx = x + SP_3, bw = w - 2 * SP_3;
    uint32_t id = rid(bx, y, bw, H_ROW);
    bool in = inside(ui, bx, y, bw, H_ROW), hov = in || (ui->force & UI_F_HOVER), foc = focusable(ui, id, false);
    bool down = in && (ui->buttons & 1);
    bool c = (in && ui->pressed) || (foc && (take_key(ui, KEY_ENTER) || take_key(ui, ' ')));    /* select on press, like rows */
    if (sel) gfx_round_fill(ui->c, bx, y, bw, H_ROW, R_2, TH.selection);
    else if (hov) gfx_round_fill(ui->c, bx, y, bw, H_ROW, R_2, down ? TH.press_ov : TH.hover_ov);
    if (sel) gfx_round_fill(ui->c, bx, y + (H_ROW - MARKER_H) / 2, MARKER_W, MARKER_H, R_1, TH.accent_line);
    int tx = bx + SP_3, tr = trail ? gfx_text_width(T_CAPTION, trail) + SP_3 : 0;
    if (icon >= 0) { ui_icon(ui, tx, y + (H_ROW - ICON_SM) / 2, icon, ICON_SM, sel ? TH.accent_text : TH.text2); tx += ICON_SM + SP_3; }
    ui_text_fit(ui, sel ? T_BODY_B : T_BODY, tx, y + (H_ROW - T_BODY->height) / 2, bx + bw - SP_3 - tr - tx, label, TH.text);
    if (trail) gfx_text(ui->c, T_CAPTION, bx + bw - SP_3 - tr + SP_3, y + (H_ROW - T_CAPTION->height) / 2, trail, TH.text3);
    if ((foc && ui->kbd) || (ui->force & UI_F_FOCUS)) gfx_rrect_ring(ui->c, bx, y, bw, H_ROW, RADII(R_2, R_2, R_2, R_2), 2, TH.accent_text);
    return c;
}

void ui_toolbar(struct ui *ui, int x, int y, int w) {
    gfx_fill(ui->c, x, y, w, H_TOOLBAR - 1, TH.window);
    gfx_hline(ui->c, x, y + H_TOOLBAR - 1, w, TH.line);
}

void ui_vsep(struct ui *ui, int x, int y, int h) { gfx_vline(ui->c, x, y, h, TH.line); }

int ui_crumbs(struct ui *ui, int x, int y, int maxw, const char *const *parts, int n) {
    int hit = -1, cx = x;
    struct canvas cc = *ui->c, *o = ui->c;
    canvas_clip(&cc, x, y - 4, maxw, H_CTL + 8);
    ui->c = &cc;
    for (int i = 0; i < n; i++) {
        bool last = i == n - 1;
        const struct font *f = last ? T_BODY_B : T_BODY;
        int w = gfx_text_width(f, parts[i]) + 2 * SP_3;
        if (!last) {
            if (ui_button_ex(ui, cx, y, w, H_CTL, UI_NOICON, NULL, UI_GHOST_BTN)) hit = i;
            gfx_text(ui->c, f, cx + SP_3, y + (H_CTL - f->height) / 2, parts[i], TH.text2);
            ui_icon(ui, cx + w, y + (H_CTL - ICON_SM) / 2, ICON_CHEVRON_RIGHT, ICON_SM, TH.text3);
            cx += w + ICON_SM;
        } else gfx_text(ui->c, f, cx + SP_3, y + (H_CTL - f->height) / 2, parts[i], TH.text);
    }
    ui->c = o;
    return hit;
}

void ui_status(struct ui *ui, int x, int y, int w, const char *left, const char *right) {
    gfx_fill(ui->c, x, y, w, H_STATUS, TH.window);
    gfx_hline(ui->c, x, y, w, TH.line);
    int ty = y + (H_STATUS - T_CAPTION->height) / 2, rw = right ? gfx_text_width(T_CAPTION, right) : 0;
    if (left) ui_text_fit(ui, T_CAPTION, x + SP_4, ty, w - 2 * SP_4 - rw - SP_4, left, TH.text2);
    if (right) gfx_text(ui->c, T_CAPTION, x + w - SP_4 - rw, ty, right, TH.text2);
}

/* ---- containers and feedback ---- */
void ui_card(struct ui *ui, int x, int y, int w, int h) {
    gfx_round_fill(ui->c, x, y, w, h, R_3, TH.raised);
    gfx_ring_out(ui->c, x, y, w, h, R_3, TH.line);
}
void ui_panel(struct ui *ui, int x, int y, int w, int h) { ui_card(ui, x, y, w, h); }

void ui_card_row(struct ui *ui, int x, int y, int w, int h, const char *label, const char *desc, bool first) {
    if (!first) gfx_hline(ui->c, x, y, w, TH.line);
    int bh = T_BODY->height + (desc ? T_CAPTION->height : 0), ty = y + (h - bh) / 2;
    gfx_text(ui->c, T_BODY, x + SP_4, ty, label, TH.text);
    if (desc) gfx_text(ui->c, T_CAPTION, x + SP_4, ty + T_BODY->height, desc, TH.text2);
}

void ui_progress(struct ui *ui, int x, int y, int w, int v, int max) {
    gfx_round_fill(ui->c, x, y, w, 4, R_1, TH.line);
    if (v < 0) {                                                   /* a 30% segment sweeping in 1.2 s */
        int sw = w * 3 / 10, p = (int)(ui->now % 1200) * (w + sw) / 1200 - sw;
        struct canvas cc = *ui->c, *o = ui->c;
        canvas_clip(&cc, x, y, w, 4);
        ui->c = &cc;
        accent_fill(ui, x + p, y, sw, 4, R_1, false);
        ui->c = o;
        wake_at(ui, ui->now + 33);
    } else if (max > 0 && v > 0) accent_fill(ui, x, y, imin(v, max) * w / max, 4, R_1, false);
}

void ui_spinner(struct ui *ui, int x, int y) { spinner(ui, x, y, ICON_SM, TH.accent_text); }

bool ui_empty(struct ui *ui, int x, int y, int w, int h, int icon, const char *title, const char *body, const char *btn) {
    int bw = w - 2 * SP_7, bh = body ? ui_text_wrap(ui, T_BODY, 0, 0, bw, body, 0) : 0;
    int th = ICON_APP + SP_5 + T_HEADING->height + (body ? SP_2 + bh : 0) + (btn ? SP_5 + H_CTL : 0);
    int cy = y + (h - th) / 2, cx = x + w / 2;
    ui_icon(ui, cx - ICON_APP / 2, cy, icon, ICON_APP, TH.text3);
    cy += ICON_APP + SP_5;
    gfx_text_center(ui->c, T_HEADING, x, cy, w, title, TH.text);
    cy += T_HEADING->height + SP_2;
    if (body) {                                                    /* centred lines */
        const char *s = body;
        for (int ly = 0; *s && ly < bh; ly += T_BODY->height) {
            int n = 0, fit = 0;
            while (s[n] && s[n] != '\n') {
                int e = n;
                while (s[e] && s[e] != ' ' && s[e] != '\n') e++;
                if (gfx_text_width_n(T_BODY, s, e) > bw && fit) break;
                n = fit = e;
                if (s[n] == ' ') n++;
            }
            if (!fit) fit = n;
            gfx_text_n(ui->c, T_BODY, cx - gfx_text_width_n(T_BODY, s, fit) / 2, cy + ly, s, fit, TH.text2);
            s += imax(n, fit);
            if (*s == '\n') s++;
        }
        cy += bh;
    }
    if (!btn) return false;
    int w2 = ui_button_w(UI_NOICON, btn);
    return ui_button(ui, cx - w2 / 2, cy + SP_5, w2, H_CTL, btn, UI_SECONDARY);
}

void ui_tooltip(struct ui *ui, int x, int y, int w, int h, const char *text) {
    uint32_t id = rid(x, y, w, h);
    if (!inside(ui, x, y, w, h) || (ui->buttons & 1)) {
        if (ui->tip_id == id) { ui->tip_id = 0; if (ui->tip_show) ui->tip_seen = ui->now; ui->tip_show = false; }
        return;
    }
    if (ui->pressed) { ui->tip_id = id; ui->tip_t0 = ui->now + 100000; return; }   /* clicked: no tip until it leaves */
    if (ui->tip_id != id) {
        bool chain = ui->tip_seen && ui->now - ui->tip_seen < D_TOOLTIP_CHAIN;
        ui->tip_id = id; ui->tip_t0 = chain ? ui->now : ui->now + D_TOOLTIP;
    }
    if ((int)(ui->now - ui->tip_t0) >= 0) { ui->tip = text; ui->tip_r = (struct rect){ x, y, w, h }; ui->tip_show = true; ui->tip_seen = ui->now; }
    else wake_at(ui, ui->tip_t0);
}

void ui_toast(struct ui *ui, const char *msg, const char *action) {
    strlcpy(ui->toast_msg, msg, sizeof ui->toast_msg);
    strlcpy(ui->toast_act, action ? action : "", sizeof ui->toast_act);
    ui->toast_until = ui->now + D_TOAST;
    wake_at(ui, ui->now);
}

bool ui_toast_action(struct ui *ui) { bool r = ui->toast_hit; ui->toast_hit = false; return r; }

/* a floating surface: E2 shadow, overlay fill, 1px line outline */
static void surface(struct ui *ui, int x, int y, int w, int h, int r, int lvl) {
    gfx_elev(ui->c, x, y, w, h, r, lvl, TH.elev[lvl]);
    gfx_round_fill(ui->c, x, y, w, h, r, TH.overlay);
    gfx_round_rect(ui->c, x, y, w, h, r, TH.line);
}

static void draw_overlays(struct ui *ui) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    canvas_noclip(c);
    ui->in_ov = true;
    if (ui->toast_until && (int)(ui->toast_until - ui->now) <= 0) ui->toast_until = 0;
    if (ui->toast_until) {
        const struct font *f = T_BODY, *fb = T_BODY_B;
        int tw = gfx_text_width(f, ui->toast_msg), aw = ui->toast_act[0] ? gfx_text_width(fb, ui->toast_act) + 2 * SP_3 : 0;
        int w = imin(SP_4 + tw + (aw ? SP_4 + aw + SP_2 : SP_4), W - 2 * SP_5), h = 36;
        int x = (W - w) / 2, y = H - SP_7 - h;
        ui->toast_r = (struct rect){ x, y, w, h };
        surface(ui, x, y, w, h, R_3, E2);
        ui_text_fit(ui, f, x + SP_4, y + (h - f->height) / 2, w - SP_4 - (aw ? SP_4 + aw + SP_2 : SP_4), ui->toast_msg, TH.text);
        if (aw) {
            int bx = x + w - SP_2 - aw, by = y + (h - H_CTL) / 2;
            ui->toast_btn = (struct rect){ bx, by, aw, H_CTL };
            if (in_rect(ui->toast_btn, ui->mx, ui->my)) gfx_round_fill(c, bx, by, aw, H_CTL, R_2, (ui->buttons & 1) ? TH.press_ov : TH.hover_ov);
            gfx_text(c, fb, bx + SP_3, by + (H_CTL - fb->height) / 2, ui->toast_act, TH.accent_text);
        }
        wake_at(ui, ui->toast_until);
    }
    if (ui->tip_show && ui->tip_id) {
        ui->tip_show = false;
        const struct font *f = T_BODY;
        int w = imin(gfx_text_width(f, ui->tip) + 2 * SP_3, W_TIP_MAX), h = f->height + 2 * SP_2;
        int x = imin(imax(ui->tip_r.x, SP_2), W - w - SP_2), y = ui->tip_r.y + ui->tip_r.h + SP_3;
        if (y + h > H - SP_2) y = ui->tip_r.y - SP_3 - h;
        surface(ui, x, y, w, h, R_2, E2);
        ui_text_fit(ui, f, x + SP_3, y + SP_2, w - 2 * SP_3, ui->tip, TH.text);
    }
    if (ui->pop_id) {                                             /* the dropdown's menu */
        const struct font *f = T_BODY;
        int n = ui->pop_n, w = ui->pop_anchor.w, h = n * H_ROW + 2 * SP_2;
        for (int i = 0; i < n; i++) w = imax(w, gfx_text_width(f, ui->pop_items[i]) + 2 * SP_4 + ICON_SM + SP_3 + 2 * SP_2);
        w = imin(w, W_MENU_MAX);
        int x = imin(ui->pop_anchor.x, W - w - SP_2), y = ui->pop_anchor.y + ui->pop_anchor.h + SP_2;
        if (y + h > H - SP_2) y = imax(ui->pop_anchor.y - SP_2 - h, SP_2);
        ui->pop_r = (struct rect){ x, y, w, h };
        if (in_rect(ui->pop_r, ui->mx, ui->my)) { int i = (ui->my - y - SP_2) / H_ROW; if (i >= 0 && i < n) ui->pop_hi = i; }
        surface(ui, x, y, w, h, R_3, E2);
        struct canvas cc = *c;
        canvas_clip(&cc, x, y + SP_2, w, h - 2 * SP_2);
        ui->c = &cc;
        for (int i = 0; i < n; i++) {
            int ry = y + SP_2 + i * H_ROW;
            if (i == ui->pop_hi) {
                gfx_round_fill(&cc, x + SP_2, ry, w - 2 * SP_2, H_ROW, R_2, TH.hover_ov);
                if (ui->kbd) gfx_rrect_ring(&cc, x + SP_2, ry, w - 2 * SP_2, H_ROW, RADII(R_2, R_2, R_2, R_2), 2, TH.accent_text);
            }
            if (i == ui->pop_sel) ui_icon(ui, x + SP_2 + SP_4, ry + (H_ROW - ICON_SM) / 2, ICON_CHECK, ICON_SM, TH.text);
            int tx = x + SP_2 + SP_4 + ICON_SM + SP_3;
            ui_text_fit(ui, f, tx, ry + (H_ROW - f->height) / 2, x + w - SP_2 - SP_4 - tx, ui->pop_items[i], TH.text);
        }
        ui->c = c;
    }
    ui->in_ov = false;
}

/* ---- dialogs ---- */
static int dialog_frame(struct ui *ui, int pw, int ph, int *ox, int *oy) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    bool first = !ui->modal_prev;
    ui->modal = true;
    gfx_fill(c, 0, 0, W, H, TH.scrim);
    int px = (W - pw) / 2, py = (H - ph) / 2;
    surface(ui, px, py, pw, ph, R_3, E4);
    *ox = px; *oy = py;
    ui->in_ov = true;
    return first;
}

static int dialog_buttons(struct ui *ui, int px, int py, int pw, int ph, const char *cancel, const char *ok, int style, bool first) {
    int ow = imax(ui_button_w(UI_NOICON, ok), W_CTL_MIN), cw = imax(ui_button_w(UI_NOICON, cancel), W_CTL_MIN);
    int by = py + ph - SP_6 - H_CTL, ox = px + pw - SP_6 - ow, cx = ox - SP_3 - cw, r = 0;
    bool danger = (style & 0xFF) == UI_DANGER_BTN;
    if (first) ui->focus = danger ? rid(cx, by, cw, H_CTL) : rid(ox, by, ow, H_CTL);
    if (ui_button(ui, cx, by, cw, H_CTL, cancel, UI_SECONDARY)) r = -1;
    if (ui_button(ui, ox, by, ow, H_CTL, ok, danger ? UI_DANGER_BTN : UI_ACCENT_BTN)) r = 1;
    if (take_key(ui, KEY_ESC)) r = -1;
    if (take_key(ui, KEY_ENTER)) r = danger ? -1 : 1;              /* nothing focused took it */
    ui->in_ov = false;
    return r;
}

int ui_dialog(struct ui *ui, const char *title, const char *body, const char *cancel, const char *ok, int style) {
    int W = ui->win->w, pw = imin(W - 2 * SP_5, W_ALERT), tw = pw - 2 * SP_6, px, py;
    int bh = body ? ui_text_wrap(ui, T_BODY, 0, 0, tw, body, 0) : 0;
    int ph = SP_6 + T_HEADING->height + (body ? SP_3 + bh : 0) + SP_6 + H_CTL + SP_6;
    bool first = dialog_frame(ui, pw, ph, &px, &py);
    gfx_text(ui->c, T_HEADING, px + SP_6, py + SP_6, title, TH.text);
    if (body) ui_text_wrap(ui, T_BODY, px + SP_6, py + SP_6 + T_HEADING->height + SP_3, tw, body, TH.text2);
    return dialog_buttons(ui, px, py, pw, ph, cancel ? cancel : "Cancel", ok, style, first);
}

int ui_prompt(struct ui *ui, const char *title, const char *okl, char *buf, int cap) {
    int W = ui->win->w, pw = imin(W - 2 * SP_5, W_FORM), ph = SP_6 + T_HEADING->height + SP_4 + H_CTL + SP_6 + H_CTL + SP_6, px, py;
    bool first = dialog_frame(ui, pw, ph, &px, &py);
    gfx_text(ui->c, T_HEADING, px + SP_6, py + SP_6, title, TH.text);
    int tx = px + SP_6, ty = py + SP_6 + T_HEADING->height + SP_4, tw = pw - 2 * SP_6;
    if (first || !ui->focus) ui->focus = rid(tx, ty, tw, H_CTL);
    bool enter = ui_textbox(ui, tx, ty, tw, H_CTL, buf, cap, "");
    ui->in_ov = true;
    int r = dialog_buttons(ui, px, py, pw, ph, "Cancel", okl, UI_PRIMARY, false);
    if (enter) r = 1;
    if (r) ui->focus = 0;
    return r;
}

int ui_confirm(struct ui *ui, const char *title, const char *text, const char *okl) {
    return ui_dialog(ui, title, text, "Cancel", okl, UI_PRIMARY);
}

/* --- ui-apps-editors --- */
int ui_dialog_ex(struct ui *ui, const char *title, const char *body, const char *alt, const char *cancel, const char *ok, int style) {
    if (cancel && !alt) return ui_dialog(ui, title, body, cancel, ok, style);
    int W = ui->win->w, pw = imin(W - 2 * SP_5, W_ALERT), tw = pw - 2 * SP_6, px, py;
    int bh = body ? ui_text_wrap(ui, T_BODY, 0, 0, tw, body, 0) : 0;
    int ph = SP_6 + T_HEADING->height + (body ? SP_3 + bh : 0) + SP_6 + H_CTL + SP_6;
    bool first = dialog_frame(ui, pw, ph, &px, &py);
    gfx_text(ui->c, T_HEADING, px + SP_6, py + SP_6, title, TH.text);
    if (body) ui_text_wrap(ui, T_BODY, px + SP_6, py + SP_6 + T_HEADING->height + SP_3, tw, body, TH.text2);
    int ow = imax(ui_button_w(UI_NOICON, ok), W_CTL_MIN), by = py + ph - SP_6 - H_CTL, ox = px + pw - SP_6 - ow, r = 0;
    if (first) ui->focus = rid(ox, by, ow, H_CTL);
    if (alt) {
        int cw = imax(ui_button_w(UI_NOICON, cancel), W_CTL_MIN), aw = imax(ui_button_w(UI_NOICON, alt), W_CTL_MIN);
        if (ui_button(ui, px + SP_6, by, aw, H_CTL, alt, (style & 0xFF) == UI_DANGER_BTN ? UI_DANGER_BTN : UI_SECONDARY)) r = 2;
        if (ui_button(ui, ox - SP_3 - cw, by, cw, H_CTL, cancel, UI_SECONDARY)) r = -1;
    }
    if (ui_button(ui, ox, by, ow, H_CTL, ok, UI_ACCENT_BTN)) r = 1;
    if (take_key(ui, KEY_ESC)) r = cancel ? -1 : 1;
    if (take_key(ui, KEY_ENTER)) r = 1;
    ui->in_ov = false;
    return r;
}
