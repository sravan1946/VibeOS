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
#include "net.h"
#include "vfs.h"

uint32_t rtc_now(void);
#include "wallpaper.h"

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
static int drag_dx, drag_dy, drag_sx, drag_sy;
static int snap_zone;                                /* 1 left half, 2 right half, 3 maximise */
static void draw_snap(struct canvas *c);
static void load_prefs(bool init);
static bool menu_open;
static int menu_hover = -1, hover_btn = -1, hover_task = -1;
static uint64_t last_click_ms; static struct window *last_click_win;
static uint32_t cpu_pct, last_idle, last_sample;
static int last_minute = -1;

/* ---- workspaces, overlays ---- */
static int cur_ws;
static bool super_alone;                                /* Super tapped on its own opens the launcher */
static bool sw_open;                                    /* Alt+Tab switcher */
static void sw_damage(void);
static void sw_close(bool commit);
static struct window *wmenu_win;
static void wmenu_close(void);
static int vol_cache = -1;                              /* master volume, -1 without a mixer */
static bool net_up, net_present;
static uint32_t net_ip;
static int unseen;                                      /* notifications not yet looked at */
static inline bool onws(const struct window *w) { return w->ws < 0 || w->ws == cur_ws; }
static inline bool shown(const struct window *w) { return w->visible && !w->minimized && onws(w); }

/* ---- launcher entries: built-in terminal + /etc/apps ---- */
struct app { char name[32]; char path[64]; char arg[64]; color_t color; char glyph; };
static struct app apps[24];
static int napps;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* screen-sized buffers (back buffer, wallpaper, window buffers, snapshots) come straight from the
 * frame allocator: at 1920x1080 four of them would fill the kernel heap */
#define SCREEN_FRAMES(w, h) ((uint32_t)(w) * (h) * 4 / PAGE_SIZE + 1)
static uint32_t *big_alloc(int w, int h) { return (uint32_t *)pmm_alloc_contig(SCREEN_FRAMES(w, h)); }
static void big_free(uint32_t *p, int w, int h) { if (p) pmm_free_contig((uint32_t)p, SCREEN_FRAMES(w, h)); }
static uint32_t *under, *ws_old;         /* lazily made: open-animation backdrop, workspace-switch snapshot */

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
    if (!shown(w)) return;
    if (sw_open) sw_damage();                         /* live thumbnails */
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
        memcpy(&c->px[y * c->stride + x0], &w->buf[row * w->canvas.stride + (x0 - w->x)], (x1 - x0) * 4);
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
        gfx_blit(c, w->x, w->y, w->buf, w->w, w->h, w->canvas.stride);
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
    if (w->topmost) gfx_circle_fill(c, w->x + w->w - 18, w->y - WM_TITLE_H / 2, 3, C_ACCENT);
    if (!(w->flags & WIN_FLAG_NORESIZE)) {             /* resize grip */
        color_t g = RGBA(255, 255, 255, 40);
        for (int i = 0; i < 3; i++) gfx_line(c, w->x + w->w - 3 - i * 4, w->y + w->h - 2, w->x + w->w - 2, w->y + w->h - 3 - i * 4, g);
    }
}

/* ---- taskbar ---- */
#define TB_Y (SH - WM_TASKBAR_H)
#define LAUNCH_W 92
#define TASK_W 168
#define WS_X (8 + LAUNCH_W + 10)
#define WS_CELL 26
#define TRAY_W 30
#define TRAY_VOL_X (SW - 304)
#define TRAY_NET_X (vol_cache >= 0 ? SW - 268 : SW - 304)
#define TRAY_X (SW - 320)
#define CLOCK_HIT 84
enum { T_NONE = -3, T_LAUNCH = -2, T_GAP = -1, T_WS0 = -10, T_VOL = -20, T_NET = -21, T_CLOCK = -22 };

static int visible_count(void) {
    int n = 0;
    for (struct window *w = wins; w; w = w->next) if (w->visible && onws(w) && !(w->flags & WIN_FLAG_FRAMELESS)) n++;
    return n;
}

static struct window *task_at(int i) {
    for (struct window *w = wins; w; w = w->next)
        if (w->visible && onws(w) && !(w->flags & WIN_FLAG_FRAMELESS) && i-- == 0) return w;
    return 0;
}

static void app_icon(struct canvas *c, int x, int y, int s, color_t col, char glyph) {
    gfx_round_fill(c, x, y, s, s, s / 4, col);
    gfx_fill(c, x + 2, y + 1, s - 4, s / 2, RGBA(255, 255, 255, 30));
    char g[2] = { glyph, 0 };
    const struct font *f = &font_ui_bold;
    gfx_text(c, f, x + (s - gfx_text_width(f, g)) / 2, y + (s - f->height) / 2, g, RGB(255, 255, 255));
}

static bool cn_open_p(void);
static bool qs_open_p(void);

/* speaker glyph with 0..2 waves, or a cross when muted */
static void draw_speaker(struct canvas *c, int x, int cy, int vol, color_t col) {
    gfx_fill(c, x, cy - 3, 4, 7, col);
    for (int i = 0; i < 5; i++) gfx_fill(c, x + 4 + i, cy - 3 - i, 1, 7 + 2 * i, col);
    if (!vol) { color_t b = btn_col[0]; gfx_line(c, x + 12, cy - 3, x + 18, cy + 3, b); gfx_line(c, x + 12, cy + 3, x + 18, cy - 3, b); return; }
    gfx_line(c, x + 12, cy - 3, x + 12, cy + 3, col);
    if (vol > 40) gfx_line(c, x + 15, cy - 5, x + 15, cy + 5, col);
}

/* three rising bars, lit when there's a lease */
static void draw_net(struct canvas *c, int x, int cy, bool up) {
    for (int i = 0; i < 3; i++) gfx_fill(c, x + i * 5, cy + 6 - 4 * (i + 1), 3, 4 * (i + 1), up ? C_TEXT : RGBA(255, 255, 255, 60));
    if (!up) gfx_line(c, x - 1, cy + 7, x + 14, cy - 7, btn_col[0]);
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
    /* workspace indicator: numbers in a row, the current one lit, a dot under the others that hold windows */
    for (int k = 0; k < WM_WS; k++) {
        int wx = WS_X + k * WS_CELL;
        bool cur = k == cur_ws, has = false;
        for (struct window *w = wins; w; w = w->next) if (w->visible && w->ws == k) has = true;
        if (cur) gfx_round_fill(c, wx, ly, WS_CELL - 2, lh, 8, RGBA(0xB3, 0x88, 0xFF, 96));
        else if (hover_task == T_WS0 - k) gfx_round_fill(c, wx, ly, WS_CELL - 2, lh, 8, RGBA(255, 255, 255, 22));
        char d[2] = { '1' + k, 0 };
        gfx_text_center(c, &font_ui_bold, wx, ly + (lh - font_ui_bold.height) / 2 - (has && !cur ? 2 : 0), WS_CELL - 2, d, cur ? C_TEXT : C_TEXT_DIM);
        if (has && !cur) gfx_circle_fill(c, wx + WS_CELL / 2 - 1, ly + lh - 5, 2, C_ACCENT);
    }
    /* window buttons */
    int n = visible_count(), x = WS_X + WM_WS * WS_CELL + 12;
    for (int i = 0; i < n; i++, x += TASK_W + 6) {
        struct window *w = task_at(i);
        if (x + TASK_W > TRAY_X) break;
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
    if (hover_task == T_CLOCK || cn_open_p()) gfx_round_fill(c, SW - cw - 26, ly, cw + 22, lh, 8, RGBA(255, 255, 255, 20));
    gfx_text(c, &font_ui_bold, SW - cw - 16, TB_Y + (WM_TASKBAR_H - font_ui_bold.height) / 2, clk, C_TEXT);
    if (unseen) gfx_circle_fill(c, SW - cw - 21, TB_Y + 10, 3, C_PINK);
    char cpu[24];
    snprintf(cpu, sizeof cpu, "CPU %u%%", cpu_pct);
    int px = SW - cw - 40 - 110;
    gfx_text(c, &font_ui, px, TB_Y + (WM_TASKBAR_H - font_ui.height) / 2, cpu, C_TEXT_DIM);
    int bx = px + 62, by = TB_Y + WM_TASKBAR_H / 2 - 3;
    gfx_round_fill(c, bx, by, 44, 6, 3, RGBA(255, 255, 255, 30));
    gfx_round_fill(c, bx, by, imax(6, 44 * (int)cpu_pct / 100), 6, 3, cpu_pct > 80 ? btn_col[0] : C_ACCENT);
    /* tray: volume (if there's a mixer) and network */
    int cy = TB_Y + WM_TASKBAR_H / 2;
    if (vol_cache >= 0) {
        int vx = TRAY_VOL_X;
        if (hover_task == T_VOL || qs_open_p()) gfx_round_fill(c, vx, ly, TRAY_W, lh, 8, RGBA(255, 255, 255, 20));
        draw_speaker(c, vx + 6, cy, vol_cache, C_TEXT);
    }
    if (net_present) {
        int nx = TRAY_NET_X;
        if (hover_task == T_NET || qs_open_p()) gfx_round_fill(c, nx, ly, TRAY_W, lh, 8, RGBA(255, 255, 255, 20));
        draw_net(c, nx + 8, cy, net_up);
    }
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

/* ---- desktop context menu (right-click on the wallpaper) ---- */
static const struct { const char *label, *path; } ctx_items[] = {
    { "New Terminal", 0 }, { "Files", "/bin/files" }, { "System Monitor", "/bin/sysmon" },
    { "Change Wallpaper", "" }, { "Settings", "/bin/settings" }, { "About VibeOS", "/bin/about" },
};
#define CTX_W 230
#define CTX_ITEM 34
static bool ctx_open;
static int ctx_x, ctx_y, ctx_hover = -1, wall_theme;

static struct rect ctx_rect(void) { return (struct rect){ ctx_x, ctx_y, CTX_W, 12 + (int)ARRAY_LEN(ctx_items) * CTX_ITEM }; }
static void ctx_damage(void) { struct rect r = ctx_rect(); wm_damage(r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW + 8); }
static int ctx_at(int x, int y) {
    struct rect r = ctx_rect();
    if (!ctx_open || x < r.x || x >= r.x + r.w || y < r.y + 6 || y >= r.y + r.h - 6) return -1;
    return (y - r.y - 6) / CTX_ITEM;
}

static void draw_ctx(struct canvas *c) {
    struct rect m = ctx_rect();
    draw_shadow(c, m, 200);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, 12, RGBA(22, 20, 34, 246));
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 12, RGBA(0xB3, 0x88, 0xFF, 60));
    for (int i = 0; i < (int)ARRAY_LEN(ctx_items); i++) {
        int y = m.y + 6 + i * CTX_ITEM;
        if (i == ctx_hover) gfx_round_fill(c, m.x + 6, y, m.w - 12, CTX_ITEM, 8, RGBA(0xB3, 0x88, 0xFF, 60));
        gfx_text(c, &font_ui, m.x + 18, y + (CTX_ITEM - font_ui.height) / 2, ctx_items[i].label, C_TEXT);
        if (ctx_items[i].path && !ctx_items[i].path[0]) {
            const char *n = wallpaper_name(wall_theme);
            gfx_text(c, &font_ui, m.x + m.w - 18 - gfx_text_width(&font_ui, n), y + (CTX_ITEM - font_ui.height) / 2, n, C_TEXT_DIM);
        }
    }
}

static void set_wallpaper(int theme) {
    wall_theme = theme % wallpaper_count();
    wallpaper_render(&wall, wall_theme);
    wallpaper_watermark(&wall);
    struct file *f;
    if (vfs_open("/", "/etc/wallpaper", O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) {
        vfs_write(f, wallpaper_name(wall_theme), strlen(wallpaper_name(wall_theme)));
        vfs_close(f);
    }
    wm_damage(0, 0, SW, SH);
}

static void ctx_activate(int i) {
    ctx_open = false;
    ctx_damage();
    if (i < 0) return;
    if (!ctx_items[i].path) termwin_open(0);
    else if (!ctx_items[i].path[0]) set_wallpaper(wall_theme + 1);
    else wm_spawn_app(ctx_items[i].path, 0);
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

/* ================= overlays: toasts, notification center, quick settings, window menu, Alt+Tab ================= */

/* greedy word wrap of s into at most max lines of width maxw; fills off/len, returns the line count */
static int wrap(const struct font *f, const char *s, int maxw, int *off, int *len, int max) {
    int n = 0, i = 0, L = strlen(s);
    while (i < L && n < max) {
        int last = -1, j = i;
        for (; j < L; j++) {
            if (s[j] == ' ') last = j;
            if (gfx_text_width_n(f, s + i, j + 1 - i) > maxw) { if (last > i) j = last; else if (j == i) j++; break; }
        }
        off[n] = i; len[n] = j - i; n++;
        i = j;
        while (i < L && s[i] == ' ') i++;
    }
    return n;
}

static const color_t C_BODY = RGB(0xC4, 0xC1, 0xDC);

/* ---- notifications: the history (newest first) feeds both the toasts and the center ---- */
#define NQ 24
#define NTOAST 4
#define TOAST_W 340
#define TOAST_MS 4000
#define TOAST_IN 200
struct notif { char title[48], body[160]; color_t col; uint32_t born, wall; bool toast; };
static struct notif hist[NQ];
static int nhist;

static int toast_layout(struct rect *r, struct notif **ns) {
    int k = 0, y = 16;
    for (int i = 0; i < nhist && k < NTOAST; i++) {
        if (!hist[i].toast) continue;
        int off[3], len[3], nl = wrap(&font_ui, hist[i].body, TOAST_W - 40, off, len, 3);
        int h = 12 + font_ui_bold.height + (nl ? 4 + nl * (font_ui.height + 2) : 0) + 12;
        r[k] = (struct rect){ SW - TOAST_W - 16, y, TOAST_W, h };
        ns[k++] = &hist[i];
        y += h + 10;
    }
    return k;
}

static void toast_damage(void) {
    struct rect r[NTOAST]; struct notif *n[NTOAST];
    int k = toast_layout(r, n);
    if (k) wm_damage(SW - TOAST_W - 16 - SHADOW, 0, TOAST_W + 16 + SHADOW, r[k - 1].y + r[k - 1].h + SHADOW + 14);
}

static bool toasts_animating(void) {
    for (int i = 0; i < nhist; i++) if (hist[i].toast && ticks_ms - hist[i].born < TOAST_IN + 24) return true;
    return false;
}

static void draw_toasts(struct canvas *c) {
    struct rect r[NTOAST]; struct notif *n[NTOAST];
    int k = toast_layout(r, n);
    for (int i = 0; i < k; i++) {
        uint32_t age = ticks_ms - n[i]->born;
        int off = 0;
        if (age < TOAST_IN) { int t = age * 256 / TOAST_IN, e = 256 - (256 - t) * (256 - t) / 256; off = (TOAST_W + 24) * (256 - e) / 256; }
        struct rect q = { r[i].x + off, r[i].y, r[i].w, r[i].h };
        draw_shadow(c, q, 200);
        gfx_round_fill(c, q.x, q.y, q.w, q.h, 12, RGBA(24, 22, 38, 248));
        gfx_round_rect(c, q.x, q.y, q.w, q.h, 12, RGBA(255, 255, 255, 40));
        gfx_round_fill(c, q.x + 10, q.y + 12, 4, q.h - 24, 2, n[i]->col);
        struct canvas tc = *c;
        canvas_clip(&tc, q.x + 22, q.y, q.w - 34, q.h);
        gfx_text(&tc, &font_ui_bold, q.x + 22, q.y + 12, n[i]->title, C_TEXT);
        int off2[3], len2[3], nl = wrap(&font_ui, n[i]->body, q.w - 40, off2, len2, 3);
        for (int l = 0; l < nl; l++)
            gfx_text_n(&tc, &font_ui, q.x + 22, q.y + 12 + font_ui_bold.height + 4 + l * (font_ui.height + 2), n[i]->body + off2[l], len2[l], C_BODY);
    }
}

/* ---- notification center ---- */
#define CN_W 380
#define CN_ROW 76
static bool cn_open;
static int cn_off;
static bool cn_open_p(void) { return cn_open; }
static int cn_vis(void) { return imax(1, imin(imax(1, nhist), (TB_Y - 24 - 70) / CN_ROW)); }
static struct rect cn_rect(void) { return (struct rect){ SW - CN_W - 12, 12, CN_W, 64 + cn_vis() * CN_ROW }; }
static void cn_damage(void) { struct rect r = cn_rect(); wm_damage(r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW + 8); }
static void cn_close(void) { if (!cn_open) return; cn_damage(); cn_open = false; wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }

static void draw_center(struct canvas *c) {
    struct rect m = cn_rect();
    draw_shadow(c, m, 220);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, 14, RGBA(22, 20, 34, 248));
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 14, RGBA(0xB3, 0x88, 0xFF, 70));
    gfx_text(c, &font_title, m.x + 18, m.y + 14, "Notifications", C_TEXT);
    if (nhist) gfx_text(c, &font_ui, m.x + m.w - 18 - gfx_text_width(&font_ui, "Clear all"), m.y + 20, "Clear all", C_ACCENT);
    gfx_fill(c, m.x + 14, m.y + 52, m.w - 28, 1, RGBA(255, 255, 255, 24));
    if (!nhist) { gfx_text_center(c, &font_ui, m.x, m.y + 52 + CN_ROW / 2 - 4, m.w, "You're all caught up", C_TEXT_DIM); return; }
    for (int i = 0; i < cn_vis() && cn_off + i < nhist; i++) {
        struct notif *n = &hist[cn_off + i];
        int y = m.y + 58 + i * CN_ROW;
        gfx_round_fill(c, m.x + 10, y, m.w - 20, CN_ROW - 6, 10, RGBA(255, 255, 255, 12));
        gfx_round_fill(c, m.x + 20, y + 12, 4, CN_ROW - 30, 2, n->col);
        uint32_t t = n->wall + tz_minutes * 60;
        char tm[8];
        snprintf(tm, sizeof tm, "%02u:%02u", (t / 3600) % 24, (t / 60) % 60);
        gfx_text(c, &font_ui, m.x + m.w - 24 - gfx_text_width(&font_ui, tm), y + 10, tm, C_TEXT_DIM);
        struct canvas tc = *c;
        canvas_clip(&tc, m.x + 34, y, m.w - 34 - 70, CN_ROW - 6);
        gfx_text(&tc, &font_ui_bold, m.x + 34, y + 10, n->title, C_TEXT);
        struct canvas bc = *c;
        canvas_clip(&bc, m.x + 34, y, m.w - 34 - 26, CN_ROW - 6);
        int off[2], len[2], nl = wrap(&font_ui, n->body, m.w - 34 - 34, off, len, 2);
        for (int l = 0; l < nl; l++)
            gfx_text_n(&bc, &font_ui, m.x + 34, y + 10 + font_ui_bold.height + 3 + l * (font_ui.height + 1), n->body + off[l], len[l], C_BODY);
    }
    int vis = cn_vis();
    if (nhist > vis) {                                         /* scroll thumb */
        int th = imax(16, vis * CN_ROW * vis / nhist), ty = m.y + 58 + (vis * CN_ROW - th) * cn_off / (nhist - vis);
        gfx_round_fill(c, m.x + m.w - 7, ty, 3, th, 1, RGBA(255, 255, 255, 60));
    }
}

/* ---- quick settings (volume + network), opened from the tray ---- */
#define QS_W 290
static bool qs_open, qs_drag;
static bool qs_open_p(void) { return qs_open; }
static struct rect qs_rect(void) {
    int h = 46 + (vol_cache >= 0 ? 50 : 0) + 62;
    return (struct rect){ SW - QS_W - 12, TB_Y - h - 8, QS_W, h };
}
static void qs_damage(void) { struct rect r = qs_rect(); wm_damage(r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW + 8); }
static void qs_close(void) { if (!qs_open) return; qs_damage(); qs_open = false; qs_drag = false; wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }

static void draw_qs(struct canvas *c) {
    struct rect m = qs_rect();
    draw_shadow(c, m, 220);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, 14, RGBA(22, 20, 34, 248));
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 14, RGBA(0xB3, 0x88, 0xFF, 70));
    gfx_text(c, &font_ui_bold, m.x + 18, m.y + 14, "Quick settings", C_TEXT);
    int y = m.y + 46;
    if (vol_cache >= 0) {
        draw_speaker(c, m.x + 20, y + 20, vol_cache, C_TEXT);
        int x0 = m.x + 64, x1 = m.x + m.w - 58, cy = y + 20, kx = x0 + (x1 - x0) * vol_cache / 100;
        gfx_round_fill(c, x0, cy - 3, x1 - x0, 6, 3, RGBA(255, 255, 255, 34));
        gfx_round_fill(c, x0, cy - 3, imax(6, kx - x0), 6, 3, C_ACCENT);
        gfx_circle_fill(c, kx, cy, 8, RGB(255, 255, 255));
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", vol_cache);
        gfx_text(c, &font_ui, m.x + m.w - 18 - gfx_text_width(&font_ui, pct), cy - font_ui.height / 2, pct, C_TEXT_DIM);
        y += 50;
    }
    gfx_fill(c, m.x + 14, y - 6, m.w - 28, 1, RGBA(255, 255, 255, 24));
    draw_net(c, m.x + 22, y + 24, net_up);
    gfx_text(c, &font_ui_bold, m.x + 64, y + 8, !net_present ? "No network card" : net_up ? "Connected" : "Not connected", C_TEXT);
    if (net_up) {
        char l[40];
        snprintf(l, sizeof l, "%u.%u.%u.%u", net_ip & 255, net_ip >> 8 & 255, net_ip >> 16 & 255, net_ip >> 24);
        gfx_text(c, &font_ui, m.x + 64, y + 30, l, C_BODY);
    }
}

/* ---- window menu (right-click on a title bar) ---- */
enum { WA_MIN = 1, WA_MAX, WA_TOP, WA_CLOSE, WA_WS = 10 };
struct wmi { char label[40]; int act; bool sep; };
static struct wmi wmi[10];
static int wmi_n, wmenu_x, wmenu_y, wmenu_hover = -1;
#define WMENU_W 224
#define WMENU_ITEM 32
static int wmenu_h(void) { int h = 12; for (int i = 0; i < wmi_n; i++) h += WMENU_ITEM + (wmi[i].sep ? 8 : 0); return h; }
static int wmenu_iy(int i) { int y = wmenu_y + 6; for (int k = 0; k <= i; k++) { if (wmi[k].sep) y += 8; if (k < i) y += WMENU_ITEM; } return y; }
static void wmenu_damage(void) { wm_damage(wmenu_x - SHADOW, wmenu_y - SHADOW, WMENU_W + 2 * SHADOW, wmenu_h() + 2 * SHADOW + 8); }
static void wmenu_close(void) { if (!wmenu_win) return; wmenu_damage(); wmenu_win = 0; }
static int wmenu_at(int x, int y) {
    if (!wmenu_win || x < wmenu_x || x >= wmenu_x + WMENU_W) return -1;
    for (int i = 0; i < wmi_n; i++) if (y >= wmenu_iy(i) && y < wmenu_iy(i) + WMENU_ITEM) return i;
    return -1;
}

static void draw_wmenu(struct canvas *c) {
    struct rect m = { wmenu_x, wmenu_y, WMENU_W, wmenu_h() };
    draw_shadow(c, m, 200);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, 12, RGBA(22, 20, 34, 248));
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 12, RGBA(0xB3, 0x88, 0xFF, 60));
    for (int i = 0; i < wmi_n; i++) {
        int y = wmenu_iy(i);
        if (wmi[i].sep) gfx_fill(c, m.x + 14, y - 5, m.w - 28, 1, RGBA(255, 255, 255, 24));
        if (i == wmenu_hover) gfx_round_fill(c, m.x + 6, y, m.w - 12, WMENU_ITEM, 8, RGBA(0xB3, 0x88, 0xFF, 60));
        gfx_text(c, &font_ui, m.x + 18, y + (WMENU_ITEM - font_ui.height) / 2, wmi[i].label, wmi[i].act == WA_CLOSE ? btn_col[0] : C_TEXT);
        if (wmi[i].act == WA_TOP && wmenu_win && wmenu_win->topmost) gfx_circle_fill(c, m.x + m.w - 22, y + WMENU_ITEM / 2, 4, C_ACCENT);
    }
}

/* ---- Alt+Tab switcher: live thumbnails, most recently used first ---- */
#define SW_CW 196
#define SW_CH 150
#define SW_TW 176
#define SW_TH 108
#define SW_COLS 6
static struct window *sw_list[16];
static int sw_n, sw_sel;
static struct rect sw_rect(void) {
    int cols = imin(sw_n, SW_COLS), rows = (sw_n + SW_COLS - 1) / SW_COLS, w = cols * SW_CW + 24, h = rows * SW_CH + 24;
    return (struct rect){ (SW - w) / 2, (TB_Y - h) / 2, w, h };
}
static struct rect sw_cell(int i) {
    struct rect r = sw_rect();
    return (struct rect){ r.x + 12 + i % SW_COLS * SW_CW, r.y + 12 + i / SW_COLS * SW_CH, SW_CW, SW_CH };
}
static void sw_damage(void) { struct rect r = sw_rect(); wm_damage(r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW + 8); }

/* 2x2-tap downscale of a window buffer, fitted into tw x th */
static void draw_thumb(struct canvas *c, int x, int y, int tw, int th, struct window *w) {
    int dw = tw, dh = w->h * tw / w->w;
    if (dh > th) { dh = th; dw = w->w * th / w->h; }
    if (dw < 1 || dh < 1) return;
    x += (tw - dw) / 2; y += (th - dh) / 2;
    int x0 = imax(x, c->cx0), x1 = imin(x + dw, c->cx1), y0 = imax(y, c->cy0), y1 = imin(y + dh, c->cy1);
    for (int dy = y0; dy < y1; dy++) {
        int ws = w->canvas.stride;
        const uint32_t *ra = &w->buf[(((dy - y) * 4 + 1) * w->h / (4 * dh)) * ws], *rb = &w->buf[(((dy - y) * 4 + 3) * w->h / (4 * dh)) * ws];
        uint32_t *o = &c->px[dy * c->stride];
        for (int dx = x0; dx < x1; dx++) {
            int xa = ((dx - x) * 4 + 1) * w->w / (4 * dw), xb = ((dx - x) * 4 + 3) * w->w / (4 * dw);
            uint32_t p0 = ra[xa], p1 = ra[xb], p2 = rb[xa], p3 = rb[xb];
            uint32_t rbv = (((p0 & 0xFF00FF) + (p1 & 0xFF00FF) + (p2 & 0xFF00FF) + (p3 & 0xFF00FF)) >> 2) & 0xFF00FF;
            uint32_t g = (((p0 & 0xFF00) + (p1 & 0xFF00) + (p2 & 0xFF00) + (p3 & 0xFF00)) >> 2) & 0xFF00;
            o[dx] = 0xFF000000u | rbv | g;
        }
    }
}

static void draw_switcher(struct canvas *c) {
    struct rect m = sw_rect();
    draw_shadow(c, m, 230);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, 16, RGBA(18, 16, 30, 244));
    gfx_round_rect(c, m.x, m.y, m.w, m.h, 16, RGBA(0xB3, 0x88, 0xFF, 80));
    for (int i = 0; i < sw_n; i++) {
        struct rect q = sw_cell(i);
        struct window *w = sw_list[i];
        if (i == sw_sel) {
            gfx_round_fill(c, q.x + 2, q.y + 2, q.w - 4, q.h - 4, 12, RGBA(0xB3, 0x88, 0xFF, 56));
            gfx_round_rect(c, q.x + 2, q.y + 2, q.w - 4, q.h - 4, 12, RGBA(0xD8, 0xC4, 0xFF, 170));
        }
        gfx_round_fill(c, q.x + 10, q.y + 10, SW_TW, SW_TH, 6, RGB(0x11, 0x13, 0x1C));
        draw_thumb(c, q.x + 10, q.y + 10, SW_TW, SW_TH, w);
        struct canvas tc = *c;
        canvas_clip(&tc, q.x + 8, q.y, q.w - 16, q.h);
        int tw = gfx_text_width(&font_ui_bold, w->title), tx = tw < q.w - 16 ? q.x + (q.w - tw) / 2 : q.x + 8;
        gfx_text(&tc, &font_ui_bold, tx, q.y + 10 + SW_TH + 10, w->title, i == sw_sel ? C_TEXT : C_TEXT_DIM);
    }
}

/* ================= compose ================= */

static bool no_present;
#define FADE_MS 180
#define RISE 12

/* open animation: draw w risen and blended over what's beneath it */
static void draw_window_fading(struct canvas *c, struct window *w, uint32_t age) {
    if (!under && !(under = big_alloc(SW, SH))) { draw_window(c, w); return; }
    int t = (int)(age * 256 / FADE_MS), e = 256 - (256 - t) * (256 - t) / 256;   /* ease-out */
    struct rect r;
    frame_rect(w, &r);
    int x0 = imax(c->cx0, r.x - SHADOW), y0 = imax(c->cy0, r.y - SHADOW);
    int x1 = imin(c->cx1, r.x + r.w + SHADOW), y1 = imin(c->cy1, r.y + r.h + SHADOW + RISE + 8);
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) memcpy(&under[y * SW + x0], &bb.px[y * SW + x0], (x1 - x0) * 4);
    int off = RISE * (256 - e) / 256;
    w->y += off;
    draw_window(c, w);
    w->y -= off;
    for (int y = y0; y < y1; y++) {
        uint32_t *o = &bb.px[y * SW], *u = &under[y * SW];
        for (int x = x0; x < x1; x++) o[x] = mix(u[x], o[x], e);
    }
}

/* returns true while something is animating */
static bool compose(void) {
    uint32_t f = irq_save();
    struct rect d = dirty;
    dirty.w = 0;
    irq_restore(f);
    if (!d.w) return false;
    bool anim = false;
    struct canvas c = bb;
    canvas_clip(&c, d.x, d.y, d.w, d.h);
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(&bb.px[y * SW + d.x], &wall.px[y * SW + d.x], d.w * 4);
    for (struct window *w = wins; w; w = w->next) {
        if (!shown(w)) continue;
        uint32_t age = ticks_ms - w->born;
        if (age < FADE_MS && !(w->flags & WIN_FLAG_FRAMELESS)) { draw_window_fading(&c, w, age); anim = true; }
        else draw_window(&c, w);
    }
    if (d.y + d.h > TB_Y) draw_taskbar(&c);
    if (drag_win && snap_zone) draw_snap(&c);
    if (menu_open) draw_menu(&c);
    if (ctx_open) draw_ctx(&c);
    if (wmenu_win) draw_wmenu(&c);
    if (qs_open) draw_qs(&c);
    if (cn_open) draw_center(&c);
    draw_toasts(&c);
    if (sw_open) draw_switcher(&c);
    draw_cursor(&c);
    if (!no_present)
        for (int y = d.y; y < d.y + d.h; y++)
            memcpy(&fb.px[y * fb.stride + d.x], &bb.px[y * SW + d.x], d.w * 4);
    if (toasts_animating()) { anim = true; toast_damage(); }
    if (anim)
        for (struct window *w = wins; w; w = w->next)
            if (ticks_ms - w->born < FADE_MS + 20) { struct rect r; frame_rect(w, &r); wm_damage(r.x - SHADOW, r.y - SHADOW, r.w + 2 * SHADOW, r.h + 2 * SHADOW + RISE + 8); }
    return anim;
}

/* blend the boot splash (snap) into the first desktop frame */
static void crossfade(const uint32_t *snap, int step, int ms) {
    for (int t = step; t < 256 + step; t += step) {
        if (t > 256) t = 256;
        uint64_t until = ticks_ms + ms;
        for (int y = 0; y < SH; y++) {
            const uint32_t *a = snap + y * SW, *b = bb.px + y * SW;
            uint32_t *o = fb.px + y * fb.stride;
            for (int x = 0; x < SW; x++) o[x] = mix(a[x], b[x], t);
        }
        while (ticks_ms < until) hlt();
    }
}

/* ================= window management ================= */

static void unlink_win(struct window *w) {
    for (struct window **pp = &wins; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; return; }
}

static void raise(struct window *w) {
    unlink_win(w);
    struct window **pp = &wins;
    while (*pp && (w->topmost || !(*pp)->topmost)) pp = &(*pp)->next;   /* topmost windows stay above the rest */
    w->next = *pp;
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
        if (shown(w) && !(w->flags & WIN_FLAG_FRAMELESS)) top = w;
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
     * never reallocates. ~4 MiB per window at 1280x800; fine for a dozen windows. A display
     * mode change only ever grows it (wm_set_mode). */
    win->buf_frames = (uint32_t)SW * SH * 4 / PAGE_SIZE + 1;
    uint32_t pa = pmm_alloc_contig(win->buf_frames);
    if (!pa) { kfree(win); return 0; }
    win->buf = (uint32_t *)pa;
    memset32(win->buf, RGB(0x11, 0x13, 0x1C), (size_t)SW * h);
    win->id = next_id++;
    win->ws = flags & WIN_FLAG_FRAMELESS ? -1 : cur_ws;
    win->born = ticks_ms;
    win->w = w; win->h = h; win->flags = flags;
    if (x < 0 || y < 0) {          /* least-overlap placement on a 24 px grid, cascade on ties */
        static int slot;
        int bx = 60 + (slot % 8) * 36, by = 50 + WM_TITLE_H + (slot % 8) * 30;
        int64_t best = -1;
        slot++;
        for (int cy = 16 + WM_TITLE_H; cy + h + 16 <= TB_Y; cy += 24)
            for (int cx = 16; cx + w + 16 <= SW; cx += 24) {
                int64_t ov = 0;
                for (struct window *o = wins; o; o = o->next) {
                    if (!shown(o) || (o->flags & WIN_FLAG_FRAMELESS)) continue;
                    int ix = imin(cx + w, o->x + o->w) - imax(cx, o->x);
                    int iy = imin(cy + h, o->y + o->h) - imax(cy - WM_TITLE_H, o->y - WM_TITLE_H);
                    if (ix > 0 && iy > 0) ov += (int64_t)ix * iy;
                }
                if (best < 0 || ov < best) { best = ov; x = cx; y = cy; }
            }
        if (best * 10 > (int64_t)w * h * 6) { x = bx; y = by; }   /* mostly covered anyway: cascade */
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
    if (wmenu_win == w) wmenu_close();
    if (sw_open) sw_close(false);
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

/* client rect of a snap zone */
static struct rect snap_rect(int z) {
    int h = TB_Y - WM_TITLE_H - 1, half = SW / 2;
    if (z == 3) return (struct rect){ 1, WM_TITLE_H, SW - 2, h };
    return (struct rect){ z == 1 ? 1 : half + 1, WM_TITLE_H, half - 2, h };
}

static void damage_snap(int z) {
    if (!z) return;
    struct rect r = snap_rect(z);
    wm_damage(r.x - 1, r.y - WM_TITLE_H - 1, r.w + 2, r.h + WM_TITLE_H + 2);
}

static void set_snap(int z) {
    if (z == snap_zone) return;
    damage_snap(snap_zone); damage_snap(z);
    snap_zone = z;
}

static void apply_snap(struct window *w, int z) {
    if (w->flags & WIN_FLAG_NORESIZE) return;
    if (!w->maximized) w->restore = (struct rect){ w->x, w->y, w->w, w->h };
    w->maximized = true;
    struct rect r = snap_rect(z);
    set_geometry(w, r.x, r.y, r.w, r.h);
}

static void draw_snap(struct canvas *c) {
    struct rect r = snap_rect(snap_zone);
    int x = r.x + 6, y = r.y - WM_TITLE_H + 6, w = r.w - 12, h = r.h + WM_TITLE_H - 12;
    gfx_round_fill(c, x, y, w, h, WM_RADIUS + 2, RGBA(0xB3, 0x88, 0xFF, 46));
    gfx_round_rect(c, x, y, w, h, WM_RADIUS + 2, RGBA(0xD8, 0xC4, 0xFF, 150));
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
        ata_flush_all();
        cli();
        if (i == napps) { while (inb(0x64) & 2) {} outb(0x64, 0xFE); }
        outw(0x604, 0x2000); outw(0xB004, 0x2000); outb(0xF4, 0);
        for (;;) hlt();
    }
    if (!apps[i].path[0]) termwin_open(0);
    else wm_spawn_app(apps[i].path, apps[i].arg);
}

static int taskbar_at(int x, int y);
/* ================= workspaces, popups, notifications ================= */

static void defocus(void) {
    if (!focused) return;
    struct window *o = focused;
    focused = 0;
    struct gui_event e = { .type = EV_FOCUS, .pressed = 0 };
    send(o, &e);
    damage_window(o);
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static void close_popups(void) {
    if (menu_open) launch(-1);
    if (ctx_open) ctx_activate(-1);
    wmenu_close(); qs_close(); cn_close(); sw_close(false);
}

/* fade to the new workspace: snapshot the screen, compose the target, cross-dissolve */
static void switch_ws(int n) {
    n = (n % WM_WS + WM_WS) % WM_WS;
    if (n == cur_ws) return;
    close_popups();
    drag_win = resize_win = capture_win = 0;
    set_snap(0);
    if (!ws_old) ws_old = big_alloc(SW, SH);
    compose();                                           /* flush pending damage so bb == screen */
    if (ws_old) memcpy(ws_old, bb.px, (size_t)SW * SH * 4);
    defocus();
    hover_win = 0;
    cur_ws = n;
    focus_top();
    wm_damage(0, 0, SW, SH);
    if (ws_old) { no_present = true; compose(); no_present = false; crossfade(ws_old, 43, 20); }
}

static void move_to_ws(struct window *w, int k) {
    if (w->ws == k || (w->flags & WIN_FLAG_FRAMELESS)) return;
    bool was = w == focused;
    damage_window(w);
    if (drag_win == w) drag_win = 0;
    if (was) defocus();
    w->ws = k;
    if (was) focus_top();
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

/* ---- notifications ---- */
void wm_notify(const char *msg, int n) {
    char buf[256];
    n = imin(n, (int)sizeof buf - 1);
    memcpy(buf, msg, n);
    buf[n] = 0;
    char *title = buf, *body = buf + n, *kind = buf + n, *p;
    if ((p = strchr(buf, '\n'))) {
        *p = 0; body = p + 1;
        if ((p = strchr(body, '\n'))) { *p = 0; kind = p + 1; } else kind = body + strlen(body);
    }
    for (char *e = kind + strlen(kind); e > kind && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
    for (char *e = body + strlen(body); e > body && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
    for (p = body; *p; p++) if (*p == '\r') *p = ' ';
    if (!*title && !*body) return;
    if (running) toast_damage();
    memmove(hist + 1, hist, (NQ - 1) * sizeof hist[0]);
    if (nhist < NQ) nhist++;
    struct notif *h = &hist[0];
    memset(h, 0, sizeof *h);
    strlcpy(h->title, *title ? title : "Notification", sizeof h->title);
    strlcpy(h->body, body, sizeof h->body);
    h->col = !strcmp(kind, "good") ? RGB(0x4B, 0xE0, 0x8A) : !strcmp(kind, "warn") ? RGB(0xFF, 0xC2, 0x4B) : !strcmp(kind, "bad") ? RGB(0xFF, 0x5F, 0x7A) : C_ACCENT;
    h->born = ticks_ms; h->wall = rtc_now();
    h->toast = !cn_open;
    int k = 0;
    for (int i = 0; i < nhist; i++) if (hist[i].toast && ++k > NTOAST) hist[i].toast = false;
    if (!cn_open) unseen++;
    if (running) { toast_damage(); if (cn_open) cn_damage(); wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }
}

static void cn_toggle(void) {
    if (cn_open) { cn_close(); return; }
    close_popups();
    toast_damage();
    for (int i = 0; i < nhist; i++) hist[i].toast = false;
    unseen = 0; cn_off = 0; cn_open = true;
    cn_damage();
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static void notif_remove(int i) {
    memmove(hist + i, hist + i + 1, (NQ - 1 - i) * sizeof hist[0]);
    nhist--;
    cn_off = imax(0, imin(cn_off, nhist - cn_vis()));
}

/* returns true when the click/scroll belonged to the center */
static bool cn_mouse(int x, int y, bool down, int dz) {
    struct rect m = cn_rect();
    if (x < m.x || x >= m.x + m.w || y < m.y || y >= m.y + m.h) {
        if (!down) return false;
        bool clk = taskbar_at(x, y) == T_CLOCK;           /* clicking the clock again just closes */
        cn_close();
        return clk;
    }
    if (dz && nhist > cn_vis()) { cn_off = imax(0, imin(nhist - cn_vis(), cn_off + (dz > 0 ? 1 : -1))); cn_damage(); }
    if (!down) return true;
    if (y < m.y + 52) { if (nhist && x > m.x + m.w - 110) { cn_damage(); nhist = 0; cn_off = 0; cn_damage(); } return true; }
    int i = (y - m.y - 58) / CN_ROW;
    if (y >= m.y + 58 && i < cn_vis() && cn_off + i < nhist) { cn_damage(); notif_remove(cn_off + i); cn_damage(); }
    return true;
}

static bool toasts_mouse(int x, int y, bool down) {
    if (!down) return false;
    struct rect r[NTOAST]; struct notif *n[NTOAST];
    int k = toast_layout(r, n);
    for (int i = 0; i < k; i++)
        if (x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y && y < r[i].y + r[i].h) { toast_damage(); n[i]->toast = false; return true; }
    return false;
}

/* ---- quick settings ---- */
static void set_volume(int v) {
    vol_cache = ac97_volume(imax(0, imin(100, v)));
    wm_damage(TRAY_X, TB_Y, SW - TRAY_X, WM_TASKBAR_H);
    if (qs_open) qs_damage();
}

static void qs_toggle(void) {
    if (qs_open) { qs_close(); return; }
    close_popups();
    qs_open = true;
    qs_damage();
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static bool qs_mouse(int x, int y, bool down, int dz) {
    struct rect m = qs_rect();
    if (x < m.x || x >= m.x + m.w || y < m.y || y >= m.y + m.h) {
        if (!down) return false;
        int t = taskbar_at(x, y);
        qs_close();
        return t == T_VOL || t == T_NET;
    }
    if (vol_cache < 0) return true;
    if (dz) set_volume(vol_cache - (dz > 0 ? 5 : -5));
    int vy = m.y + 46;
    if (down && y >= vy && y < vy + 40) {
        static int saved = 60;
        if (x < m.x + 56) { if (vol_cache) saved = vol_cache; set_volume(vol_cache ? 0 : saved); }
        else if (x < m.x + m.w - 50) { qs_drag = true; set_volume((x - (m.x + 64)) * 100 / (m.w - 64 - 58)); }
    }
    return true;
}

/* ---- window menu ---- */
static void wmi_add(const char *label, int act, bool sep) {
    if (wmi_n >= (int)ARRAY_LEN(wmi)) return;
    strlcpy(wmi[wmi_n].label, label, sizeof wmi[0].label);
    wmi[wmi_n].act = act; wmi[wmi_n].sep = sep;
    wmi_n++;
}

static void wmenu_open(struct window *w, int x, int y) {
    wmi_n = 0;
    wmi_add("Minimise", WA_MIN, false);
    if (!(w->flags & WIN_FLAG_NORESIZE)) wmi_add(w->maximized ? "Restore" : "Maximise", WA_MAX, false);
    wmi_add("Always on top", WA_TOP, false);
    for (int k = 0, first = 1; k < WM_WS; k++) {
        if (k == w->ws) continue;
        char l[32];
        snprintf(l, sizeof l, "Move to workspace %d", k + 1);
        wmi_add(l, WA_WS + k, first);
        first = 0;
    }
    wmi_add("Close", WA_CLOSE, true);
    wmenu_win = w;
    wmenu_x = imax(4, imin(x, SW - WMENU_W - 8));
    wmenu_y = imax(4, imin(y, TB_Y - wmenu_h() - 8));
    wmenu_hover = -1;
    wmenu_damage();
}

static void wmenu_act(int a) {
    struct window *w = wmenu_win;
    wmenu_close();
    if (!w) return;
    if (a == WA_MIN) { w->minimized = true; damage_window(w); focus_top(); }
    else if (a == WA_MAX) toggle_max(w);
    else if (a == WA_TOP) { w->topmost = !w->topmost; raise(w); damage_window(w); }
    else if (a == WA_CLOSE) close_window(w);
    else if (a >= WA_WS) move_to_ws(w, a - WA_WS);
}

static bool wmenu_mouse(int x, int y, bool click) {
    int i = wmenu_at(x, y);
    if (i != wmenu_hover) { wmenu_hover = i; wmenu_damage(); }
    if (x >= wmenu_x && x < wmenu_x + WMENU_W && y >= wmenu_y && y < wmenu_y + wmenu_h()) {
        if (click && i >= 0) wmenu_act(wmi[i].act);
        return true;
    }
    if (click) wmenu_close();
    return false;
}

/* ---- Alt+Tab ---- */
static void sw_start(bool back) {
    struct window *tmp[32];
    int n = 0;
    for (struct window *w = wins; w && n < 32; w = w->next)
        if (w->visible && onws(w) && !(w->flags & WIN_FLAG_FRAMELESS)) tmp[n++] = w;
    if (n == 1 && focused != tmp[0]) { tmp[0]->minimized = false; wm_focus(tmp[0]); }
    if (n < 2) return;
    close_popups();
    sw_n = imin(n, 16);
    for (int i = 0; i < sw_n; i++) sw_list[i] = tmp[n - 1 - i];     /* most recently used first */
    sw_sel = back ? sw_n - 1 : focused == sw_list[0] ? 1 : 0;
    sw_open = true;
    sw_damage();
}

static void sw_step(int d) { sw_sel = (sw_sel + d + sw_n) % sw_n; sw_damage(); }

static void sw_close(bool commit) {
    if (!sw_open) return;
    sw_damage();
    sw_open = false;
    struct window *w = sw_list[sw_sel];
    if (commit && w) { w->minimized = false; wm_focus(w); }
}

static void sw_mouse(int x, int y, bool down) {
    for (int i = 0; i < sw_n; i++) {
        struct rect q = sw_cell(i);
        if (x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h) {
            if (i != sw_sel) { sw_sel = i; sw_damage(); }
            if (down) sw_close(true);
            return;
        }
    }
    if (down) sw_close(false);
}

/* ================= input ================= */

static struct window *window_at(int x, int y, bool *in_frame) {
    struct window *hit = 0;
    for (struct window *w = wins; w; w = w->next) {
        if (!shown(w)) continue;
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
    if (y < TB_Y) return T_NONE;
    if (x >= 8 && x < 8 + LAUNCH_W) return T_LAUNCH;
    if (x >= WS_X && x < WS_X + WM_WS * WS_CELL) return T_WS0 - (x - WS_X) / WS_CELL;
    if (x >= SW - CLOCK_HIT) return T_CLOCK;
    if (vol_cache >= 0 && x >= TRAY_VOL_X && x < TRAY_VOL_X + TRAY_W) return T_VOL;
    if (net_present && x >= TRAY_NET_X && x < TRAY_NET_X + TRAY_W) return T_NET;
    int x0 = WS_X + WM_WS * WS_CELL + 12;
    if (x < x0) return T_GAP;
    int i = (x - x0) / (TASK_W + 6);
    if ((x - x0) % (TASK_W + 6) >= TASK_W || i >= visible_count()) return T_GAP;
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
        struct window *w = drag_win;
        if (up) {
            if (snap_zone) apply_snap(w, snap_zone);
            set_snap(0);
            drag_win = 0;
        } else if (ox != x || oy != y) {
            if (w->maximized) {                              /* pull a maximised/tiled window off the edge */
                if (imax(x - drag_sx, drag_sx - x) + imax(y - drag_sy, drag_sy - y) < 6) return;
                drag_dx = drag_dx * w->restore.w / imax(1, w->w);
                w->maximized = false;
                set_geometry(w, x - drag_dx, w->y, w->restore.w, w->restore.h);
            }
            int ny = imax(WM_TITLE_H, imin(y - drag_dy, TB_Y - 20));
            set_geometry(w, x - drag_dx, ny, w->w, w->h);
            set_snap(w->flags & WIN_FLAG_NORESIZE ? 0 : x <= 1 ? 1 : x >= SW - 2 ? 2 : y <= 1 ? 3 : 0);
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

    if (qs_drag) {                                           /* dragging the volume slider */
        if (!(b & 1)) qs_drag = false;
        else { struct rect m = qs_rect(); set_volume((x - (m.x + 64)) * 100 / (m.w - 64 - 58)); }
        return;
    }
    if (sw_open) { sw_mouse(x, y, down); return; }
    if (wmenu_win && wmenu_mouse(x, y, down || ((b & 2) && !(ob & 2)))) return;
    if (cn_open && cn_mouse(x, y, down, dz)) return;
    if (qs_open && qs_mouse(x, y, down, dz)) return;
    if (toasts_mouse(x, y, down)) return;

    if (ctx_open) {
        int ci = ctx_at(x, y);
        if (ci != ctx_hover) { ctx_hover = ci; ctx_damage(); }
        if (down || ((b & 2) && !(ob & 2))) { ctx_activate(ci); if (ci >= 0) return; }
        else if (ci >= 0) return;
    }

    bool in_frame = false;
    struct window *w = window_at(x, y, &in_frame);
    int mitem = menu_at(x, y);
    int task = taskbar_at(x, y);
    struct rect m = menu_rect();
    bool in_menu = menu_open && x >= m.x && x < m.x + m.w && y >= m.y && y < m.y + m.h;
    if (in_menu || task != T_NONE) w = 0;
    int btn = w && in_frame ? title_button_at(w, x, y) : -1;
    set_hover(w, btn, task == T_NONE ? -1 : task, mitem);

    if (w && !in_frame && !w->on_event) {                   /* motion inside a user client */
        static struct window *last_in;
        if (last_in && last_in != w) { struct gui_event e = { .type = EV_MOUSE_LEAVE }; wm_post(last_in, &e); }
        last_in = w;
        if (ox != x || oy != y) { struct gui_event e = { .type = EV_MOUSE_MOVE, .x = x - w->x, .y = y - w->y, .button = b }; send(w, &e); }
    }
    if (dz && task == T_VOL) set_volume(vol_cache - (dz > 0 ? 5 : -5));
    if (dz && w && !in_frame) { struct gui_event e = { .type = EV_SCROLL, .x = x - w->x, .y = y - w->y, .dz = dz }; send(w, &e); }

    for (int k = 2; k <= 4; k += 2)                                          /* right and middle */
        if ((b & k) && !(ob & k) && w && !in_frame) { struct gui_event e = { .type = EV_MOUSE_DOWN, .x = x - w->x, .y = y - w->y, .button = k }; send(w, &e); }
    if ((b & 2) && !(ob & 2) && w && in_frame && y < w->y && !(w->flags & WIN_FLAG_FRAMELESS)) {   /* window menu */
        close_popups();
        wm_focus(w);
        wmenu_open(w, x, y);
        return;
    }
    if ((b & 2) && !(ob & 2) && !w && task == T_NONE && !in_menu) {             /* desktop context menu */
        if (menu_open) launch(-1);
        struct rect r = ctx_rect();
        ctx_x = imin(x, SW - CTX_W - 8); ctx_y = imin(y, TB_Y - r.h - 8);
        ctx_open = true; ctx_hover = -1;
        ctx_damage();
        return;
    }
    if (!down) return;

    if (in_menu) { if (mitem >= 0) launch(mitem); return; }
    if (menu_open && task != T_LAUNCH) launch(-1);                 /* click elsewhere closes the menu */
    if (task == T_LAUNCH) { menu_open = !menu_open; if (menu_open) load_apps(); wm_damage(0, 0, SW, SH); return; }
    if (task >= 0) {
        struct window *tw = task_at(task);
        if (tw == focused && !tw->minimized) { tw->minimized = true; damage_window(tw); focus_top(); }
        else { tw->minimized = false; wm_focus(tw); }
        return;
    }
    if (task <= T_WS0 && task > T_WS0 - WM_WS) { switch_ws(T_WS0 - task); return; }
    if (task == T_VOL || task == T_NET) { qs_toggle(); return; }
    if (task == T_CLOCK) { cn_toggle(); return; }
    if (task == T_GAP) return;
    if (!w) { if (focused) { struct window *o = focused; focused = 0; damage_window(o); wm_damage(0, TB_Y, SW, WM_TASKBAR_H); } return; }

    if ((keyboard_mods() & MOD_SUPER) && !(w->flags & WIN_FLAG_FRAMELESS)) {   /* Super+drag moves from anywhere */
        super_alone = false;
        wm_focus(w);
        drag_win = w; drag_dx = x - w->x; drag_dy = y - w->y; drag_sx = x; drag_sy = y;
        return;
    }
    if (!(w->flags & WIN_FLAG_FRAMELESS)) wm_focus(w);
    if (in_frame) {
        if (btn == 0) { close_window(w); return; }
        if (btn == 1) { w->minimized = true; damage_window(w); focus_top(); return; }
        if (btn == 2) { toggle_max(w); return; }
        if (y < w->y) {
            if (last_click_win == w && ticks_ms - last_click_ms < 400) { toggle_max(w); last_click_win = 0; return; }
            last_click_win = w; last_click_ms = ticks_ms;
            drag_win = w; drag_dx = x - w->x; drag_dy = y - w->y; drag_sx = x; drag_sy = y;
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
    if (ev->key == KEY_SUPER) {
        if (ev->pressed) super_alone = !(mods & ~MOD_SUPER);
        else if (super_alone) { menu_open = !menu_open; if (menu_open) load_apps(); wm_damage(0, 0, SW, SH); }
        return;
    }
    if (!ev->pressed && sw_open && (ev->key == KEY_LALT || ev->key == KEY_RALT)) sw_close(true);   /* release Alt: switch */
    if (ev->pressed) {
        super_alone = false;
        if (sw_open) {
            if (ev->key == KEY_TAB) { sw_step(mods & MOD_SHIFT ? -1 : 1); return; }
            if (ev->key == KEY_RIGHT || ev->key == KEY_DOWN) { sw_step(1); return; }
            if (ev->key == KEY_LEFT || ev->key == KEY_UP) { sw_step(-1); return; }
            if (ev->key == KEY_ESC) { sw_close(false); return; }
            if (ev->key == KEY_ENTER) { sw_close(true); return; }
        }
        if (mods & MOD_SUPER) {                           /* workspaces + notification center */
            if ((mods & MOD_CTRL) && (ev->key == KEY_LEFT || ev->key == KEY_RIGHT)) { switch_ws(cur_ws + (ev->key == KEY_LEFT ? -1 : 1)); return; }
            if (ev->key >= '1' && ev->key < '1' + WM_WS) {
                if (!(mods & MOD_SHIFT)) switch_ws(ev->key - '1');
                else if (focused) move_to_ws(focused, ev->key - '1');
                return;
            }
            if (ev->key == 'n' || ev->key == 'N') { cn_toggle(); return; }
        }
        if ((mods & MOD_SUPER) && focused && !(focused->flags & WIN_FLAG_FRAMELESS)) {   /* Super+arrows tile */
            struct window *w = focused;
            if (ev->key == KEY_LEFT) { apply_snap(w, 1); return; }
            if (ev->key == KEY_RIGHT) { apply_snap(w, 2); return; }
            if (ev->key == KEY_UP) { apply_snap(w, 3); return; }
            if (ev->key == KEY_DOWN) { if (w->maximized) toggle_max(w); return; }
        }
        if ((mods & MOD_CTRL) && (mods & MOD_ALT) && (ev->key == 't' || ev->key == 'T')) { termwin_open(0); return; }
        if ((mods & MOD_ALT) && ev->key == KEY_TAB) { sw_start(mods & MOD_SHIFT); return; }
        if ((mods & MOD_ALT) && ev->key == KEY_F4) { if (focused) close_window(focused); return; }
        if (ev->key == KEY_ESC && (menu_open || ctx_open || wmenu_win || cn_open || qs_open)) { close_popups(); return; }
        if (ev->key == KEY_F11 && focused) { toggle_max(focused); return; }
        if (ev->key == KEY_PRTSC) { wm_spawn_app("/bin/screenshot", 0); return; }
    }
    if (ev->key == KEY_PRTSC) return;
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

/* /dev/screen: u32 w, u32 h, then the composed screen as 0xAARRGGBB rows (what a user process encodes for PrtSc) */
int wm_screen_read(uint32_t off, void *buf, uint32_t n) {
    if (!running) return 0;
    uint32_t total = 8 + (uint32_t)SW * SH * 4, done = 0;
    uint8_t *o = buf;
    while (done < n && off + done < total) {
        uint32_t p = off + done, k;
        if (p < 8) { uint32_t hdr[2] = { SW, SH }; k = imin(8 - p, n - done); memcpy(o + done, (uint8_t *)hdr + p, k); }
        else { k = imin(total - p, n - done); memcpy(o + done, (uint8_t *)bb.px + (p - 8), k); }
        done += k;
    }
    return done;
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
    for (uint32_t va = 0xA0000000u; va + len <= 0xB0000000u; va += 0x01000000u) {   /* 16 MiB slots: room to regrow */
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
        wi->stride = w->canvas.stride;
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
    case WIN_SET_PREFS:
        load_prefs(false);
        return 0;
    case WIN_SET_VISIBLE:
        w->visible = c != 0;
        damage_window(w);
        if (!w->visible && focused == w) focus_top();
        return 0;
    }
    return -EINVAL;
}

/* ================= display mode changes ================= */

/* which snap zone (1 left, 2 right, 3 maximised) a maximised window fills */
static int zone_of(struct window *w) {
    for (int z = 1; z <= 2; z++) { struct rect r = snap_rect(z); if (w->x == r.x && w->w == r.w) return z; }
    return 3;
}

/* fit r (client rect) on the screen: shrink if resizable, then pull it inside */
static void fit_rect(struct rect *r, bool noresize) {
    if (!noresize) { r->w = imin(r->w, SW - 2); r->h = imin(r->h, TB_Y - WM_TITLE_H - 1); }
    r->x = imax(1, imin(r->x, SW - 1 - r->w));
    r->y = imax(WM_TITLE_H, imin(r->y, TB_Y - 1 - r->h));
}

/* Allocate everything first (back buffer, wallpaper, bigger window buffers), so a failure leaves
 * the old mode intact; then program the adapter, swap the buffers in, re-lay-out the windows. */
int wm_set_mode(int nw, int nh) {
    if (nw == SW && nh == SH) return 0;
    struct regrow { struct window *w; uint32_t *buf, frames; int zone; } *rg;
    int n = 0, r = -ENOMEM;
    for (struct window *w = wins; w; w = w->next) n++;
    if (!(rg = kzalloc((n + 1) * sizeof *rg))) return -ENOMEM;
    big_free(under, SW, SH); big_free(ws_old, SW, SH);
    under = ws_old = 0;
    uint32_t *nb = big_alloc(nw, nh), *nwall = big_alloc(nw, nh);
    if (!nb || !nwall) goto fail;
    n = 0;
    for (struct window *w = wins; w; w = w->next, n++) {
        rg[n].w = w;
        rg[n].zone = w->maximized ? zone_of(w) : 0;
        /* ponytail: buffers only grow (stride >= width, capacity >= stride * height). A client may be
         * mid-frame with the old stride, so shrinking could fault it; closing the window frees it. */
        uint32_t frames = MAX(SCREEN_FRAMES(nw, nh), w->buf_frames);
        if (w->canvas.stride >= nw && (uint64_t)w->canvas.stride * nh * 4 <= (uint64_t)w->buf_frames * PAGE_SIZE) continue;
        if (!(rg[n].buf = (uint32_t *)pmm_alloc_contig(frames))) goto fail;
        rg[n].frames = frames;
    }
    if ((r = display_mode(nw, nh)) < 0) goto fail;

    close_popups();
    drag_win = resize_win = capture_win = 0;
    snap_zone = 0;
    big_free(bb.px, SW, SH); big_free(wall.px, SW, SH);
    SW = nw; SH = nh;
    canvas_init(&bb, nb, SW, SH, SW);
    canvas_init(&wall, nwall, SW, SH, SW);
    wallpaper_render(&wall, wall_theme);
    wallpaper_watermark(&wall);
    cur_x = imin(cur_x, SW - 1); cur_y = imin(cur_y, SH - 1);
    for (int i = 0; i < n; i++) {
        struct window *w = rg[i].w;
        if (rg[i].buf) {                                  /* move to the bigger buffer, same user address */
            int os = w->canvas.stride;
            for (int y = 0; y < w->h; y++) memcpy(rg[i].buf + y * SW, w->buf + y * os, w->w * 4);
            if (w->user_va) {
                struct task *o = task_find(w->owner_pid);
                if (o && o->dir == w->owner_dir) {
                    vmm_unmap_range(w->owner_dir, w->user_va, w->buf_frames * PAGE_SIZE, false);
                    for (uint32_t k = 0; k < rg[i].frames; k++)   /* the 16 MiB slot is ours (map_into_owner) */
                        vmm_map(w->owner_dir, w->user_va + k * PAGE_SIZE, (uint32_t)rg[i].buf + k * PAGE_SIZE, PTE_U | PTE_W | PTE_NOFREE);
                }
            }
            pmm_free_contig((uint32_t)w->buf, w->buf_frames);
            w->buf = rg[i].buf; w->buf_frames = rg[i].frames;
            canvas_init(&w->canvas, w->buf, w->w, w->h, SW);
            struct gui_event e = { .type = EV_RESIZE, .x = SW, .w = w->w, .h = w->h };   /* x = new stride */
            send(w, &e);
        }
        bool nores = w->flags & WIN_FLAG_NORESIZE;
        if (w->flags & WIN_FLAG_FRAMELESS) {
            set_geometry(w, imax(0, imin(w->x, SW - w->w)), imax(0, imin(w->y, SH - w->h)), w->w, w->h);
            continue;
        }
        if (w->maximized) {                               /* stays maximised / snapped, at the new size */
            fit_rect(&w->restore, nores);
            struct rect z = snap_rect(rg[i].zone);
            set_geometry(w, z.x, z.y, z.w, z.h);
            continue;
        }
        struct rect g = { w->x, w->y, w->w, w->h };
        fit_rect(&g, nores);
        set_geometry(w, g.x, g.y, g.w, g.h);
    }
    kfree(rg);
    wm_damage(0, 0, SW, SH);
    return 0;
fail:
    big_free(nb, nw, nh); big_free(nwall, nw, nh);
    for (int i = 0; i < n; i++) if (rg[i].buf) pmm_free_contig((uint32_t)rg[i].buf, rg[i].frames);
    kfree(rg);
    return r;
}

/* ================= main loop ================= */

bool wm_running(void) { return running; }
void wm_wake(void) { wakeup(&wm_chan); }

/* volume and network state can change behind our back (mixer ioctls, DHCP): poll cheaply, damage on change */
static void poll_tray(void) {
    int v = ac97_volume(-1);
    bool up = ni.up, pr = ni.present;
    uint32_t ip = ni.ip;
    if (v == vol_cache && up == net_up && pr == net_present && ip == net_ip) return;
    vol_cache = v; net_up = up; net_present = pr; net_ip = ip;
    wm_damage(TRAY_X, TB_Y, SW - TRAY_X, WM_TASKBAR_H);
    if (qs_open) qs_damage();
}

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
                wm_damage(TRAY_X, TB_Y, SW - TRAY_X, WM_TASKBAR_H);
            }
            poll_tray();
            for (int i = 0, k = 0; i < nhist; i++)                     /* toasts time out */
                if (hist[i].toast && ticks_ms - hist[i].born >= TOAST_MS) { if (!k++) toast_damage(); hist[i].toast = false; }
            uint32_t t = rtc_now() + tz_minutes * 60;
            if ((int)((t / 60) % 60) != last_minute) { last_minute = (t / 60) % 60; wm_damage(SW - 120, TB_Y, 120, WM_TASKBAR_H); }
        }
        if (compose()) {                                 /* animating: pace frames, let apps run */
            uint32_t f2 = irq_save();
            sleep_on(&wm_chan, 12);
            irq_restore(f2);
        }
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

/* startup chime: a rising arpeggio, played off the boot path */
static int chime(void *arg) {
    static const uint16_t notes[] = { 523, 659, 784, 1047 };
    for (unsigned i = 0; i < ARRAY_LEN(notes); i++) { speaker_tone(notes[i]); sleep_ms(i == 3 ? 220 : 90); }
    speaker_tone(0);
    return 0;
}

/* /etc/wallpaper (theme name) and /etc/tz (minutes east of UTC); Settings asks for a reload */
static void load_prefs(bool init) {
    int theme = wall_theme;
    void *d; uint32_t n;
    if (vfs_read_all("/etc/wallpaper", &d, &n) == 0) {
        for (int i = 0; i < wallpaper_count(); i++) if (!strncmp(d, wallpaper_name(i), strlen(wallpaper_name(i)))) theme = i;
        kfree(d);
    }
    if (init || theme != wall_theme) {
        wall_theme = theme;
        wallpaper_render(&wall, wall_theme);
        wallpaper_watermark(&wall);
    }
    if (vfs_read_all("/etc/tz", &d, &n) == 0) {
        char *p = d; int sign = 1, v = 0;
        if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        tz_minutes = sign * v;
        kfree(d);
    }
    last_minute = -1;                                      /* repaint the clock */
    if (!init) wm_damage(0, 0, SW, SH);
}

void wm_init(void) {
    SW = fb.w; SH = fb.h;
    uint32_t *b = big_alloc(SW, SH), *wp = big_alloc(SW, SH);
    if (!b || !wp) { kprintf("wm: no memory for buffers\n"); return; }
    canvas_init(&bb, b, SW, SH, SW);
    canvas_init(&wall, wp, SW, SH, SW);
    load_prefs(true);
    splash_progress(95);
    for (int d = 0; d <= SHADOW; d++) { int t = SHADOW - d; shadow_lut[d] = t * t * 110 / (SHADOW * SHADOW); }
    for (int i = 0; i < WM_RADIUS; i++) {
        int dy2 = (2 * (WM_RADIUS - i) - 1) * (2 * (WM_RADIUS - i) - 1);
        corner_inset[i] = WM_RADIUS - (isqrt(imax(0, 4 * WM_RADIUS * WM_RADIUS - dy2)) + 1) / 2;
    }
    mouse_pos(&cur_x, &cur_y);
    vol_cache = ac97_volume(-1);
    net_up = ni.up; net_present = ni.present; net_ip = ni.ip;
    load_apps();
    running = true;
    splash_progress(100);
    uint32_t *snap = big_alloc(SW, SH);
    if (snap) for (int y = 0; y < SH; y++) memcpy(snap + y * SW, fb.px + y * fb.stride, SW * 4);
    bootcon_enable(false);
    mouse_sink = irq_mouse;
    key_sink = irq_key;
    last_sample = ticks_ms; last_idle = idle_ms();
    wm_damage(0, 0, SW, SH);
    if (snap) { no_present = true; compose(); no_present = false; crossfade(snap, 32, 30); big_free(snap, SW, SH); }
    kthread_create("compositor", wm_thread, 0);
    kthread_create("chime", chime, 0);
    autostart();
    kprintf("[desktop] ready\n");
}
