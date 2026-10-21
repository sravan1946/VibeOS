/* wm.c - compositor + window manager.
 *
 * One kernel thread owns the screen. Input IRQs push into a ring and wake it;
 * it handles input, lets kernel windows (terminals) render, then recomposites
 * the damaged region (one bounding box) into a back buffer and copies that
 * region to the framebuffer. User windows draw into their own buffer (mapped
 * into the process) and call win_flip. */
#include "wm.h"
#include "console.h"
#include "mouse.h"
#include "tty.h"

uint32_t rtc_now(void);
void wallpaper_render(struct canvas *c);

static struct canvas bb, wall;          /* back buffer, wallpaper */
static int SW, SH;
static struct window *wins, *focused;
static int next_id = 1;
static bool running;
static char wm_chan;
static struct rect dirty;               /* w == 0: clean */
static int tz_minutes;

/* ---- input ring (filled from IRQs) ---- */
struct input { int kind; struct gui_event ev; int x, y, b, dz; };
static struct input inq[256];
static volatile uint32_t inq_h, inq_t;

/* ---- interaction state ---- */
static int cur_x, cur_y, cur_b;
static struct window *drag_win, *resize_win, *capture_win, *hover_win;
static int drag_dx, drag_dy;
static bool menu_open;
static int menu_hover = -1, hover_btn = -1, hover_task = -1;
static uint64_t last_click_ms; static struct window *last_click_win;
static uint32_t cpu_pct, last_idle, last_sample;
static int last_minute = -1;

/* ---- launcher entries: built-in terminal + /etc/apps ---- */
struct app { char name[32]; char path[64]; char arg[64]; color_t color; char glyph; };
static struct app apps[24];
static int napps;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ================= damage ================= */

void wm_damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = imin(SW, x + w), y1 = imin(SH, y + h);
    x = imax(0, x); y = imax(0, y);
    if (x >= x1 || y >= y1) return;
    uint32_t f = irq_save();
    if (!dirty.w) dirty = (struct rect){ x, y, x1 - x, y1 - y };
    else {
        int dx0 = imin(dirty.x, x), dy0 = imin(dirty.y, y);
        int dx1 = imax(dirty.x + dirty.w, x1), dy1 = imax(dirty.y + dirty.h, y1);
        dirty = (struct rect){ dx0, dy0, dx1 - dx0, dy1 - dy0 };
    }
    irq_restore(f);
    wakeup(&wm_chan);
}

#define SHADOW 22
static void frame_rect(struct window *w, struct rect *r) {
    if (w->flags & WIN_FLAG_FRAMELESS) { *r = (struct rect){ w->x, w->y, w->w, w->h }; return; }
    *r = (struct rect){ w->x - 1, w->y - WM_TITLE_H, w->w + 2, w->h + WM_TITLE_H + 1 };
}

static void damage_window(struct window *w) {
    struct rect r;
    frame_rect(w, &r);
    wm_damage(r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW + 8);
}

void wm_damage_client(struct window *w, int x, int y, int cw, int ch) {
    if (!w->visible || w->minimized) return;
    x = imax(0, x); y = imax(0, y);
    cw = imin(cw, w->w - x); ch = imin(ch, w->h - y);
    wm_damage(w->x + x, w->y + y, cw, ch);
}

/* ================= drawing ================= */

static uint8_t shadow_lut[SHADOW + 1];
static uint8_t corner_inset[WM_RADIUS];

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

static void draw_shadow(struct canvas *c, struct rect r, int strength) {
    int x0 = imax(c->cx0, r.x - SHADOW), x1 = imin(c->cx1, r.x + r.w + SHADOW);
    int y0 = imax(c->cy0, r.y - SHADOW + 6), y1 = imin(c->cy1, r.y + r.h + SHADOW + 6);
    for (int y = y0; y < y1; y++) {
        int sy = y - 6;                                     /* shadow is offset down */
        int dy = sy < r.y + WM_RADIUS ? r.y + WM_RADIUS - sy : sy >= r.y + r.h - WM_RADIUS ? sy - (r.y + r.h - WM_RADIUS - 1) : 0;
        uint32_t *row = &c->px[y * c->stride];
        for (int x = x0; x < x1; x++) {
            int dx = x < r.x + WM_RADIUS ? r.x + WM_RADIUS - x : x >= r.x + r.w - WM_RADIUS ? x - (r.x + r.w - WM_RADIUS - 1) : 0;
            if (!dx && !dy) { x = imax(x, r.x + r.w - WM_RADIUS - 1); continue; }   /* skip interior */
            int d = (dx && dy) ? (int)isqrt(dx * dx + dy * dy) : dx + dy;
            d -= WM_RADIUS;
            if (d < 0) d = 0;
            if (d > SHADOW) continue;
            uint32_t a = shadow_lut[d] * strength >> 8;
            if (a) row[x] = blend(row[x], a << 24);
        }
    }
}

static const color_t C_FRAME = RGB(0x24, 0x22, 0x36), C_FRAME_OFF = RGB(0x1B, 0x1A, 0x26);
static const color_t C_TEXT = RGB(0xED, 0xEB, 0xFF), C_TEXT_DIM = RGB(0x8C, 0x89, 0xA6);
static const color_t C_ACCENT = RGB(0xB3, 0x88, 0xFF), C_PINK = RGB(0xFF, 0x7A, 0xC6);
static const color_t C_BORDER = RGBA(255, 255, 255, 34);
static const color_t btn_col[3] = { RGB(0xFF, 0x5F, 0x7A), RGB(0xFF, 0xC2, 0x4B), RGB(0x4B, 0xE0, 0x8A) };

static void btn_center(struct window *w, int i, int *cx, int *cy) {
    *cx = w->x + 14 + i * 22;
    *cy = w->y - WM_TITLE_H / 2;
}

static void blit_client(struct canvas *c, struct window *w) {
    for (int row = 0; row < w->h; row++) {
        int y = w->y + row;
        if (y < c->cy0 || y >= c->cy1) continue;
        int inset = 0, from_bottom = w->h - 1 - row;
        if (!(w->flags & WIN_FLAG_FRAMELESS) && from_bottom < WM_RADIUS) inset = corner_inset[from_bottom];
        int x0 = imax(c->cx0, w->x + inset), x1 = imin(c->cx1, w->x + w->w - inset);
        if (x0 >= x1) continue;
        memcpy(&c->px[y * c->stride + x0], &w->buf[row * SW + (x0 - w->x)], (x1 - x0) * 4);
    }
}

static void draw_window(struct canvas *c, struct window *w) {
    struct rect r;
    frame_rect(w, &r);
    bool foc = w == focused;
    if (r.x - SHADOW >= c->cx1 || r.y - SHADOW >= c->cy1 || r.x + r.w + SHADOW <= c->cx0 || r.y + r.h + SHADOW + 6 <= c->cy0)
        return;
    if (w->flags & WIN_FLAG_FRAMELESS) {
        draw_shadow(c, r, foc ? 170 : 110);
        gfx_blit(c, w->x, w->y, w->buf, w->w, w->h, SW);
        return;
    }
    draw_shadow(c, r, foc ? 200 : 120);
    gfx_round_fill(c, r.x, r.y, r.w, r.h, WM_RADIUS, foc ? C_FRAME : C_FRAME_OFF);
    if (foc)   /* faint accent glow along the top of the focused window */
        gfx_fill(c, r.x + WM_RADIUS, r.y, r.w - 2 * WM_RADIUS, 1, RGBA(0xB3, 0x88, 0xFF, 110));
    blit_client(c, w);
    gfx_round_rect(c, r.x, r.y, r.w, r.h, WM_RADIUS, foc ? RGBA(0xB3, 0x88, 0xFF, 70) : C_BORDER);
    for (int i = 0; i < 3; i++) {
        int cx, cy;
        btn_center(w, i, &cx, &cy);
        if (!foc && hover_win != w) { gfx_circle_fill(c, cx, cy, 6, RGB(0x45, 0x43, 0x58)); continue; }
        gfx_circle_fill(c, cx, cy, 6, btn_col[i]);
        if (hover_win == w && hover_btn >= 0) {          /* glyphs on hover, like macOS */
            color_t g = RGBA(0, 0, 0, 150);
            if (i == 0) { gfx_line(c, cx - 2, cy - 2, cx + 2, cy + 2, g); gfx_line(c, cx - 2, cy + 2, cx + 2, cy - 2, g); }
            if (i == 1) gfx_fill(c, cx - 3, cy, 7, 1, g);
            if (i == 2) { gfx_fill(c, cx - 3, cy, 7, 1, g); gfx_fill(c, cx, cy - 3, 1, 7, g); }
        }
    }
    int tw = gfx_text_width(&font_ui_bold, w->title);
    int maxw = w->w - 90 * 2;
    int tx = w->x + (w->w - imin(tw, maxw)) / 2;
    if (maxw > 20) {
        struct canvas cc = *c;
        canvas_clip(&cc, w->x + 80, r.y, w->w - 160, WM_TITLE_H);
        gfx_text(&cc, &font_ui_bold, tx, r.y + (WM_TITLE_H - font_ui_bold.height) / 2 + 1, w->title, foc ? C_TEXT : C_TEXT_DIM);
    }
    if (!(w->flags & WIN_FLAG_NORESIZE)) {             /* resize grip */
        color_t g = RGBA(255, 255, 255, 40);
        for (int i = 0; i < 3; i++) gfx_line(c, w->x + w->w - 3 - i * 4, w->y + w->h - 2, w->x + w->w - 2, w->y + w->h - 3 - i * 4, g);
    }
}

/* ---- taskbar ---- */
#define TB_Y (SH - WM_TASKBAR_H)
#define LAUNCH_W 92
#define TASK_W 168

static int visible_count(void) {
    int n = 0;
    for (struct window *w = wins; w; w = w->next) if (w->visible && !(w->flags & WIN_FLAG_FRAMELESS)) n++;
    return n;
}

static struct window *task_at(int i) {
    for (struct window *w = wins; w; w = w->next)
        if (w->visible && !(w->flags & WIN_FLAG_FRAMELESS) && i-- == 0) return w;
    return 0;
}

static void app_icon(struct canvas *c, int x, int y, int s, color_t col, char glyph) {
    gfx_round_fill(c, x, y, s, s, s / 4, col);
    gfx_fill(c, x + 2, y + 1, s - 4, s / 2, RGBA(255, 255, 255, 30));
    char g[2] = { glyph, 0 };
    const struct font *f = &font_ui_bold;
    gfx_text(c, f, x + (s - gfx_text_width(f, g)) / 2, y + (s - f->height) / 2, g, RGB(255, 255, 255));
}

static void draw_taskbar(struct canvas *c) {
    if (TB_Y >= c->cy1) {} else {}
    gfx_fill(c, 0, TB_Y, SW, WM_TASKBAR_H, RGBA(13, 12, 22, 222));
    gfx_fill(c, 0, TB_Y, SW, 1, RGBA(255, 255, 255, 22));
    /* launcher pill: gradient columns clipped to a stadium shape */
    int lx = 8, ly = TB_Y + 7, lh = WM_TASKBAR_H - 14, rr = lh / 2;
    bool lit = menu_open || hover_task == -2;
    for (int i = 0; i < LAUNCH_W; i++) {
        int k = i < rr ? rr - i : i >= LAUNCH_W - rr ? i - (LAUNCH_W - rr - 1) : 0;
        int inset = k ? rr - (int)isqrt(imax(0, rr * rr - k * k)) : 0;
        color_t col = mix(C_ACCENT, C_PINK, i * 256 / LAUNCH_W);
        if (lit) col = mix(col, RGB(255, 255, 255), 50);
        gfx_fill(c, lx + i, ly + inset, 1, lh - 2 * inset, col);
    }
    int dcx = lx + 20, dcy = ly + lh / 2;                       /* diamond logo */
    for (int dy = -6; dy <= 6; dy++) {
        int half = 6 - (dy < 0 ? -dy : dy);
        gfx_fill(c, dcx - half, dcy + dy, 2 * half + 1, 1, RGB(255, 255, 255));
    }
    gfx_text(c, &font_ui_bold, lx + 33, ly + (lh - font_ui_bold.height) / 2, "VibeOS", RGB(255, 255, 255));
    /* window buttons */
    int n = visible_count(), x = lx + LAUNCH_W + 12;
    for (int i = 0; i < n; i++, x += TASK_W + 6) {
        struct window *w = task_at(i);
        if (x + TASK_W > SW - 200) break;
        bool act = w == focused && !w->minimized;
        color_t bg = act ? RGBA(0xB3, 0x88, 0xFF, 60) : hover_task == i ? RGBA(255, 255, 255, 26) : RGBA(255, 255, 255, 12);
        gfx_round_fill(c, x, ly, TASK_W, lh, 8, bg);
        if (act) gfx_round_fill(c, x + TASK_W / 2 - 12, TB_Y + WM_TASKBAR_H - 5, 24, 3, 1, C_ACCENT);
        struct canvas tc = *c;
        canvas_clip(&tc, x + 10, ly, TASK_W - 20, lh);
        gfx_text(&tc, &font_ui, x + 12, ly + (lh - font_ui.height) / 2, w->title, w->minimized ? C_TEXT_DIM : C_TEXT);
    }
    /* clock + cpu */
    uint32_t t = rtc_now() + tz_minutes * 60;
    char clk[16];
    snprintf(clk, sizeof clk, "%02u:%02u", (t / 3600) % 24, (t / 60) % 60);
    int cw = gfx_text_width(&font_ui_bold, clk);
    gfx_text(c, &font_ui_bold, SW - cw - 16, TB_Y + (WM_TASKBAR_H - font_ui_bold.height) / 2, clk, C_TEXT);
    char cpu[24];
    snprintf(cpu, sizeof cpu, "CPU %u%%", cpu_pct);
    int px = SW - cw - 40 - 110;
    gfx_text(c, &font_ui, px, TB_Y + (WM_TASKBAR_H - font_ui.height) / 2, cpu, C_TEXT_DIM);
    int bx = px + 62, by = TB_Y + WM_TASKBAR_H / 2 - 3;
    gfx_round_fill(c, bx, by, 44, 6, 3, RGBA(255, 255, 255, 30));
    gfx_round_fill(c, bx, by, imax(6, 44 * (int)cpu_pct / 100), 6, 3, cpu_pct > 80 ? btn_col[0] : C_ACCENT);
}

/* ---- launcher menu ---- */
#define MENU_W 300
#define ITEM_H 42
static int menu_items(void) { return napps + 2; }    /* apps + restart + shut down */
static struct rect menu_rect(void) {
    int h = 56 + menu_items() * ITEM_H + 18;
    return (struct rect){ 8, TB_Y - h - 8, MENU_W, h };
}
static int menu_item_y(int i) {
    struct rect m = menu_rect();
    return m.y + 52 + i * ITEM_H + (i >= napps ? 10 : 0);
}

static void draw_menu(struct canvas *c) {
    struct rect m = menu_rect();
    draw_shadow(c, m, 220);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, 14, RGBA(22, 20, 34, 246));
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 14, RGBA(0xB3, 0x88, 0xFF, 70));
    gfx_text(c, &font_title, m.x + 18, m.y + 12, "VibeOS", C_TEXT);
    gfx_text(c, &font_ui, m.x + 18 + gfx_text_width(&font_title, "VibeOS") + 10, m.y + 18, "apps", C_TEXT_DIM);
    for (int i = 0; i < menu_items(); i++) {
        int y = menu_item_y(i);
        if (i == napps) gfx_fill(c, m.x + 14, y - 6, m.w - 28, 1, RGBA(255, 255, 255, 24));
        if (i == menu_hover) gfx_round_fill(c, m.x + 8, y, m.w - 16, ITEM_H - 4, 9, RGBA(255, 255, 255, 22));
        const char *name; color_t col; char g;
        if (i < napps) { name = apps[i].name; col = apps[i].color; g = apps[i].glyph; }
        else if (i == napps) { name = "Restart"; col = RGB(0x5A, 0x64, 0x8C); g = 'R'; }
        else { name = "Shut down"; col = RGB(0xC2, 0x3B, 0x5A); g = 'X'; }
        app_icon(c, m.x + 18, y + 6, 26, col, g);
        gfx_text(c, &font_ui, m.x + 56, y + (ITEM_H - 4 - font_ui.height) / 2, name, C_TEXT);
    }
}

/* ---- cursor ---- */
static const char *cursor_img[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X",
    "      X..X", "       XX",
};
static void draw_cursor(struct canvas *c) {
    for (int y = 0; y < (int)ARRAY_LEN(cursor_img); y++)
        for (int x = 0; cursor_img[y][x]; x++) {
            char p = cursor_img[y][x];
            if (p == 'X') gfx_pixel(c, cur_x + x, cur_y + y, RGB(20, 18, 30));
            else if (p == '.') gfx_pixel(c, cur_x + x, cur_y + y, RGB(255, 255, 255));
        }
}
#define CURSOR_W 12
#define CURSOR_H 20

/* ================= compose ================= */

static void compose(void) {
    uint32_t f = irq_save();
    struct rect d = dirty;
    dirty.w = 0;
    irq_restore(f);
    if (!d.w) return;
    struct canvas c = bb;
    canvas_clip(&c, d.x, d.y, d.w, d.h);
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(&bb.px[y * SW + d.x], &wall.px[y * SW + d.x], d.w * 4);
    for (struct window *w = wins; w; w = w->next)
        if (w->visible && !w->minimized) draw_window(&c, w);
    if (d.y + d.h > TB_Y) draw_taskbar(&c);
    if (menu_open) draw_menu(&c);
    draw_cursor(&c);
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(&fb.px[y * fb.stride + d.x], &bb.px[y * SW + d.x], d.w * 4);
}

/* ================= window management ================= */

static void unlink_win(struct window *w) {
    for (struct window **pp = &wins; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; return; }
}

static void raise(struct window *w) {
    unlink_win(w);
    w->next = 0;
    struct window **pp = &wins;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
}

static void send(struct window *w, const struct gui_event *ev) {
    if (w->on_event) w->on_event(w, ev);
    else wm_post(w, ev);
}

void wm_focus(struct window *w) {
    if (focused == w) { if (w) raise(w), damage_window(w); return; }
    struct window *old = focused;
    focused = w;
    if (old) { struct gui_event e = { .type = EV_FOCUS, .pressed = 0 }; send(old, &e); damage_window(old); }
    if (w) {
        w->minimized = false;
        raise(w);
        struct gui_event e = { .type = EV_FOCUS, .pressed = 1 };
        send(w, &e);
        damage_window(w);
    }
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

struct window *wm_focused(void) { return focused; }

static void focus_top(void) {
    struct window *top = 0;
    for (struct window *w = wins; w; w = w->next)
        if (w->visible && !w->minimized && !(w->flags & WIN_FLAG_FRAMELESS)) top = w;
    focused = 0;
    if (top) wm_focus(top);
    else wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

struct window *wm_create(const char *title, int x, int y, int w, int h, int flags) {
    w = imax(80, imin(w, SW - 20));
    h = imax(40, imin(h, TB_Y - WM_TITLE_H - 10));
    struct window *win = kzalloc(sizeof *win);
    if (!win) return 0;
    /* ponytail: every window gets a full-screen-sized buffer (stride = SW) so resizing
     * never reallocates. ~4 MiB per window; fine for a dozen windows. */
    win->buf_frames = (uint32_t)SW * SH * 4 / PAGE_SIZE + 1;
    uint32_t pa = pmm_alloc_contig(win->buf_frames);
    if (!pa) { kfree(win); return 0; }
    win->buf = (uint32_t *)pa;
    memset32(win->buf, RGB(0x11, 0x13, 0x1C), (size_t)SW * h);
    win->id = next_id++;
    win->w = w; win->h = h; win->flags = flags;
    if (x < 0 || y < 0) {                               /* cascade */
        static int slot;
        x = 60 + (slot % 8) * 36 + (SW - 1280) / 4;
        y = 50 + WM_TITLE_H + (slot % 8) * 30;
        slot++;
        if (x + w > SW - 10) x = imax(10, SW - 10 - w);
        if (y + h > TB_Y - 10) y = imax(WM_TITLE_H + 10, TB_Y - 10 - h);
    }
    win->x = x; win->y = y;
    strlcpy(win->title, title, sizeof win->title);
    canvas_init(&win->canvas, win->buf, w, h, SW);
    win->visible = true;
    uint32_t f = irq_save();
    raise(win);
    irq_restore(f);
    if (!(flags & WIN_FLAG_FRAMELESS)) wm_focus(win);
    damage_window(win);
    return win;
}

void wm_destroy(struct window *w) {
    damage_window(w);
    unlink_win(w);
    if (w->on_destroy) w->on_destroy(w);
    if (drag_win == w) drag_win = 0;
    if (resize_win == w) resize_win = 0;
    if (capture_win == w) capture_win = 0;
    if (hover_win == w) hover_win = 0;
    if (last_click_win == w) last_click_win = 0;
    if (w->user_va) {
        struct task *o = task_find(w->owner_pid);
        if (o && o->dir == w->owner_dir) vmm_unmap_range(w->owner_dir, w->user_va, w->buf_frames * PAGE_SIZE, false);
    }
    pmm_free_contig((uint32_t)w->buf, w->buf_frames);
    w->buf = 0;
    if (focused == w) focus_top();
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

void wm_set_title(struct window *w, const char *t) {
    strlcpy(w->title, t, sizeof w->title);
    damage_window(w);
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static void set_geometry(struct window *w, int x, int y, int cw, int ch) {
    damage_window(w);
    bool resized = cw != w->w || ch != w->h;
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    w->canvas.w = cw; w->canvas.h = ch;
    canvas_noclip(&w->canvas);
    damage_window(w);
    if (resized) { struct gui_event e = { .type = EV_RESIZE, .w = cw, .h = ch }; send(w, &e); }
}

static void toggle_max(struct window *w) {
    if (w->flags & WIN_FLAG_NORESIZE) return;
    if (w->maximized) {
        w->maximized = false;
        set_geometry(w, w->restore.x, w->restore.y, w->restore.w, w->restore.h);
    } else {
        w->restore = (struct rect){ w->x, w->y, w->w, w->h };
        w->maximized = true;
        set_geometry(w, 1, WM_TITLE_H, SW - 2, TB_Y - WM_TITLE_H - 1);
    }
}

static void close_window(struct window *w) {
    struct gui_event e = { .type = EV_CLOSE };
    send(w, &e);
}

/* ================= apps ================= */

static void load_apps(void) {
    napps = 0;
    apps[napps++] = (struct app){ "Terminal", "", "", RGB(0x2D, 0x2A, 0x45), '>' };
    void *data; uint32_t size;
    if (vfs_read_all("/etc/apps", &data, &size) < 0) return;
    char *s = data;
    while (*s && napps < (int)ARRAY_LEN(apps)) {
        char *line = s;
        while (*s && *s != '\n') s++;
        if (*s) *s++ = 0;
        if (*line == '#' || !*line) continue;
        /* name|path|arg|RRGGBB|glyph */
        char *fld[5] = { 0 };
        int n = 0;
        for (char *p = line; n < 5; ) {
            fld[n++] = p;
            char *bar = strchr(p, '|');
            if (!bar) break;
            *bar = 0; p = bar + 1;
        }
        if (n < 2) continue;
        struct vnode *v;
        if (vfs_lookup("/", fld[1], &v) < 0) continue;     /* not installed */
        vnode_put(v);
        struct app *a = &apps[napps++];
        strlcpy(a->name, fld[0], sizeof a->name);
        strlcpy(a->path, fld[1], sizeof a->path);
        strlcpy(a->arg, n > 2 ? fld[2] : "", sizeof a->arg);
        uint32_t col = 0x6C5CE7;
        if (n > 3) { col = 0; for (char *p = fld[3]; *p; p++) col = col * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); }
        a->color = 0xFF000000u | col;
        a->glyph = n > 4 && fld[4][0] ? fld[4][0] : fld[0][0];
    }
    kfree(data);
}

int wm_spawn_app(const char *path, const char *arg) {
    char *argv[3] = { (char *)path, arg && *arg ? (char *)arg : 0, 0 };
    char *envp[] = { "PATH=/bin", "HOME=/home", 0 };
    struct file *null = 0;
    vfs_open("/", "/dev/null", O_RDWR, &null);
    struct file *fds[3] = { null, null, null };
    int pid = proc_spawn_ex(path, argv, envp, "/home", fds, 0);
    if (null) vfs_close(null);
    if (pid < 0) kprintf("wm: can't start %s: %d\n", path, pid);
    return pid;
}

static void launch(int i) {
    menu_open = false;
    struct rect m = menu_rect();
    wm_damage(m.x - SHADOW, m.y - SHADOW, m.w + 2 * SHADOW, m.h + 2 * SHADOW + 8);
    if (i < 0) return;
    if (i == napps || i == napps + 1) {
        extern int ext2_sync_all(void);
        ext2_sync_all();
        cli();
        if (i == napps) { while (inb(0x64) & 2) {} outb(0x64, 0xFE); }
        outw(0x604, 0x2000); outw(0xB004, 0x2000); outb(0xF4, 0);
        for (;;) hlt();
    }
    if (!apps[i].path[0]) termwin_open(0);
    else wm_spawn_app(apps[i].path, apps[i].arg);
}

/* ================= input ================= */

static struct window *window_at(int x, int y, bool *in_frame) {
    struct window *hit = 0;
    for (struct window *w = wins; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        struct rect r;
        frame_rect(w, &r);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { hit = w; *in_frame = !(x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h); }
    }
    return hit;
}

static int title_button_at(struct window *w, int x, int y) {
    if (w->flags & WIN_FLAG_FRAMELESS) return -1;
    for (int i = 0; i < 3; i++) {
        int cx, cy;
        btn_center(w, i, &cx, &cy);
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 64) return i;
    }
    return -1;
}

static int taskbar_at(int x, int y) {
    if (y < TB_Y) return -3;
    if (x >= 8 && x < 8 + LAUNCH_W) return -2;
    int x0 = 8 + LAUNCH_W + 12;
    if (x < x0) return -1;
    int i = (x - x0) / (TASK_W + 6);
    if ((x - x0) % (TASK_W + 6) >= TASK_W || i >= visible_count()) return -1;
    return i;
}

static void set_hover(struct window *hw, int btn, int task, int mitem) {
    if (hw != hover_win || btn != hover_btn) {
        if (hover_win) damage_window(hover_win);
        if (hw) damage_window(hw);
    }
    if (task != hover_task) wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
    if (mitem != menu_hover) { struct rect m = menu_rect(); wm_damage(m.x, m.y, m.w, m.h); }
    hover_win = hw; hover_btn = btn; hover_task = task; menu_hover = mitem;
}

static int menu_at(int x, int y) {
    if (!menu_open) return -1;
    struct rect m = menu_rect();
    if (x < m.x || x >= m.x + m.w) return -1;
    for (int i = 0; i < menu_items(); i++) {
        int iy = menu_item_y(i);
        if (y >= iy && y < iy + ITEM_H - 4) return i;
    }
    return -1;
}

static void mouse_event(int x, int y, int b, int dz) {
    int ox = cur_x, oy = cur_y, ob = cur_b;
    cur_x = x; cur_y = y; cur_b = b;
    if (ox != x || oy != y) { wm_damage(ox, oy, CURSOR_W, CURSOR_H); wm_damage(x, y, CURSOR_W, CURSOR_H); }
    bool down = (b & 1) && !(ob & 1), up = !(b & 1) && (ob & 1);

    if (drag_win) {
        if (up) drag_win = 0;
        else if (ox != x || oy != y) {
            int ny = imax(WM_TITLE_H, imin(y - drag_dy, TB_Y - 20));
            set_geometry(drag_win, x - drag_dx, ny, drag_win->w, drag_win->h);
        }
        return;
    }
    if (resize_win) {
        if (up) {
            set_geometry(resize_win, resize_win->x, resize_win->y, resize_win->w, resize_win->h);
            resize_win = 0;
        } else if (ox != x || oy != y) {
            struct window *w = resize_win;
            int nw = imax(160, imin(x - w->x + drag_dx, SW - w->x - 1));
            int nh = imax(80, imin(y - w->y + drag_dy, TB_Y - w->y - 1));
            set_geometry(w, w->x, w->y, nw, nh);
        }
        return;
    }
    if (capture_win) {                                       /* button held inside a client */
        struct window *w = capture_win;
        struct gui_event e = { .type = up ? EV_MOUSE_UP : EV_MOUSE_MOVE, .x = x - w->x, .y = y - w->y, .button = up ? 1 : b };
        send(w, &e);
        if (up) capture_win = 0;
        return;
    }

    bool in_frame = false;
    struct window *w = window_at(x, y, &in_frame);
    int mitem = menu_at(x, y);
    int task = taskbar_at(x, y);
    struct rect m = menu_rect();
    bool in_menu = menu_open && x >= m.x && x < m.x + m.w && y >= m.y && y < m.y + m.h;
    if (in_menu || task != -3) w = 0;
    int btn = w && in_frame ? title_button_at(w, x, y) : -1;
    set_hover(w, btn, task == -3 ? -1 : task, mitem);

    if (w && !in_frame && !w->on_event) {                   /* motion inside a user client */
        static struct window *last_in;
        if (last_in && last_in != w) { struct gui_event e = { .type = EV_MOUSE_LEAVE }; wm_post(last_in, &e); }
        last_in = w;
        if (ox != x || oy != y) { struct gui_event e = { .type = EV_MOUSE_MOVE, .x = x - w->x, .y = y - w->y, .button = b }; send(w, &e); }
    }
    if (dz && w && !in_frame) { struct gui_event e = { .type = EV_SCROLL, .x = x - w->x, .y = y - w->y, .dz = dz }; send(w, &e); }

    if ((b & 2) && !(ob & 2) && w && !in_frame) { struct gui_event e = { .type = EV_MOUSE_DOWN, .x = x - w->x, .y = y - w->y, .button = 2 }; send(w, &e); }
    if (!down) return;

    if (in_menu) { if (mitem >= 0) launch(mitem); return; }
    if (menu_open && task != -2) launch(-1);                 /* click elsewhere closes the menu */
    if (task == -2) { menu_open = !menu_open; if (menu_open) load_apps(); wm_damage(0, 0, SW, SH); return; }
    if (task >= 0) {
        struct window *tw = task_at(task);
        if (tw == focused && !tw->minimized) { tw->minimized = true; damage_window(tw); focus_top(); }
        else { tw->minimized = false; wm_focus(tw); }
        return;
    }
    if (task == -1) return;
    if (!w) { if (focused) { struct window *o = focused; focused = 0; damage_window(o); wm_damage(0, TB_Y, SW, WM_TASKBAR_H); } return; }

    if (!(w->flags & WIN_FLAG_FRAMELESS)) wm_focus(w);
    if (in_frame) {
        if (btn == 0) { close_window(w); return; }
        if (btn == 1) { w->minimized = true; damage_window(w); focus_top(); return; }
        if (btn == 2) { toggle_max(w); return; }
        if (y < w->y) {
            if (last_click_win == w && ticks_ms - last_click_ms < 400) { toggle_max(w); last_click_win = 0; return; }
            last_click_win = w; last_click_ms = ticks_ms;
            if (!w->maximized) { drag_win = w; drag_dx = x - w->x; drag_dy = y - w->y; }
        }
        return;
    }
    if (!(w->flags & WIN_FLAG_NORESIZE) && x >= w->x + w->w - 14 && y >= w->y + w->h - 14) {
        resize_win = w; drag_dx = w->x + w->w - x; drag_dy = w->y + w->h - y;
        w->maximized = false;
        return;
    }
    capture_win = w;
    struct gui_event e = { .type = EV_MOUSE_DOWN, .x = x - w->x, .y = y - w->y, .button = 1 };
    send(w, &e);
}

static void key_event(const struct gui_event *ev) {
    int mods = ev->mods;
    if (ev->pressed) {
        if (ev->key == KEY_SUPER && !(mods & ~MOD_SUPER)) { menu_open = !menu_open; if (menu_open) load_apps(); wm_damage(0, 0, SW, SH); return; }
        if ((mods & MOD_CTRL) && (mods & MOD_ALT) && (ev->key == 't' || ev->key == 'T')) { termwin_open(0); return; }
        if ((mods & MOD_ALT) && ev->key == KEY_TAB) {         /* cycle: bottom-most visible to top */
            for (struct window *w = wins; w; w = w->next)
                if (w->visible && !(w->flags & WIN_FLAG_FRAMELESS) && w != focused) { w->minimized = false; wm_focus(w); break; }
            return;
        }
        if ((mods & MOD_ALT) && ev->key == KEY_F4) { if (focused) close_window(focused); return; }
        if (ev->key == KEY_ESC && menu_open) { launch(-1); return; }
        if (ev->key == KEY_F11 && focused) { toggle_max(focused); return; }
    }
    if (focused) send(focused, ev);
}

/* IRQ-context producers */
static void irq_mouse(int x, int y, int b, int dz) {
    uint32_t n = inq_h - inq_t;
    if (n >= ARRAY_LEN(inq)) return;
    struct input *in = &inq[inq_h % ARRAY_LEN(inq)];
    in->kind = 1; in->x = x; in->y = y; in->b = b; in->dz = dz;
    inq_h++;
    wakeup(&wm_chan);
}

static void irq_key(const struct gui_event *ev) {
    if (inq_h - inq_t >= ARRAY_LEN(inq)) return;
    struct input *in = &inq[inq_h % ARRAY_LEN(inq)];
    in->kind = 2; in->ev = *ev;
    inq_h++;
    wakeup(&wm_chan);
}

/* ================= user windows (fd + syscalls) ================= */

void wm_post(struct window *w, const struct gui_event *ev) {
    if (w->qh - w->qt >= ARRAY_LEN(w->q)) {                 /* coalesce: drop oldest motion */
        if (ev->type == EV_MOUSE_MOVE) return;
        w->qt++;
    }
    w->q[w->qh++ % ARRAY_LEN(w->q)] = *ev;
    wakeup(w);
}

static int win_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) {
    struct window *w = v->priv;
    uint32_t f = irq_save();
    while (w->qh == w->qt) {
        if (sleep_on(w, 0) < 0) { irq_restore(f); return -EINTR; }
    }
    uint32_t got = 0;
    while (got + sizeof(struct gui_event) <= n && w->qh != w->qt) {
        memcpy((char *)buf + got, &w->q[w->qt++ % ARRAY_LEN(w->q)], sizeof(struct gui_event));
        got += sizeof(struct gui_event);
    }
    irq_restore(f);
    return got;
}

static int win_poll(struct vnode *v, int ev) {
    struct window *w = v->priv;
    return ((w->qh != w->qt ? POLLIN : 0) | POLLOUT) & ev;
}

static void win_release(struct vnode *v) {
    struct window *w = v->priv;
    if (w->buf) wm_destroy(w);
    kfree(w);
}

static const struct vnode_ops win_ops = { .read = win_read, .poll = win_poll, .release = win_release };

static struct window *win_from_fd(int fd) {
    struct file *f = fd_get(fd);
    return f && f->vn->ops == &win_ops ? f->vn->priv : 0;
}

static int map_into_owner(struct window *w) {
    uint32_t len = w->buf_frames * PAGE_SIZE;
    for (uint32_t va = 0xA0000000u; va + len <= 0xB0000000u; va += 0x00800000u) {
        bool free = true;
        for (uint32_t a = va; a < va + len; a += 0x100000) if (vmm_translate(current->dir, a)) { free = false; break; }
        if (!free) continue;
        for (uint32_t i = 0; i < w->buf_frames; i++)
            if (vmm_map(current->dir, va + i * PAGE_SIZE, (uint32_t)w->buf + i * PAGE_SIZE, PTE_U | PTE_W | PTE_NOFREE) < 0)
                return -ENOMEM;
        w->user_va = va;
        w->owner_dir = current->dir;
        w->owner_pid = current->pid;
        return 0;
    }
    return -ENOMEM;
}

int win_syscall(int num, uint32_t a, uint32_t b, uint32_t c) {
    if (!running) return -ENODEV;
    if (num == SYS_win_create) {
        struct win_info *wi = (struct win_info *)a;
        if (!vmm_user_ok(current->dir, a, sizeof *wi, true)) return -EFAULT;
        wi->title[sizeof wi->title - 1] = 0;
        struct window *w = wm_create(wi->title, wi->x, wi->y, wi->w, wi->h, wi->flags & 3);
        if (!w) return -ENOMEM;
        if (map_into_owner(w) < 0) { wm_destroy(w); kfree(w); return -ENOMEM; }
        w->vn.mode = S_IFCHR | 0600;
        w->vn.ops = &win_ops;
        w->vn.priv = w;
        struct file *f = vfs_open_vnode(&w->vn, O_RDWR);
        int fd = f ? fd_alloc(f) : -ENOMEM;
        if (fd < 0) { if (f) vfs_close(f); return fd; }
        wi->x = w->x; wi->y = w->y; wi->w = w->w; wi->h = w->h;
        wi->pixels = (uint32_t *)w->user_va;
        wi->stride = SW;
        return fd;
    }
    struct window *w = win_from_fd(a);
    if (!w) return -EBADF;
    if (num == SYS_win_flip) {
        if (!b) { wm_damage_client(w, 0, 0, w->w, w->h); return 0; }
        if (!vmm_user_ok(current->dir, b, sizeof(struct win_rect), false)) return -EFAULT;
        struct win_rect *r = (struct win_rect *)b;
        wm_damage_client(w, r->x, r->y, r->w, r->h);
        return 0;
    }
    switch (b) {                                             /* SYS_win_set */
    case WIN_SET_TITLE: {
        char t[64];
        for (int i = 0; i < 63; i++) {
            if (!vmm_user_ok(current->dir, c + i, 1, false)) return -EFAULT;
            if (!(t[i] = ((char *)c)[i])) break;
            t[i + 1] = 0;
        }
        t[63] = 0;
        wm_set_title(w, t);
        return 0;
    }
    case WIN_SET_SIZE: case WIN_SET_POS: {
        if (!vmm_user_ok(current->dir, c, sizeof(struct win_rect), false)) return -EFAULT;
        struct win_rect *r = (struct win_rect *)c;
        if (b == WIN_SET_POS) set_geometry(w, r->x, r->y, w->w, w->h);
        else set_geometry(w, w->x, w->y, imax(80, imin(r->w, SW - 4)), imax(40, imin(r->h, TB_Y - WM_TITLE_H - 4)));
        return 0;
    }
    case WIN_SET_VISIBLE:
        w->visible = c != 0;
        damage_window(w);
        if (!w->visible && focused == w) focus_top();
        return 0;
    }
    return -EINVAL;
}

/* ================= main loop ================= */

bool wm_running(void) { return running; }
void wm_wake(void) { wakeup(&wm_chan); }

static int wm_thread(void *arg) {
    uint64_t last_tick = 0;
    for (;;) {
        uint32_t f = irq_save();
        while (inq_h == inq_t && !dirty.w && ticks_ms - last_tick < 250)
            sleep_on(&wm_chan, 16);
        irq_restore(f);
        /* coalesce mouse motion: only the latest position matters unless buttons change */
        while (inq_h != inq_t) {
            struct input in = inq[inq_t % ARRAY_LEN(inq)];
            inq_t++;
            if (in.kind == 1) {
                struct input *nx = &inq[inq_t % ARRAY_LEN(inq)];
                if (inq_h != inq_t && nx->kind == 1 && nx->b == in.b && !in.dz) continue;
                mouse_event(in.x, in.y, in.b, in.dz);
            } else key_event(&in.ev);
        }
        for (struct window *w = wins, *n; w; w = n) {
            n = w->next;
            if (w->on_frame) w->on_frame(w);
        }
        if (ticks_ms - last_tick >= 250) {
            last_tick = ticks_ms;
            uint32_t now = ticks_ms, idle = idle_ms();
            if (now - last_sample >= 1000) {
                uint32_t span = now - last_sample, id = idle - last_idle;
                cpu_pct = span ? 100 - MIN(100, id * 100 / span) : 0;
                last_sample = now; last_idle = idle;
                wm_damage(SW - 260, TB_Y, 260, WM_TASKBAR_H);
            }
            uint32_t t = rtc_now() + tz_minutes * 60;
            if ((int)((t / 60) % 60) != last_minute) { last_minute = (t / 60) % 60; wm_damage(SW - 120, TB_Y, 120, WM_TASKBAR_H); }
        }
        compose();
    }
    return 0;
}

/* /etc/autostart: "term ARGV..." opens a terminal running ARGV (kept open after
 * exit), "run ARGV..." starts a GUI app, "shell" opens an interactive terminal.
 * No file: one shell. */
static void autostart(void) {
    void *data; uint32_t size;
    if (vfs_read_all("/etc/autostart", &data, &size) < 0) { termwin_open(0); return; }
    for (char *s = data; *s; ) {
        char *line = s;
        while (*s && *s != '\n') s++;
        if (*s) *s++ = 0;
        char *argv[16]; int n = 0;
        for (char *p = line; *p && n < 15; ) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            argv[n++] = p;
            while (*p && *p != ' ') p++;
        }
        argv[n] = 0;
        if (!n || argv[0][0] == '#') continue;
        if (!strcmp(argv[0], "shell")) termwin_open(0);
        else if (!strcmp(argv[0], "term") && n > 1) termwin_open_argv(argv + 1, true);
        else if (!strcmp(argv[0], "run") && n > 1) wm_spawn_app(argv[1], n > 2 ? argv[2] : 0);
    }
    kfree(data);
}

void wm_init(void) {
    SW = fb.w; SH = fb.h;
    uint32_t *b = kmalloc((size_t)SW * SH * 4), *wp = kmalloc((size_t)SW * SH * 4);
    if (!b || !wp) { kprintf("wm: no memory for buffers\n"); return; }
    canvas_init(&bb, b, SW, SH, SW);
    canvas_init(&wall, wp, SW, SH, SW);
    wallpaper_render(&wall);
    for (int d = 0; d <= SHADOW; d++) { int t = SHADOW - d; shadow_lut[d] = t * t * 110 / (SHADOW * SHADOW); }
    for (int i = 0; i < WM_RADIUS; i++) {
        int dy2 = (2 * (WM_RADIUS - i) - 1) * (2 * (WM_RADIUS - i) - 1);
        corner_inset[i] = WM_RADIUS - (isqrt(imax(0, 4 * WM_RADIUS * WM_RADIUS - dy2)) + 1) / 2;
    }
    void *tz; uint32_t tzs;
    if (vfs_read_all("/etc/tz", &tz, &tzs) == 0) {
        char *p = tz; int sign = 1, v = 0;
        if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        tz_minutes = sign * v;
        kfree(tz);
    }
    mouse_pos(&cur_x, &cur_y);
    load_apps();
    running = true;
    bootcon_enable(false);
    mouse_sink = irq_mouse;
    key_sink = irq_key;
    last_sample = ticks_ms; last_idle = idle_ms();
    wm_damage(0, 0, SW, SH);
    kthread_create("compositor", wm_thread, 0);
    autostart();
    kprintf("[desktop] ready\n");
}
