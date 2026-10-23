/* wm.c - compositor + window manager.
 *
 * One kernel thread owns the screen. Input IRQs push into a ring and wake it;
 * it handles input, lets kernel windows (terminals) render, then recomposites
 * the damaged region (one bounding box) into a back buffer and copies that
 * region to the framebuffer. User windows draw into their own buffer (mapped
 * into the process) and call win_flip. Everything it draws follows docs/DESIGN.md. */
#include "wm.h"
#include "console.h"
#include "mouse.h"
#include "tty.h"
#include "net.h"
#include "vfs.h"
#include "design.h"

uint32_t rtc_now(void);
#include "wallpaper.h"
#include "icons.h"

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
static int cur_x, cur_y, cur_b, cur_shape;           /* cur_shape: the resize edges (E_*) the pointer shows, 0 = arrow */
static struct window *drag_win, *resize_win, *capture_win, *hover_win, *last_in;   /* last_in: client the pointer was last inside (gets EV_MOUSE_LEAVE) */
static int drag_dx, drag_dy, drag_sx, drag_sy, rs_edges;
static struct rect rs_from;                          /* client rect when a resize started */
static int snap_zone;                                /* 1 left half, 2 right half, 3 maximise */
static void draw_snap(struct canvas *c);
static void load_prefs(bool init);
static bool menu_open;                               /* the launcher */
static int hover_btn = -1, hover_task = -1;
static uint64_t last_click_ms; static struct window *last_click_win;
static int last_minute = -1;
static bool animating;                               /* something drawn this frame is mid-motion: compose again */
static uint16_t ease_o[257], ease_i[257];            /* cubic-bezier(0,0,0,1) entrances, (0.4,0,1,1) exits; 0..256 */
enum { E_L = 1, E_R = 2, E_T = 4, E_B = 8 };

/* ---- workspaces, overlays ---- */
static int cur_ws;
static bool super_alone;                                /* Super tapped on its own opens the launcher */
static bool sw_open;                                    /* Alt+Tab switcher */
static void sw_damage(void);
static void sw_close(bool commit);
static int vol_cache = -1;                              /* master volume, -1 without a mixer */
static bool net_up, net_present;
static uint32_t net_ip;
static int unseen;                                      /* notifications not yet looked at */
static bool cn_open, qs_open;                           /* notification centre, quick settings */
static inline bool onws(const struct window *w) { return w->ws < 0 || w->ws == cur_ws; }
static inline bool shown(const struct window *w) { return w->visible && !w->minimized && onws(w); }

/* ---- launcher entries: built-in terminal + /etc/apps ---- */
struct app { char name[32], path[64], arg[64], desc[48]; int icon; };   /* icon: APPICON_* or -1 */
static struct app apps[32];
static int napps;

/* ---- menus (desktop, window, panel overflow): one open at a time ---- */
enum { MK_DESK = 1, MK_WIN, MK_TASKS };
struct mitem { char label[48], hint[24]; short icon, act; uint8_t sep, danger; };
static struct mitem mi[16];
static int mn, mkind, msel = -1;
static bool mkbd;                                       /* the selection came from the keyboard: show the focus ring */
static struct rect mr;
static uint32_t m_born;
static int m_up;                                        /* opened upward: it slides down into place */
static struct window *mwin;
static void m_close(void);

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

#define TH cur_theme
#define ELEV_PAD(lv) (3 * (ELEV[lv].blur / 2) + ELEV[lv].dy)    /* how far a shadow reaches past its caster */
#define SH_PAD ELEV_PAD(E3)

/* screen-sized buffers (back buffer, wallpaper, window buffers, snapshots) come straight from the
 * frame allocator: at 1920x1080 four of them would fill the kernel heap */
#define SCREEN_FRAMES(w, h) ((uint32_t)(w) * (h) * 4 / PAGE_SIZE + 1)
static uint32_t *big_alloc(int w, int h) { return (uint32_t *)pmm_alloc_contig(SCREEN_FRAMES(w, h)); }
static void big_free(uint32_t *p, int w, int h) { if (p) pmm_free_contig((uint32_t)p, SCREEN_FRAMES(w, h)); }
static uint32_t *under, *ws_old;         /* lazily made: backdrop of something fading in, workspace-switch snapshot */

static void icon(struct canvas *c, int id, int size, int x, int y, color_t col) {
    const struct icon *i = icon_get(id, size);
    if (i) gfx_mask(c, x, y, i->a8, i->w, i->h, i->w, col);
}

/* premultiplied ARGB (app icons): d = s + d * (255 - a) / 255 */
static void blit_pm(struct canvas *c, int x, int y, const struct aicon *a) {
    if (!a) return;
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0), x1 = imin(x + a->w, c->cx1), y1 = imin(y + a->h, c->cy1);
    for (int yy = y0; yy < y1; yy++) {
        const uint32_t *s = &a->argb[(yy - y) * a->w - x];
        uint32_t *d = &c->px[yy * c->stride];
        for (int xx = x0; xx < x1; xx++) {
            uint32_t p = s[xx], al = p >> 24;
            if (!al) continue;
            if (al == 255) { d[xx] = p; continue; }
            uint32_t k = 255 - al, q = d[xx];
            k += k >> 7;                                    /* 0..256: channels split cleanly with >> 8 */
            d[xx] = 0xFF000000u | (((p & 0xFF00FF) + (((q & 0xFF00FF) * k >> 8) & 0xFF00FF)) + ((p & 0xFF00) + (((q & 0xFF00) * k >> 8) & 0xFF00)));
        }
    }
}

static inline bool inside(struct rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

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

static void dmg(struct rect r, int pad) { wm_damage(r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad); }

static inline int win_r(const struct window *w) { return w->maximized ? R_0 : WM_RADIUS; }

/* the frame is the title bar plus the client; the 1px ring sits just outside it */
static void frame_rect(struct window *w, struct rect *r) {
    if (w->flags & WIN_FLAG_FRAMELESS) { *r = (struct rect){ w->x, w->y, w->w, w->h }; return; }
    *r = (struct rect){ w->x, w->y - WM_TITLE_H, w->w, w->h + WM_TITLE_H };
}

static void damage_window(struct window *w) {
    struct rect r;
    frame_rect(w, &r);
    dmg(r, SH_PAD);
}

static void damage_title(struct window *w) {
    if (w->flags & WIN_FLAG_FRAMELESS) return;
    wm_damage(w->x, w->y - WM_TITLE_H, w->w, WM_TITLE_H);
}

void wm_damage_client(struct window *w, int x, int y, int cw, int ch) {
    if (!shown(w)) return;
    if (sw_open) sw_damage();                         /* live thumbnails */
    x = imax(0, x); y = imax(0, y);
    cw = imin(cw, w->w - x); ch = imin(ch, w->h - y);
    wm_damage(w->x + x, w->y + y, cw, ch);
}

/* ================= motion ================= */

/* cubic-bezier(x1,y1,x2,y2) (control points in 1/256) as a 257-entry table: progress -> eased progress, 0..256 */
static void bezier_lut(uint16_t *lut, int x1, int y1, int x2, int y2) {
    int k = 0;
    for (int64_t t = 0; t <= 1024; t++) {
        int64_t u = 1024 - t, a = 3 * u * u * t, b = 3 * u * t * t, c = t * t * t;
        int bx = (int)((a * x1 + b * x2 + c * 256) >> 30), by = (int)((a * y1 + b * y2 + c * 256) >> 30);
        while (k <= bx && k <= 256) lut[k++] = by;
    }
    while (k <= 256) lut[k++] = 256;
}

/* entrance progress (eased, 0..256) of something shown at `born` lasting dur ms (window animations off: a D_REDUCED
 * fade); while it runs it asks for another frame over r grown by pad */
static int enter_e(uint32_t born, int dur, struct rect r, int pad) {
    int d = theme_anim ? dur : D_REDUCED;
    uint32_t age = ticks_ms - born;
    if (age >= (uint32_t)d) return 256;
    animating = true;
    dmg(r, pad);
    return ease_o[age * 256 / d];
}
static inline int slide(int e, int px) { return theme_anim ? px * (256 - e) / 256 : 0; }

/* fade: keep what's under r, draw, then mix the new pixels over the kept ones */
static struct rect fade_r;
static bool fade_begin(struct canvas *c, struct rect r, int pad) {
    if (!under && !(under = big_alloc(SW, SH))) return false;
    int x0 = imax(c->cx0, r.x - pad), y0 = imax(c->cy0, r.y - pad);
    int x1 = imin(c->cx1, r.x + r.w + pad), y1 = imin(c->cy1, r.y + r.h + pad);
    if (x0 >= x1 || y0 >= y1) return false;
    fade_r = (struct rect){ x0, y0, x1 - x0, y1 - y0 };
    for (int y = y0; y < y1; y++) memcpy(&under[y * SW + x0], &bb.px[y * SW + x0], (x1 - x0) * 4);
    return true;
}
static void fade_end(int a) {
    for (int y = fade_r.y; y < fade_r.y + fade_r.h; y++) {
        uint32_t *o = &bb.px[y * SW], *u = &under[y * SW];
        for (int x = fade_r.x; x < fade_r.x + fade_r.w; x++) o[x] = mix(u[x], o[x], a);
    }
}

/* ================= windows ================= */

static uint8_t corner_inset[WM_RADIUS];              /* client pixels the rounded bottom corners leave out, per row from the bottom */

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

/* title bar controls: 0 minimise, 1 maximise, 2 close; 32x24 hit areas 4 apart, 6 from the right edge */
static int ctl_x(const struct window *w, int i) { return w->x + w->w - 6 - W_WINCTL - (2 - i) * (W_WINCTL + SP_2); }
#define CTL_Y(w) ((w)->y - WM_TITLE_H + (WM_TITLE_H - H_WINCTL) / 2)

static void blit_client(struct canvas *c, struct window *w, int rad) {
    for (int row = 0; row < w->h; row++) {
        int y = w->y + row;
        if (y < c->cy0 || y >= c->cy1) continue;
        int from_bottom = w->h - 1 - row, inset = rad && from_bottom < rad ? corner_inset[from_bottom] : 0;
        int x0 = imax(c->cx0, w->x + inset), x1 = imin(c->cx1, w->x + w->w - inset);
        if (x0 >= x1) continue;
        memcpy(&c->px[y * c->stride + x0], &w->buf[row * w->canvas.stride + (x0 - w->x)], (x1 - x0) * 4);
    }
}

/* the frame without its shadow: title bar, client, ring, highlight */
static void draw_frame(struct canvas *c, struct window *w) {
    struct rect r;
    frame_rect(w, &r);
    bool foc = w == focused;
    int rad = win_r(w);
    gfx_rrect_fill(c, r.x, r.y, r.w, WM_TITLE_H, RADII(rad, rad, 0, 0), foc ? TH.window : TH.base);
    if (rad)   /* anti-aliased bottom corners in the client's own corner colour; the client blit leaves them out */
        gfx_rrect_fill(c, r.x, r.y + r.h - rad, r.w, rad, RADII(0, 0, rad, rad), 0xFF000000u | w->buf[(w->h - 1) * w->canvas.stride]);
    blit_client(c, w, rad);
    gfx_ring_out(c, r.x, r.y, r.w, r.h, rad, TH.ring);
    if (foc && ALPHA(TH.hilite)) gfx_hline(c, r.x + rad, r.y, r.w - 2 * rad, TH.hilite);
    icon(c, w->icon, ICON_SM, r.x + SP_4, r.y + (WM_TITLE_H - ICON_SM) / 2, TH.text2);
    int tx = r.x + SP_4 + ICON_SM + SP_3, tmax = ctl_x(w, 0) - SP_3 - tx;
    if (w->topmost) {
        tmax -= ICON_SM + SP_3;
        icon(c, ICON_PIN, ICON_SM, ctl_x(w, 0) - SP_3 - ICON_SM, r.y + (WM_TITLE_H - ICON_SM) / 2, TH.text2);
    }
    if (tmax > 16) gfx_text_ell(c, T_BODY_B, tx, r.y + (WM_TITLE_H - T_BODY_B->height) / 2, tmax, w->title, foc ? TH.text : TH.text2);
    static const short ids[3] = { ICON_MINIMIZE, ICON_MAXIMIZE, ICON_CLOSE };
    for (int i = 0; i < 3; i++) {
        int bx = ctl_x(w, i), by = CTL_Y(w), id = i == 1 && w->maximized ? ICON_RESTORE : ids[i];
        bool dis = i == 1 && (w->flags & WIN_FLAG_NORESIZE), hot = !dis && hover_win == w && hover_btn == i;
        color_t g = dis ? TH.disabled : foc ? TH.text2 : TH_A(TH.text2, 128);
        if (hot) {
            gfx_round_fill(c, bx, by, W_WINCTL, H_WINCTL, R_2, i == 2 ? TH.danger : TH.hover_ov);
            g = i == 2 ? TH.on_danger : TH.text;
        }
        icon(c, id, ICON_SM, bx + (W_WINCTL - ICON_SM) / 2, by + (H_WINCTL - ICON_SM) / 2, g);
    }
}

static void draw_window(struct canvas *c, struct window *w) {
    struct rect r;
    frame_rect(w, &r);
    if (r.x - SH_PAD >= c->cx1 || r.y - SH_PAD >= c->cy1 || r.x + r.w + SH_PAD <= c->cx0 || r.y + r.h + SH_PAD <= c->cy0)
        return;
    if (w->flags & WIN_FLAG_DESKTOP) {                  /* ARGB layer over the wallpaper: no shadow, alpha 0 is skipped */
        int x0 = imax(c->cx0, w->x), x1 = imin(c->cx1, w->x + w->w), y0 = imax(c->cy0, w->y), y1 = imin(c->cy1, w->y + w->h);
        for (int y = y0; y < y1; y++) {
            uint32_t *d = &c->px[y * c->stride], *s = &w->buf[(y - w->y) * w->canvas.stride - w->x];
            for (int x = x0; x < x1; x++) if (s[x]) d[x] = blend(d[x], s[x]);
        }
        return;
    }
    int lv = w == focused ? E3 : E2;
    gfx_elev(c, r.x, r.y, r.w, r.h, w->flags & WIN_FLAG_FRAMELESS ? 0 : win_r(w), lv, TH.elev[lv]);
    if (w->flags & WIN_FLAG_FRAMELESS) { gfx_blit(c, w->x, w->y, w->buf, w->w, w->h, w->canvas.stride); return; }
    draw_frame(c, w);
}

/* opening (or restoring): scale 0.97 -> 1 and fade in over D_SLOW (window animations off: a D_REDUCED fade) */
static void draw_window_open(struct canvas *c, struct window *w, int e) {
    struct rect r;
    frame_rect(w, &r);
    if (!fade_begin(c, r, SH_PAD)) { draw_window(c, w); return; }
    draw_window(c, w);
    struct rect R = fade_r;
    int s = theme_anim ? 993 + 31 * e / 256 : 1024, cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (s < 1024) {
        /* shrink in place about the centre: rows and columns are walked outward, so every source pixel (further out)
         * is read before it is overwritten; sources outside R are the backdrop */
        int maxk = imax(cy - R.y, R.y + R.h - cy) + 1;
        for (int k = 0; k < maxk; k++)
            for (int side = 0; side < 2; side++) {
                int y = side ? cy + k : cy - 1 - k;
                if (y < R.y || y >= R.y + R.h) continue;
                int sy = cy + (y - cy) * 1024 / s;
                bool in = sy >= R.y && sy < R.y + R.h;
                uint32_t *d = &bb.px[y * SW], *u = &under[y * SW], *src = &bb.px[(in ? sy : y) * SW];
                for (int x = imax(cx, R.x); x < R.x + R.w; x++) { int sx = cx + (x - cx) * 1024 / s; d[x] = in && sx < R.x + R.w ? src[sx] : u[x]; }
                for (int x = imin(cx - 1, R.x + R.w - 1); x >= R.x; x--) { int sx = cx + (x - cx) * 1024 / s; d[x] = in && sx >= R.x ? src[sx] : u[x]; }
            }
    }
    fade_end(e);
}

/* closing or minimising: a snapshot of the frame fades out and shrinks to 0.97 over D_EXIT(D_SLOW) */
static struct { uint32_t *px; struct rect r; int rad, lv; uint32_t born, frames; } gh;

static void ghost_free(void) {
    if (!gh.px) return;
    dmg(gh.r, SH_PAD);
    pmm_free_contig((uint32_t)gh.px, gh.frames);
    gh.px = 0;
}

static void ghost_make(struct window *w) {
    ghost_free();
    if (!running || !shown(w) || (w->flags & WIN_FLAG_FRAMELESS)) return;
    struct rect r;
    frame_rect(w, &r);
    uint32_t frames = SCREEN_FRAMES(r.w, r.h);
    if (!(gh.px = (uint32_t *)pmm_alloc_contig(frames))) return;
    memset32(gh.px, 0, (size_t)r.w * r.h);
    struct canvas g;                                   /* screen coordinates over the snapshot */
    canvas_init(&g, gh.px - (r.y * r.w + r.x), r.x + r.w, r.y + r.h, r.w);
    canvas_clip(&g, r.x, r.y, r.w, r.h);
    draw_frame(&g, w);
    gh.r = r; gh.rad = win_r(w); gh.lv = w == focused ? E3 : E2; gh.born = ticks_ms; gh.frames = frames;
    dmg(r, SH_PAD);
}

static void draw_ghost(struct canvas *c) {
    if (!gh.px) return;
    int d = theme_anim ? D_EXIT(D_SLOW) : D_REDUCED;
    uint32_t age = ticks_ms - gh.born;
    if (age >= (uint32_t)d) { ghost_free(); return; }
    animating = true;
    dmg(gh.r, SH_PAD);
    int e = ease_i[age * 256 / d], a = 256 - e, s = theme_anim ? 1024 - 31 * e / 256 : 1024;
    struct rect r = gh.r;
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, w = r.w * s / 1024, h = r.h * s / 1024, x = cx - w / 2, y = cy - h / 2;
    gfx_elev(c, x, y, w, h, gh.rad, gh.lv, TH.elev[gh.lv] * a / 256);
    for (int yy = imax(y, c->cy0); yy < imin(y + h, c->cy1); yy++) {
        const uint32_t *src = &gh.px[imax(0, imin(r.h - 1, cy + (yy - cy) * 1024 / s - r.y)) * r.w];
        uint32_t *o = &c->px[yy * c->stride];
        for (int xx = imax(x, c->cx0); xx < imin(x + w, c->cx1); xx++) {
            uint32_t p = src[imin(r.w - 1, imax(0, cx + (xx - cx) * 1024 / s - r.x))];
            if (p) o[xx] = mix(o[xx], p, a);
        }
    }
}

/* the Lucide symbol that stands for an app in title bars, panel buttons and notifications */
static int app_sym(int appicon) {
    static const short m[][2] = {
        { APPICON_TERMINAL, ICON_TERMINAL }, { APPICON_BROWSER, ICON_GLOBE }, { APPICON_FILES, ICON_FOLDER },
        { APPICON_EDITOR, ICON_FILE_TEXT }, { APPICON_PAINT, ICON_BRUSH }, { APPICON_CALC, ICON_CALCULATOR },
        { APPICON_CLOCK, ICON_CLOCK }, { APPICON_SYSMON, ICON_ACTIVITY }, { APPICON_SNAKE, ICON_GAMEPAD },
        { APPICON_TETRIS, ICON_GAMEPAD }, { APPICON_MINES, ICON_BOMB }, { APPICON_DOOM, ICON_GAMEPAD },
        { APPICON_PIANO, ICON_KEYBOARD }, { APPICON_CODE, ICON_CODE }, { APPICON_CHESS, ICON_GAMEPAD },
        { APPICON_G2048, ICON_GRID }, { APPICON_SOLITAIRE, ICON_GAMEPAD }, { APPICON_SETTINGS, ICON_SETTINGS },
        { APPICON_ABOUT, ICON_INFO }, { APPICON_PHOTOS, ICON_IMAGE }, { APPICON_MUSIC, ICON_MUSIC }, { APPICON_PYPAINT, ICON_BRUSH },
    };
    for (unsigned i = 0; i < ARRAY_LEN(m); i++) if (m[i][0] == appicon) return m[i][1];
    return ICON_APP_WINDOW;
}

/* the /etc/apps entry of the calling process (by program name), -1 if none */
static int proc_app(void) {
    const char *n = current && current->proc ? current->proc->name : "";
    for (int i = 0; i < napps; i++) {
        const char *b = strrchr(apps[i].path, '/');
        if (b && !strcmp(b + 1, n)) return i;
    }
    return -1;
}

/* ================= panel (docs/DESIGN.md §9 Panel) ================= */
#define TB_Y (SH - WM_TASKBAR_H)
#define TASK_X0 (H_PANEL + SP_4 + 20 + 3 * (6 + 6) + SP_4)   /* after the launcher button and the workspace pills: 120 */
#define TRAY_CELL 32
#define MORE_W 32
enum { T_NONE = -3, T_LAUNCH = -2, T_GAP = -1, T_WS0 = -10, T_VOL = -20, T_NET = -21, T_CLOCK = -22, T_BELL = -23, T_MORE = -24 };
static struct window *tasks[32];                        /* the windows with buttons first (nshow), then the overflow */
static int ntasks, nshow, task_w, tray_x, ncell, clock_x, clock_w, cells[3];

static const char *const WDAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MON[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September",
                                   "October", "November", "December" };
/* "Thu 23 Oct  14:05", or with full set "Thursday 23 October 2026" */
static void clock_text(char *s, int n, bool full) {
    uint32_t t = rtc_now() + tz_minutes * 60;
    int z = t / 86400 + 719468, era = z / 146097, doe = z - era * 146097;      /* days -> civil date (H. Hinnant) */
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    const char *wd = WDAY[(t / 86400 + 4) % 7];
    if (full) snprintf(s, n, "%s %d %s %d", wd, d, MON[m - 1], y);
    else snprintf(s, n, "%.3s %d %.3s  %02u:%02u", wd, d, MON[m - 1], (t / 3600) % 24, (t / 60) % 60);
}

static void panel_layout(void) {
    ntasks = 0;
    for (struct window *w = wins; w && ntasks < (int)ARRAY_LEN(tasks); w = w->next)
        if (w->visible && onws(w) && !(w->flags & WIN_FLAG_FRAMELESS)) tasks[ntasks++] = w;
    for (int i = 1; i < ntasks; i++)                   /* creation order, so buttons don't jump when focus moves */
        for (int j = i; j > 0 && tasks[j - 1]->id > tasks[j]->id; j--) { struct window *t = tasks[j]; tasks[j] = tasks[j - 1]; tasks[j - 1] = t; }
    clock_x = SW - SP_4 - clock_w - SP_3;            /* clock_w: measured when the panel is drawn */
    ncell = 0;
    if (unseen) cells[ncell++] = T_BELL;
    if (net_present) cells[ncell++] = T_NET;
    if (vol_cache >= 0) cells[ncell++] = T_VOL;
    tray_x = clock_x - ncell * TRAY_CELL;
    int avail = tray_x - SP_3 - TASK_X0;
    nshow = ntasks;
    task_w = ntasks ? imin(W_TASK_MAX, (avail + SP_2) / ntasks - SP_2) : 0;
    if (ntasks && task_w < W_TASK_MIN) {               /* too many: minimum-width buttons, then "+N" for the rest */
        nshow = imax(0, (avail - MORE_W) / (W_TASK_MIN + SP_2));
        task_w = nshow ? imin(W_TASK_MAX, (avail - MORE_W) / nshow - SP_2) : 0;
        for (int i = nshow; i < ntasks && nshow; i++)  /* the focused window always keeps a button */
            if (tasks[i] == focused) { tasks[i] = tasks[nshow - 1]; tasks[nshow - 1] = focused; }
    }
}
static int task_x(int i) { return TASK_X0 + i * (task_w + SP_2); }

/* workspace pills: the current one 20 wide, the others 6, 6 apart; each owns the space halfway to its neighbours */
static int ws_px(int k) { int x = H_PANEL + SP_4; for (int i = 0; i < k; i++) x += (i == cur_ws ? 20 : 6) + 6; return x; }
static int ws_c(int k) { return ws_px(k) + (k == cur_ws ? 10 : 3); }
static void ws_slot(int k, int *x0, int *x1) {
    *x0 = k ? (ws_c(k - 1) + ws_c(k)) / 2 : H_PANEL;
    *x1 = k < WM_WS - 1 ? (ws_c(k) + ws_c(k + 1)) / 2 : TASK_X0 - SP_2;
}

static int vol_icon(void) { return vol_cache <= 0 ? ICON_VOLUME_MUTE : vol_cache < 50 ? ICON_VOLUME_LOW : ICON_VOLUME_HIGH; }

static void draw_taskbar(struct canvas *c) {
    char clk[40];
    clock_text(clk, sizeof clk, false);
    clock_w = gfx_text_width(T_BODY, clk);
    panel_layout();
    int y = TB_Y, by = y + 5;
    gfx_fill(c, 0, y, SW, WM_TASKBAR_H, TH.base);
    gfx_hline(c, 0, y, SW, TH.line);
    if (menu_open || hover_task == T_LAUNCH) gfx_fill(c, 0, y + 1, H_PANEL, H_PANEL - 1, menu_open ? TH.press_ov : TH.hover_ov);
    icon(c, ICON_GRID, ICON_MD, (H_PANEL - ICON_MD) / 2, y + (H_PANEL - ICON_MD) / 2, TH.text);
    if (hover_task <= T_WS0 && hover_task > T_WS0 - WM_WS) {
        int a, b;
        ws_slot(T_WS0 - hover_task, &a, &b);
        gfx_round_fill(c, a, y + 8, b - a, HIT_MIN, R_2, TH.hover_ov);
    }
    for (int k = 0; k < WM_WS; k++)
        gfx_round_fill(c, ws_px(k), y + (H_PANEL - 6) / 2, k == cur_ws ? 20 : 6, 6, 3, k == cur_ws ? TH.text : TH_A(TH.text2, 140));
    for (int i = 0; i < nshow; i++) {
        struct window *w = tasks[i];
        int x = task_x(i);
        bool act = w == focused && !w->minimized;
        if (act) gfx_round_fill(c, x, by, task_w, H_TASK, R_2, TH.raised);
        else if (hover_task == i) gfx_round_fill(c, x, by, task_w, H_TASK, R_2, TH.hover_ov);
        if (act) gfx_fill(c, x, y + 1, task_w, SIGNAL, TH.accent_line);   /* the signal line */
        icon(c, w->icon, ICON_SM, x + 10, by + (H_TASK - ICON_SM) / 2, act ? TH.text : TH.text2);
        gfx_text_ell(c, T_BODY, x + 10 + ICON_SM + SP_3, by + (H_TASK - T_BODY->height) / 2, task_w - 20 - ICON_SM - SP_3,
                     w->title, w->minimized ? TH.text2 : TH.text);
    }
    if (nshow < ntasks) {                               /* overflow button: "+N" opens the list of the rest */
        int x = task_x(nshow);
        if (hover_task == T_MORE || mkind == MK_TASKS) gfx_round_fill(c, x, by, MORE_W, H_TASK, R_2, mkind == MK_TASKS ? TH.press_ov : TH.hover_ov);
        char s[8];
        snprintf(s, sizeof s, "+%d", ntasks - nshow);
        gfx_text_center(c, T_BODY_B, x, by + (H_TASK - T_BODY_B->height) / 2, MORE_W, s, TH.text2);
    }
    for (int i = 0; i < ncell; i++) {
        int x = tray_x + i * TRAY_CELL, k = cells[i];
        if (hover_task == k || (k == T_BELL ? cn_open : qs_open)) gfx_round_fill(c, x, y + 4, TRAY_CELL, TRAY_CELL, R_2, TH.hover_ov);
        int id = k == T_BELL ? ICON_BELL : k == T_VOL ? vol_icon() : ICON_NETWORK;
        icon(c, id, ICON_SM, x + (TRAY_CELL - ICON_SM) / 2, y + 4 + (TRAY_CELL - ICON_SM) / 2, k == T_NET && !net_up ? TH.text2 : TH.text);
    }
    if (hover_task == T_CLOCK || cn_open) gfx_round_fill(c, clock_x, y + 4, SW - clock_x - SP_2, TRAY_CELL, R_2, TH.hover_ov);
    gfx_text(c, T_BODY, clock_x + SP_3, y + (H_PANEL - T_BODY->height) / 2, clk, TH.text);
}

/* ---- panel tooltips: after D_TOOLTIP over the same item ---- */
static int tip_for = T_NONE;
static uint32_t tip_since;
static bool tip_on;
static struct rect tip_r;

static void tip_text(char *s, int n, char *hint, int hn) {
    *s = *hint = 0;
    int t = tip_for;
    if (t == T_LAUNCH) { strlcpy(s, "Applications", n); strlcpy(hint, "Super", hn); }
    else if (t <= T_WS0 && t > T_WS0 - WM_WS) { snprintf(s, n, "Workspace %d", T_WS0 - t + 1); snprintf(hint, hn, "Super+%d", T_WS0 - t + 1); }
    else if (t >= 0 && t < nshow) strlcpy(s, tasks[t]->title, n);
    else if (t == T_MORE) snprintf(s, n, "%d more windows", ntasks - nshow);
    else if (t == T_NET && !net_up) strlcpy(s, "Not connected", n);
    else if (t == T_NET) snprintf(s, n, "Wired \xc2\xb7 %u.%u.%u.%u", net_ip & 255, net_ip >> 8 & 255, net_ip >> 16 & 255, net_ip >> 24);
    else if (t == T_VOL && !vol_cache) strlcpy(s, "Muted", n);
    else if (t == T_VOL) snprintf(s, n, "Volume %d%%", vol_cache);
    else if (t == T_BELL) snprintf(s, n, unseen == 1 ? "1 new notification" : "%d new notifications", unseen);
    else if (t == T_CLOCK) clock_text(s, n, true);
}

static int tip_anchor(int t, int *w) {
    if (t == T_LAUNCH) { *w = H_PANEL; return 0; }
    if (t <= T_WS0 && t > T_WS0 - WM_WS) { int a, b; ws_slot(T_WS0 - t, &a, &b); *w = b - a; return a; }
    if (t >= 0) { *w = task_w; return task_x(t); }
    if (t == T_MORE) { *w = MORE_W; return task_x(nshow); }
    if (t == T_CLOCK) { *w = SW - clock_x; return clock_x; }
    for (int i = 0; i < ncell; i++) if (cells[i] == t) { *w = TRAY_CELL; return tray_x + i * TRAY_CELL; }
    *w = 0;
    return -1;
}

static void tip_hide(void) { if (tip_on) dmg(tip_r, ELEV_PAD(E2)); tip_on = false; }

static void draw_tip(struct canvas *c) {
    char s[80], hint[16];
    tip_text(s, sizeof s, hint, sizeof hint);
    int aw, ax = tip_anchor(tip_for, &aw);
    if (!s[0] || ax < 0) return;
    char sep[24] = "";
    if (hint[0]) snprintf(sep, sizeof sep, " \xc2\xb7 %s", hint);
    int w = imin(W_TIP_MAX, gfx_text_width(T_BODY, s) + gfx_text_width(T_BODY, sep) + 2 * SP_3), h = T_BODY->height + 2 * SP_2;
    tip_r = (struct rect){ imax(SP_3, imin(SW - SP_3 - w, ax + aw / 2 - w / 2)), TB_Y - SP_3 - h, w, h };
    gfx_elev(c, tip_r.x, tip_r.y, w, h, R_2, E2, TH.elev[E2]);
    gfx_round_fill(c, tip_r.x, tip_r.y, w, h, R_2, TH.overlay);
    gfx_round_rect(c, tip_r.x, tip_r.y, w, h, R_2, TH.line);
    int tw = gfx_text_ell(c, T_BODY, tip_r.x + SP_3, tip_r.y + SP_2, w - 2 * SP_3, s, TH.text);
    if (hint[0]) gfx_text(c, T_BODY, tip_r.x + SP_3 + tw, tip_r.y + SP_2, sep, TH.dark ? TH.text2 : TH.text3);
}

/* ================= launcher (docs/DESIGN.md §9 Launcher) ================= */
#define L_PAD SP_5
#define L_COLW ((W_LAUNCHER - 2 * L_PAD - SP_3) / 2)
#define L_LIST_Y (L_PAD + H_SEARCH_LG + SP_5 + 14 + SP_2)    /* search, "All apps" (16 above, 4 below) */
static char lq[32];                                     /* the search text */
static int lhit[32], nhit, lsel = -1, ltop, lhov = -1;  /* matching apps, selected match (-1 none), first row shown, hovered footer button */
static bool lkbd;
static uint32_t l_born, l_typed;

static int l_allrows(void) { return (napps + 1) / 2; }  /* the list keeps its height while you type: it doesn't jump */
static int l_rows(void) { return imax(1, imin(l_allrows(), (TB_Y - SP_2 - SP_3 - L_LIST_Y - SP_3 - H_FOOTER) / H_ROW2)); }
static struct rect l_rect(void) {
    int h = L_LIST_Y + l_rows() * H_ROW2 + SP_3 + H_FOOTER;
    return (struct rect){ SP_3, TB_Y - SP_2 - h, W_LAUNCHER, h };
}
static int l_hrows(void) { return imax(l_rows(), (nhit + 1) / 2); }  /* rows of the result columns (column-major) */
/* result i's row rect; false if scrolled out */
static bool l_item(int i, struct rect *q) {
    struct rect m = l_rect();
    int rows = l_hrows(), col = i / rows, row = i % rows - ltop;
    if (row < 0 || row >= l_rows()) return false;
    *q = (struct rect){ m.x + L_PAD + col * (L_COLW + SP_3), m.y + L_LIST_Y + row * H_ROW2, L_COLW, H_ROW2 };
    return true;
}
/* footer buttons: 0 restart, 1 shut down */
static struct rect l_btn(int i) {
    struct rect m = l_rect();
    int w1 = 2 * 10 + ICON_SM + 6 + gfx_text_width(T_BODY, "Shut down"), w0 = 2 * 10 + ICON_SM + 6 + gfx_text_width(T_BODY, "Restart");
    int y = m.y + m.h - H_FOOTER + (H_FOOTER - H_CTL) / 2, x1 = m.x + m.w - SP_3 - w1;
    return i ? (struct rect){ x1, y, w1, H_CTL } : (struct rect){ x1 - SP_3 - w0, y, w0, H_CTL };
}

static bool has_ci(const char *h, const char *n) {
    for (; *h; h++) {
        int i = 0;
        while (n[i] && h[i] && (h[i] | 32) == (n[i] | 32)) i++;
        if (!n[i]) return true;
    }
    return !*n;
}

static void l_filter(void) {
    nhit = 0;
    for (int i = 0; i < napps && nhit < (int)ARRAY_LEN(lhit); i++)
        if (has_ci(apps[i].name, lq) || has_ci(apps[i].desc, lq)) lhit[nhit++] = i;
    lsel = lq[0] && nhit ? 0 : -1;                      /* Enter takes the first match */
    ltop = 0;
}

static void draw_launcher(struct canvas *c) {
    struct rect m = l_rect();
    int e = enter_e(l_born, D_NORMAL, m, ELEV_PAD(E4) + 8);
    int off = slide(e, 8);
    m.y += off;
    bool f = e < 256 && fade_begin(c, m, ELEV_PAD(E4));
    gfx_elev(c, m.x, m.y, m.w, m.h, R_3, E4, TH.elev[E4]);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, R_3, TH.overlay);
    gfx_round_rect(c, m.x, m.y, m.w, m.h, R_3, TH.line);
    /* search field: focused on open, filters as you type */
    int sx = m.x + L_PAD, sy = m.y + L_PAD, sw = m.w - 2 * L_PAD;
    gfx_round_fill(c, sx, sy, sw, H_SEARCH_LG, R_2, TH.raised);
    gfx_round_rect(c, sx, sy, sw, H_SEARCH_LG, R_2, TH.control);
    icon(c, ICON_SEARCH, ICON_SM, sx + SP_4, sy + (H_SEARCH_LG - ICON_SM) / 2, TH.text2);
    int tx = sx + SP_4 + ICON_SM + SP_3, ty = sy + (H_SEARCH_LG - T_BODY->height) / 2;
    int tw = lq[0] ? gfx_text_ell(c, T_BODY, tx, ty, sw - (tx - sx) - SP_4, lq, TH.text) : 0;
    if (!lq[0]) gfx_text(c, T_BODY, tx + 2, ty, "Search apps", TH.text3);
    uint32_t idle = ticks_ms - l_typed;
    if (idle >= D_CARET_IDLE || (idle / D_CARET) % 2 == 0) gfx_fill(c, tx + tw, sy + (H_SEARCH_LG - 16) / 2, 1, 16, TH.text);
    gfx_text(c, T_CAPTION_B, sx + SP_3, sy + H_SEARCH_LG + SP_5, lq[0] ? "Results" : "All apps", TH.text2);
    if (!nhit) {
        char s[64];
        snprintf(s, sizeof s, "No apps match \xe2\x80\x9c%s\xe2\x80\x9d", lq);
        gfx_text_center(c, T_BODY, m.x, m.y + L_LIST_Y + SP_5, m.w, s, TH.text2);
    }
    for (int i = 0; i < nhit; i++) {
        struct rect q;
        if (!l_item(i, &q)) continue;
        q.y += off;
        const struct app *a = &apps[lhit[i]];
        if (i == lsel) {
            gfx_round_fill(c, q.x, q.y, q.w, q.h, R_2, TH.hover_ov);
            if (lkbd) gfx_rrect_ring(c, q.x, q.y, q.w, q.h, RADII(R_2, R_2, R_2, R_2), FOCUS_W, TH.accent_text);
        }
        if (a->icon >= 0) blit_pm(c, q.x + SP_3, q.y + (H_ROW2 - ICON_XL) / 2, appicon_get(a->icon, ICON_XL));
        int lx = q.x + SP_3 + ICON_XL + SP_4, lw = q.w - (lx - q.x) - SP_3;
        int ny = q.y + (H_ROW2 - T_BODY->height - T_CAPTION->height) / 2;
        gfx_text_ell(c, T_BODY, lx, ny, lw, a->name, TH.text);
        gfx_text_ell(c, T_CAPTION, lx, ny + T_BODY->height, lw, a->desc, TH.text2);
    }
    if (l_hrows() > l_rows()) {                         /* overlay scrollbar */
        int th = imax(SCROLL_MIN, l_rows() * H_ROW2 * l_rows() / l_hrows()), span = l_rows() * H_ROW2 - th;
        int ty2 = m.y + L_LIST_Y + span * ltop / imax(1, l_hrows() - l_rows());
        gfx_round_fill(c, m.x + m.w - SP_1 - SCROLL_W, ty2, SCROLL_W, th, R_F(SCROLL_W), TH_A(TH.text, 89));
    }
    /* footer: who you are, restart, shut down */
    int fy = m.y + m.h - H_FOOTER;
    gfx_hline(c, m.x, fy, m.w, TH.line);
    gfx_round_fill(c, m.x + L_PAD, fy + (H_FOOTER - ICON_LG) / 2, ICON_LG, ICON_LG, R_F(ICON_LG), TH.base);
    icon(c, ICON_USER, ICON_SM, m.x + L_PAD + 4, fy + (H_FOOTER - ICON_SM) / 2, TH.text2);
    gfx_text(c, T_BODY, m.x + L_PAD + ICON_LG + SP_3, fy + (H_FOOTER - T_BODY->height) / 2, "vibe", TH.text);
    for (int i = 0; i < 2; i++) {
        struct rect b = l_btn(i);
        b.y += off;
        if (lhov == i) gfx_round_fill(c, b.x, b.y, b.w, b.h, R_2, TH.hover_ov);
        icon(c, i ? ICON_POWER : ICON_RESTART, ICON_SM, b.x + 10, b.y + (H_CTL - ICON_SM) / 2, TH.text2);
        gfx_text(c, T_BODY, b.x + 10 + ICON_SM + 6, b.y + (H_CTL - T_BODY->height) / 2, i ? "Shut down" : "Restart", TH.text);
    }
    if (f) fade_end(e);
}

/* ================= menus (docs/DESIGN.md §9 Menus) ================= */
static int m_ih(int i) { return (mi[i].sep ? 2 * SP_2 + 1 : 0) + H_ROW; }
static int m_iy(int i) { int y = mr.y + SP_2; for (int k = 0; k < i; k++) y += m_ih(k); return y + (mi[i].sep ? 2 * SP_2 + 1 : 0); }
static bool m_icons(void) { for (int i = 0; i < mn; i++) if (mi[i].icon >= 0) return true; return false; }

static void draw_mmenu(struct canvas *c) {
    struct rect m = mr;
    int e = enter_e(m_born, D_NORMAL, m, ELEV_PAD(E2) + SP_2);
    int dy = slide(e, SP_2) * (m_up ? 1 : -1);
    m.y += dy;
    bool f = e < 256 && fade_begin(c, m, ELEV_PAD(E2));
    gfx_elev(c, m.x, m.y, m.w, m.h, R_3, E2, TH.elev[E2]);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, R_3, TH.overlay);
    gfx_round_rect(c, m.x, m.y, m.w, m.h, R_3, TH.line);
    bool ic = m_icons();
    for (int i = 0; i < mn; i++) {
        int y = m_iy(i) + dy, x = m.x + SP_4;
        if (mi[i].sep) gfx_hline(c, m.x + SP_4, y - SP_2 - 1, m.w - 2 * SP_4, TH.line);
        if (i == msel) {
            gfx_round_fill(c, m.x + SP_2, y, m.w - 2 * SP_2, H_ROW, R_2, TH.hover_ov);
            if (mkbd) gfx_rrect_ring(c, m.x + SP_2, y, m.w - 2 * SP_2, H_ROW, RADII(R_2, R_2, R_2, R_2), FOCUS_W, TH.accent_text);
        }
        color_t fg = mi[i].danger ? TH.danger : TH.text;
        if (mi[i].icon >= 0) icon(c, mi[i].icon, ICON_SM, x, y + (H_ROW - ICON_SM) / 2, mi[i].danger ? TH.danger : TH.text2);
        if (ic) x += ICON_SM + SP_3;
        int hw = mi[i].hint[0] ? gfx_text_width(T_BODY, mi[i].hint) : 0, ty = y + (H_ROW - T_BODY->height) / 2;
        if (hw) gfx_text(c, T_BODY, m.x + m.w - SP_4 - hw, ty, mi[i].hint, TH.dark ? TH.text2 : TH.text3);
        gfx_text_ell(c, T_BODY, x, ty, m.x + m.w - SP_4 - (hw ? hw + SP_5 : 0) - x, mi[i].label, fg);
    }
    if (f) fade_end(e);
}

static int wall_theme;


/* /etc/wallpaper is a procedural theme's name or the path of an image. The kernel never decodes images: /bin/theme
 * does and pushes the pixels to /dev/wallpaper (wm_wallpaper_write), at the current screen size; so after a mode
 * change or at boot the procedural wallpaper stands in until it has done so again. */
static char wp_cfg[128];                                 /* /etc/wallpaper as last loaded: only a change there touches the wallpaper */
static bool wall_img, wp_redo;                           /* wall holds a pushed image; ask for a push on the next tick */
static uint32_t *wp_stage;                               /* an image arriving */
static int wp_pid;

static void render_wallpaper(void) {
    wallpaper_render(&wall, wall_theme);
    wall_img = false;
}

static void set_wallpaper(int theme) {
    wall_theme = theme % wallpaper_count();
    render_wallpaper();
    strlcpy(wp_cfg, wallpaper_name(wall_theme), sizeof wp_cfg);
    struct file *f;
    if (vfs_open("/", "/etc/wallpaper", O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) {
        vfs_write(f, wallpaper_name(wall_theme), strlen(wallpaper_name(wall_theme)));
        vfs_close(f);
    }
    wm_damage(0, 0, SW, SH);
}

/* text: a procedural wallpaper's name; binary: "WPIM", w, h (the screen size), then w*h XRGB pixels, written in order */
#define WPIM 0x4D495057u
int wm_wallpaper_write(uint32_t off, const void *buf, uint32_t n) {
    if (!running) return -ENODEV;
    const uint8_t *b = buf;
    if (off == 0 && n >= 4 && *(const uint32_t *)b != WPIM) {          /* a name */
        for (int i = 0; i < wallpaper_count(); i++)
            if (!strncmp(buf, wallpaper_name(i), strlen(wallpaper_name(i)))) { wall_theme = i; render_wallpaper(); wm_damage(0, 0, SW, SH); return n; }
        return -EINVAL;
    }
    uint32_t total = 12 + (uint32_t)SW * SH * 4;
    if (off == 0) {
        if (n < 12 || ((const uint32_t *)b)[1] != (uint32_t)SW || ((const uint32_t *)b)[2] != (uint32_t)SH) return -EINVAL;
        if (!wp_stage && !(wp_stage = big_alloc(SW, SH))) return -ENOMEM;
    }
    if (!wp_stage || off + n > total) return -EINVAL;
    uint32_t skip = off < 12 ? 12 - off : 0;
    if (n > skip) memcpy((uint8_t *)wp_stage + (off + skip - 12), b + skip, n - skip);
    if (off + n == total) {
        memcpy(wall.px, wp_stage, (size_t)SW * SH * 4);
        big_free(wp_stage, SW, SH); wp_stage = 0;
        wall_img = true;
        wm_damage(0, 0, SW, SH);
    }
    return n;
}


/* ---- cursor: an arrow, or a double-headed arrow across the edge being resized ---- */
static const char *cursor_img[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
    "X.........X", "X......XXXX", "X...X..X", "X..XX..X", "X.X  X..X", "XX   X..X", "X     X..X",
    "      X..X", "       XX",
};
/* double-headed resize arrows (white, black outline), 19x19, hotspot in the middle: ew, ns, nwse, nesw */
static const char *const resize_img[4][19] = {
    { "                   ", "                   ", "                   ", "                   ", "                   ", "    XXXX   XXXX    ", "   XX..X   X..XX   ", "  XX...XXXXX...XX  ", " XX.............XX ", " X...............X ", " XX.............XX ", "  XX...XXXXX...XX  ", "   XX..X   X..XX   ", "    XXXX   XXXX    ", "                   ", "                   ", "                   ", "                   ", "                   " },
    { "                   ", "        XXX        ", "       XX.XX       ", "      XX...XX      ", "     XX.....XX     ", "     X.......X     ", "     X.......X     ", "     XXX...XXX     ", "       X...X       ", "       X...X       ", "       X...X       ", "     XXX...XXX     ", "     X.......X     ", "     X.......X     ", "     XX.....XX     ", "      XX...XX      ", "       XX.XX       ", "        XXX        ", "                   " },
    { "                   ", "                   ", "                   ", "   XXXXXXXX        ", "   X......X        ", "   X.....XX        ", "   X.....X         ", "   X.....XX        ", "   X......XX XXX   ", "   X.XXX...XXX.X   ", "   XXX XX......X   ", "        XX.....X   ", "         X.....X   ", "        XX.....X   ", "        X......X   ", "        XXXXXXXX   ", "                   ", "                   ", "                   " },
    { "                   ", "                   ", "                   ", "        XXXXXXXX   ", "        X......X   ", "        XX.....X   ", "         X.....X   ", "        XX.....X   ", "   XXX XX......X   ", "   X.XXX...XXX.X   ", "   X......XX XXX   ", "   X.....XX        ", "   X.....X         ", "   X.....XX        ", "   X......X        ", "   XXXXXXXX        ", "                   ", "                   ", "                   " },
};
static void cursor_damage(int x, int y) { wm_damage(x - 12, y - 12, 32, 34); }

static void draw_cursor(struct canvas *c) {
    const char *const *img = cursor_img;
    int n = ARRAY_LEN(cursor_img), ox = 0, oy = 0;
    if (cur_shape) {
        bool h = cur_shape & (E_L | E_R), v = cur_shape & (E_T | E_B);
        img = resize_img[h && v ? ((cur_shape & E_L) == (cur_shape & E_T ? E_L : 0) ? 2 : 3) : v ? 1 : 0];
        n = 19; ox = oy = -9;
    }
    for (int y = 0; y < n; y++)
        for (int x = 0; img[y][x]; x++) {
            char p = img[y][x];
            if (p == 'X') gfx_pixel(c, cur_x + ox + x, cur_y + oy + y, RGB(0x14, 0x15, 0x17));
            else if (p == '.') gfx_pixel(c, cur_x + ox + x, cur_y + oy + y, RGB(255, 255, 255));
        }
}
/* ================= overlays: notifications, notification centre, quick settings, Alt+Tab ================= */

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

/* ---- notifications (docs/DESIGN.md §9 Notifications): the history (newest first) feeds both the stack and the centre ---- */
#define NQ 24
#define NVIS 3
#define NT_PAD SP_4
struct notif { char title[48], body[160], app[32]; uint8_t kind; short sym; uint32_t born, wall, until; bool toast; };   /* kind: 0 info, 1 good, 2 warn, 3 bad */
static struct notif hist[NQ];
static int nhist, toast_hov = -1;                       /* the toast under the pointer: they all wait while it's there */

static int n_icon(const struct notif *n, color_t *col) {
    if (n->kind == 1) { *col = TH.success; return ICON_SUCCESS; }
    if (n->kind == 2) { *col = TH.warning; return ICON_WARNING; }
    if (n->kind == 3) { *col = TH.danger; return ICON_ERROR; }
    *col = TH.text2;
    return n->sym;
}

static int n_height(const struct notif *n, int w, int lines) {
    int off[3], len[3], nl = wrap(T_BODY, n->body, w, off, len, lines);
    return ICON_SM + SP_3 + T_BODY_B->height + nl * T_BODY->height;
}

/* icon, app and time (or a close box when hot), title, body cut to `lines` lines */
static void draw_notif(struct canvas *c, const struct notif *n, int x, int y, int w, int lines, bool hot) {
    color_t ic;
    int id = n_icon(n, &ic);
    icon(c, id, ICON_SM, x, y, ic);
    char tm[8];
    uint32_t t = n->wall + tz_minutes * 60;
    snprintf(tm, sizeof tm, "%02u:%02u", (t / 3600) % 24, (t / 60) % 60);
    int tw = gfx_text_width(T_CAPTION, tm), cy = y + (ICON_SM - T_CAPTION->height) / 2;
    gfx_text_ell(c, T_CAPTION, x + ICON_SM + SP_3, cy, w - ICON_SM - SP_3 - tw - SP_5, n->app, TH.text2);
    if (hot) icon(c, ICON_CLOSE, ICON_SM, x + w - ICON_SM, y, TH.text);
    else gfx_text(c, T_CAPTION, x + w - tw, cy, tm, TH.text2);
    y += ICON_SM + SP_3;
    gfx_text_ell(c, T_BODY_B, x, y, w, n->title, TH.text);
    y += T_BODY_B->height;
    int off[3], len[3], nl = wrap(T_BODY, n->body, w, off, len, lines);
    for (int l = 0; l < nl; l++, y += T_BODY->height)
        if (l == nl - 1 && off[l] + len[l] < (int)strlen(n->body)) gfx_text_ell(c, T_BODY, x, y, w, n->body + off[l], TH.text2);
        else gfx_text_n(c, T_BODY, x, y, n->body + off[l], len[l], TH.text2);
}

/* the visible stack, bottom-right above the panel, newest at the bottom */
static int toast_layout(struct rect *r, struct notif **ns) {
    int k = 0, y = TB_Y - SP_4;
    for (int i = 0; i < nhist && k < NVIS; i++) {
        if (!hist[i].toast) continue;
        int h = 2 * NT_PAD + n_height(&hist[i], W_NOTIF - 2 * NT_PAD, 3);
        y -= h;
        r[k] = (struct rect){ SW - SP_4 - W_NOTIF, y, W_NOTIF, h };
        ns[k++] = &hist[i];
        y -= SP_3;
    }
    return k;
}

static void toast_damage(void) {
    struct rect r[NVIS]; struct notif *n[NVIS];
    int k = toast_layout(r, n);
    if (k) wm_damage(SW - SP_4 - W_NOTIF - ELEV_PAD(E2), r[k - 1].y - ELEV_PAD(E2), W_NOTIF + SP_4 + 2 * ELEV_PAD(E2), TB_Y - r[k - 1].y + ELEV_PAD(E2));
}

static void draw_toasts(struct canvas *c) {
    struct rect r[NVIS]; struct notif *n[NVIS];
    int k = toast_layout(r, n);
    for (int i = 0; i < k; i++) {
        struct rect q = r[i];
        int e = enter_e(n[i]->born, D_NORMAL, q, ELEV_PAD(E2) + 8);
        q.y += slide(e, 8);
        bool f = e < 256 && fade_begin(c, q, ELEV_PAD(E2));
        gfx_elev(c, q.x, q.y, q.w, q.h, R_3, E2, TH.elev[E2]);
        gfx_round_fill(c, q.x, q.y, q.w, q.h, R_3, TH.overlay);
        gfx_round_rect(c, q.x, q.y, q.w, q.h, R_3, TH.line);
        draw_notif(c, n[i], q.x + NT_PAD, q.y + NT_PAD, q.w - 2 * NT_PAD, 3, toast_hov == i);
        if (f) fade_end(e);
    }
}

/* ---- notification centre: the history, above the clock ---- */
#define CN_W (W_NOTIF + 2 * SP_4)
#define CN_HEAD 44
#define CN_ROW 96
#define CN_EMPTY 136
static int cn_off, cn_hov = -1, cn_clr_hot;
static uint32_t cn_born;
static int cn_vis(void) { return imax(1, imin(imax(1, nhist), (TB_Y - SP_2 - SP_3 - CN_HEAD - 1) / CN_ROW)); }
static struct rect cn_rect(void) {
    int h = CN_HEAD + 1 + (nhist ? cn_vis() * CN_ROW : CN_EMPTY);
    return (struct rect){ SW - SP_3 - CN_W, TB_Y - SP_2 - h, CN_W, h };
}
static struct rect cn_clear(void) {
    struct rect m = cn_rect();
    int w = gfx_text_width(T_BODY, "Clear all") + 2 * SP_4;
    return (struct rect){ m.x + m.w - SP_3 - w, m.y + (CN_HEAD - H_CTL) / 2, w, H_CTL };
}
static void cn_damage(void) { dmg(cn_rect(), ELEV_PAD(E4) + 8); }
static void cn_close(void) { if (!cn_open) return; cn_damage(); cn_open = false; wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }

static void draw_center(struct canvas *c) {
    struct rect m = cn_rect();
    int e = enter_e(cn_born, D_NORMAL, m, ELEV_PAD(E4) + 8), off = slide(e, 8);
    m.y += off;
    bool f = e < 256 && fade_begin(c, m, ELEV_PAD(E4));
    gfx_elev(c, m.x, m.y, m.w, m.h, R_3, E4, TH.elev[E4]);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, R_3, TH.overlay);
    gfx_round_rect(c, m.x, m.y, m.w, m.h, R_3, TH.line);
    gfx_text(c, T_HEADING, m.x + SP_5, m.y + (CN_HEAD - T_HEADING->height) / 2, "Notifications", TH.text);
    if (nhist) {
        struct rect b = cn_clear();
        b.y += off;
        if (cn_clr_hot) gfx_round_fill(c, b.x, b.y, b.w, b.h, R_2, TH.hover_ov);
        gfx_text(c, T_BODY, b.x + SP_4, b.y + (H_CTL - T_BODY->height) / 2, "Clear all", TH.text);
    }
    gfx_hline(c, m.x, m.y + CN_HEAD, m.w, TH.line);
    int y0 = m.y + CN_HEAD + 1;
    if (!nhist) {
        icon(c, ICON_BELL_OFF, ICON_MD, m.x + (m.w - ICON_MD) / 2, y0 + SP_7, TH.text3);
        gfx_text_center(c, T_HEADING, m.x, y0 + SP_7 + ICON_MD + SP_4, m.w, "No notifications", TH.text);
        gfx_text_center(c, T_BODY, m.x, y0 + SP_7 + ICON_MD + SP_4 + T_HEADING->height + SP_2, m.w, "You\xe2\x80\x99re all caught up.", TH.text2);
    }
    for (int i = 0; i < cn_vis() && cn_off + i < nhist; i++) {
        int y = y0 + i * CN_ROW;
        if (i) gfx_hline(c, m.x + SP_5, y, m.w - 2 * SP_5, TH.line);
        if (cn_hov == i) gfx_round_fill(c, m.x + SP_2, y + SP_2, m.w - 2 * SP_2, CN_ROW - 2 * SP_2, R_2, TH.hover_ov);
        draw_notif(c, &hist[cn_off + i], m.x + SP_5, y + SP_4, m.w - 2 * SP_5, 2, cn_hov == i);
    }
    int vis = cn_vis();
    if (nhist > vis) {                                         /* overlay scrollbar */
        int th = imax(SCROLL_MIN, vis * CN_ROW * vis / nhist), ty = y0 + (vis * CN_ROW - th) * cn_off / (nhist - vis);
        gfx_round_fill(c, m.x + m.w - SP_1 - SCROLL_W, ty, SCROLL_W, th, R_F(SCROLL_W), TH_A(TH.text, 89));
    }
    if (f) fade_end(e);
}

/* ---- quick settings (volume, network), a panel popover over the tray ---- */
#define QS_W 320
static bool qs_drag, qs_hot;                            /* dragging the volume thumb; pointer over the mute button */
static uint32_t qs_born;
static struct rect qs_rect(void) {
    panel_layout();
    int h = SP_3 + (vol_cache >= 0 ? H_ROW2 + 1 : 0) + H_ROW2 + SP_3;
    return (struct rect){ imax(SP_3, imin(SW - SP_3, clock_x) - QS_W), TB_Y - SP_2 - h, QS_W, h };
}
#define QS_SX0(m) ((m).x + SP_4 + H_CTL + SP_4)
#define QS_SX1(m) ((m).x + (m).w - SP_5 - 40 - SP_3)
static void qs_damage(void) { dmg(qs_rect(), ELEV_PAD(E2) + 8); }
static void qs_close(void) { if (!qs_open) return; qs_damage(); qs_open = false; qs_drag = false; wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }

static void draw_qs(struct canvas *c) {
    struct rect m = qs_rect();
    int e = enter_e(qs_born, D_NORMAL, m, ELEV_PAD(E2) + 8);
    m.y += slide(e, 4);
    bool f = e < 256 && fade_begin(c, m, ELEV_PAD(E2));
    gfx_elev(c, m.x, m.y, m.w, m.h, R_3, E2, TH.elev[E2]);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, R_3, TH.overlay);
    gfx_round_rect(c, m.x, m.y, m.w, m.h, R_3, TH.line);
    int y = m.y + SP_3;
    if (vol_cache >= 0) {
        int bx = m.x + SP_4, by = y + (H_ROW2 - H_CTL) / 2;
        if (qs_hot) gfx_round_fill(c, bx, by, H_CTL, H_CTL, R_2, TH.hover_ov);
        icon(c, vol_icon(), ICON_SM, bx + (H_CTL - ICON_SM) / 2, by + (H_CTL - ICON_SM) / 2, qs_hot ? TH.text : TH.text2);
        int x0 = QS_SX0(m), x1 = QS_SX1(m), cy = y + H_ROW2 / 2, kx = x0 + (x1 - x0) * vol_cache / 100;
        gfx_round_fill(c, x0, cy - SLIDER_TRACK / 2, x1 - x0, SLIDER_TRACK, R_F(SLIDER_TRACK), TH.control);
        if (kx > x0) {
            gfx_round_fill(c, x0, cy - SLIDER_TRACK / 2, kx - x0, SLIDER_TRACK, R_F(SLIDER_TRACK), TH.accent);
            if (!TH.dark) gfx_round_rect(c, x0, cy - SLIDER_TRACK / 2, kx - x0, SLIDER_TRACK, R_F(SLIDER_TRACK), TH.accent_line);
        }
        int ks = qs_drag ? SLIDER_THUMB_HOT : SLIDER_THUMB;
        gfx_elev(c, kx - ks / 2, cy - ks / 2, ks, ks, R_F(ks), E1, TH.elev[E1]);
        gfx_round_fill(c, kx - ks / 2, cy - ks / 2, ks, ks, R_F(ks), RGB(255, 255, 255));
        gfx_round_rect(c, kx - ks / 2, cy - ks / 2, ks, ks, R_F(ks), TH.line_strong);
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", vol_cache);
        gfx_text(c, T_BODY, m.x + m.w - SP_5 - gfx_text_width(T_BODY, pct), cy - T_BODY->height / 2, pct, TH.text);
        y += H_ROW2;
        gfx_hline(c, m.x + SP_4, y, m.w - 2 * SP_4, TH.line);
        y += 1;
    }
    icon(c, ICON_NETWORK, ICON_SM, m.x + SP_4 + (H_CTL - ICON_SM) / 2, y + (H_ROW2 - ICON_SM) / 2, net_up ? TH.text : TH.text2);
    int ty = y + (H_ROW2 - T_BODY->height - T_CAPTION->height) / 2, tx = QS_SX0(m);
    gfx_text(c, T_BODY, tx, ty, net_present ? "Wired connection" : "No network adapter", TH.text);
    char l[48] = "";
    if (net_up) snprintf(l, sizeof l, "Connected \xc2\xb7 %u.%u.%u.%u", net_ip & 255, net_ip >> 8 & 255, net_ip >> 16 & 255, net_ip >> 24);
    else if (net_present) strlcpy(l, "Not connected", sizeof l);
    gfx_text(c, T_CAPTION, tx, ty + T_BODY->height, l, TH.text2);
    if (f) fade_end(e);
}

/* ---- Alt+Tab switcher: live thumbnails, most recently used first ---- */
#define SW_CW 196
#define SW_CH 152
#define SW_TW 176
#define SW_TH 108
#define SW_COLS 6
static struct window *sw_list[16];
static int sw_n, sw_sel;
static uint32_t sw_born;
static struct rect sw_rect(void) {
    int cols = imin(sw_n, SW_COLS), rows = (sw_n + SW_COLS - 1) / SW_COLS, w = cols * SW_CW + 2 * SP_4, h = rows * SW_CH + 2 * SP_4;
    return (struct rect){ (SW - w) / 2, (TB_Y - h) / 2, w, h };
}
static struct rect sw_cell(int i) {
    struct rect r = sw_rect();
    return (struct rect){ r.x + SP_4 + i % SW_COLS * SW_CW, r.y + SP_4 + i / SW_COLS * SW_CH, SW_CW, SW_CH };
}
static void sw_damage(void) { dmg(sw_rect(), ELEV_PAD(E4)); }

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
    gfx_rect(c, x, y, dw, dh, TH.line);
}

static void draw_switcher(struct canvas *c) {
    struct rect m = sw_rect();
    int e = enter_e(sw_born, D_NORMAL, m, ELEV_PAD(E4));
    bool f = e < 256 && fade_begin(c, m, ELEV_PAD(E4));
    gfx_elev(c, m.x, m.y, m.w, m.h, R_3, E4, TH.elev[E4]);
    gfx_round_fill(c, m.x, m.y, m.w, m.h, R_3, TH.overlay);
    gfx_round_rect(c, m.x, m.y, m.w, m.h, R_3, TH.line);
    for (int i = 0; i < sw_n; i++) {
        struct rect q = sw_cell(i);
        struct window *w = sw_list[i];
        bool sel = i == sw_sel;
        if (sel) gfx_round_fill(c, q.x + SP_1, q.y + SP_1, q.w - 2 * SP_1, q.h - 2 * SP_1, R_2, TH.selection);
        draw_thumb(c, q.x + 10, q.y + 10, SW_TW, SW_TH, w);
        int ly = q.y + 10 + SW_TH + (q.h - 10 - SW_TH - T_BODY->height) / 2;
        icon(c, w->icon, ICON_SM, q.x + 10, ly + (T_BODY->height - ICON_SM) / 2, sel ? TH.accent_text : TH.text2);
        gfx_text_ell(c, sel ? T_BODY_B : T_BODY, q.x + 10 + ICON_SM + SP_3, ly, SW_TW - ICON_SM - SP_3, w->title, sel ? TH.text : TH.text2);
    }
    if (f) fade_end(e);
}

/* ================= compose ================= */

static bool no_present;

/* returns true while something is animating */
static bool compose(void) {
    uint32_t f = irq_save();
    struct rect d = dirty;
    dirty.w = 0;
    irq_restore(f);
    if (!d.w) return false;
    animating = false;
    struct canvas c = bb;
    canvas_clip(&c, d.x, d.y, d.w, d.h);
    for (int y = d.y; y < d.y + d.h; y++)
        memcpy(&bb.px[y * SW + d.x], &wall.px[y * SW + d.x], d.w * 4);
    for (struct window *w = wins; w; w = w->next) {
        if (!shown(w)) continue;
        if (w == drag_win && snap_zone) draw_snap(&c);   /* under the window being dragged */
        if (!(w->flags & WIN_FLAG_FRAMELESS) && ticks_ms - w->born < (uint32_t)(theme_anim ? D_SLOW : D_REDUCED)) {
            struct rect r;
            frame_rect(w, &r);
            draw_window_open(&c, w, enter_e(w->born, D_SLOW, r, SH_PAD));
        } else draw_window(&c, w);
    }
    draw_ghost(&c);
    if (d.y + d.h > TB_Y) draw_taskbar(&c);
    if (tip_on) draw_tip(&c);
    if (menu_open) draw_launcher(&c);
    if (mkind) draw_mmenu(&c);
    if (qs_open) draw_qs(&c);
    if (cn_open) draw_center(&c);
    draw_toasts(&c);
    if (sw_open) draw_switcher(&c);
    draw_cursor(&c);
    if (!no_present)
        for (int y = d.y; y < d.y + d.h; y++)
            memcpy(&fb.px[y * fb.stride + d.x], &bb.px[y * SW + d.x], d.w * 4);
    return animating;
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
    if (w->flags & WIN_FLAG_DESKTOP) { w->next = wins; wins = w; return; }    /* the desktop layer stays at the bottom */
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
    if (w && w->minimized) { w->minimized = false; w->born = ticks_ms; }
    if (focused == w) { if (w) raise(w), damage_window(w), wm_damage(0, TB_Y, SW, WM_TASKBAR_H); return; }
    struct window *old = focused;
    focused = w;
    if (old) { struct gui_event e = { .type = EV_FOCUS, .pressed = 0 }; send(old, &e); damage_window(old); }
    if (w) {
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
    if (flags & WIN_FLAG_DESKTOP) { flags |= WIN_FLAG_FRAMELESS | WIN_FLAG_NORESIZE; x = y = 0; w = SW; h = TB_Y; }
    else {
        w = imax(80, imin(w, SW - 20));
        h = imax(40, imin(h, TB_Y - WM_TITLE_H - 10));
    }
    struct window *win = kzalloc(sizeof *win);
    if (!win) return 0;
    /* ponytail: every window gets a full-screen-sized buffer (stride = SW) so resizing
     * never reallocates. ~4 MiB per window at 1280x800; fine for a dozen windows. A display
     * mode change only ever grows it (wm_set_mode). */
    win->buf_frames = (uint32_t)SW * SH * 4 / PAGE_SIZE + 1;
    uint32_t pa = pmm_alloc_contig(win->buf_frames);
    if (!pa) { kfree(win); return 0; }
    win->buf = (uint32_t *)pa;
    memset32(win->buf, flags & WIN_FLAG_DESKTOP ? 0 : TH.term.bg, (size_t)SW * h);
    win->id = next_id++;
    win->ws = flags & WIN_FLAG_FRAMELESS ? -1 : cur_ws;
    win->born = ticks_ms;
    win->w = w; win->h = h; win->flags = flags;
    int app = proc_app();
    win->icon = app_sym(app >= 0 ? apps[app].icon : -1);
    if (x < 0 || y < 0) {          /* least-overlap placement on a 24 px grid, cascade on ties */
        static int slot;
        int x0 = w + 136 <= SW ? 120 : 16;                   /* keep the desktop icon column clear when there's room */
        int bx = x0 + (slot % 8) * 36, by = 50 + WM_TITLE_H + (slot % 8) * 30;
        int64_t best = -1;
        slot++;
        for (int cy = 16 + WM_TITLE_H; cy + h + 16 <= TB_Y; cy += 24)
            for (int cx = x0; cx + w + 16 <= SW; cx += 24) {
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
    ghost_make(w);
    damage_window(w);
    unlink_win(w);
    if (w->on_destroy) w->on_destroy(w);
    if (drag_win == w) drag_win = 0;
    if (resize_win == w) resize_win = 0;
    if (capture_win == w) capture_win = 0;
    if (hover_win == w) hover_win = 0;
    if (last_in == w) last_in = 0;
    if (last_click_win == w) last_click_win = 0;
    if (mwin == w) m_close();
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
        set_geometry(w, 0, WM_TITLE_H, SW, TB_Y - WM_TITLE_H);
    }
}

/* client rect of a snap zone */
static struct rect snap_rect(int z) {
    int h = TB_Y - WM_TITLE_H, half = SW / 2;
    if (z == 3) return (struct rect){ 0, WM_TITLE_H, SW, h };
    return (struct rect){ z == 1 ? 0 : half, WM_TITLE_H, z == 1 ? half : SW - half, h };
}

static void damage_snap(int z) {
    if (!z) return;
    struct rect r = snap_rect(z);
    wm_damage(r.x, r.y - WM_TITLE_H, r.w, r.h + WM_TITLE_H);
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

/* where the window will land: a quiet translucent plate, inset 8 */
static void draw_snap(struct canvas *c) {
    struct rect r = snap_rect(snap_zone);
    int x = r.x + SP_3, y = r.y - WM_TITLE_H + SP_3, w = r.w - 2 * SP_3, h = r.h + WM_TITLE_H - 2 * SP_3;
    gfx_round_fill(c, x, y, w, h, R_3, TH_A(TH.text, 26));
    gfx_round_rect(c, x, y, w, h, R_3, TH_A(TH.text, 90));
}

static void close_window(struct window *w) {
    struct gui_event e = { .type = EV_CLOSE };
    send(w, &e);
}


/* ================= apps ================= */

static void load_apps(void) {
    napps = 0;
    apps[napps++] = (struct app){ "Terminal", "", "", "Run commands in a shell", APPICON_TERMINAL };
    void *data; uint32_t size;
    if (vfs_read_all("/etc/apps", &data, &size) < 0) return;
    char *s = data;
    while (*s && napps < (int)ARRAY_LEN(apps)) {
        char *line = s;
        while (*s && *s != '\n') s++;
        if (*s) *s++ = 0;
        if (*line == '#' || !*line) continue;
        /* name|path|arg|icon|description (icon: an APPICON key, see common/icons.h); the old name|path|arg|RRGGBB|glyph
         * still parses (no description then) */
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
        int hex = n > 3 && strlen(fld[3]) == 6;
        for (char *p = n > 3 ? fld[3] : ""; hex && *p; p++) hex = (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f');
        strlcpy(a->desc, n > 4 && !hex ? fld[4] : "", sizeof a->desc);
        a->icon = n > 3 && !hex ? appicon_find(fld[3]) : -1;
        if (a->icon < 0) a->icon = appicon_find(a->name);
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

static void power(bool reboot) {
    extern int ext2_sync_all(void);
    ext2_sync_all();
    ata_flush_all();
    cli();
    if (reboot) { while (inb(0x64) & 2) {} outb(0x64, 0xFE); }
    outw(0x604, 0x2000); outw(0xB004, 0x2000); outb(0xF4, 0);
    for (;;) hlt();
}

static void l_damage(void) { dmg(l_rect(), ELEV_PAD(E4) + 8); wm_damage(0, TB_Y, H_PANEL, WM_TASKBAR_H); }
static void l_close(void) { if (!menu_open) return; l_damage(); menu_open = false; }

static void launch(int app) {
    l_close();
    if (app < 0) return;
    if (!apps[app].path[0]) termwin_open(0);
    else wm_spawn_app(apps[app].path, apps[app].arg);
}

static int taskbar_at(int x, int y);
static void close_popups(void);
static void m_close(void);
static void tip_reset(int t);

static void l_toggle(void) {
    if (menu_open) { l_close(); return; }
    close_popups();
    load_apps();
    lq[0] = 0; lkbd = false; lhov = -1;
    l_filter();
    menu_open = true;
    l_born = l_typed = ticks_ms;
    l_damage();
}

static void l_show_sel(void) {
    if (lsel < 0) return;
    int row = lsel % l_hrows();
    if (row < ltop) ltop = row;
    if (row >= ltop + l_rows()) ltop = row - l_rows() + 1;
}

/* the launcher owns the keyboard while it's open (shortcuts with Ctrl/Alt/Super still pass) */
static bool l_key(const struct gui_event *ev) {
    if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) return false;
    int len = strlen(lq), k = ev->key;
    if (k == KEY_ESC) { l_close(); return true; }
    if (k == KEY_ENTER) { if (nhit) launch(lhit[lsel >= 0 ? lsel : 0]); return true; }
    if (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT) {
        if (nhit) {
            int d = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_LEFT ? -l_hrows() : l_hrows();
            if (lsel < 0) lsel = 0;
            else if (lsel + d >= 0 && lsel + d < nhit) lsel += d;
            lkbd = true;
            l_show_sel();
        }
    } else if (k == KEY_BACKSPACE) {
        if (!len) return true;
        lq[len - 1] = 0;
        l_filter();
    } else if (ev->ch >= 32 && ev->ch < 127) {
        if (len >= (int)sizeof lq - 1) return true;
        lq[len] = (char)ev->ch; lq[len + 1] = 0;
        l_filter();
    } else return true;
    l_typed = ticks_ms;
    l_damage();
    return true;
}

/* pointer over the launcher: hover, click, wheel; false if it's elsewhere */
static bool l_mouse(int x, int y, bool down, int dz) {
    struct rect m = l_rect();
    int hov = -1, item = -1;
    if (inside(m, x, y)) {
        for (int i = 0; i < 2; i++) if (inside(l_btn(i), x, y)) hov = i;
        for (int i = 0; i < nhit; i++) { struct rect q; if (l_item(i, &q) && inside(q, x, y)) item = i; }
    }
    int sel = item >= 0 ? item : lkbd || lq[0] ? lsel : -1;
    if (hov != lhov || sel != lsel || (item >= 0 && lkbd)) {
        lhov = hov; lsel = sel;
        if (item >= 0) lkbd = false;
        l_damage();
    }
    if (!inside(m, x, y)) return false;
    if (dz && l_hrows() > l_rows()) { ltop = imax(0, imin(l_hrows() - l_rows(), ltop + (dz > 0 ? 1 : -1))); l_damage(); }
    if (down && item >= 0) launch(lhit[item]);
    else if (down && hov >= 0) power(hov == 0);
    return true;
}

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
    l_close(); m_close(); qs_close(); cn_close(); sw_close(false);
    tip_reset(T_NONE);
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
    if (ws_old) {
        no_present = true; compose(); no_present = false;
        if (theme_anim) crossfade(ws_old, 43, 20);       /* ~150 ms */
        else crossfade(ws_old, 86, 30);                  /* reduced: a short fade */
    }
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

static void minimize(struct window *w) {
    ghost_make(w);
    w->minimized = true;
    damage_window(w);
    focus_top();
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
    h->kind = !strcmp(kind, "good") ? 1 : !strcmp(kind, "warn") ? 2 : !strcmp(kind, "bad") ? 3 : 0;
    int a = proc_app();                                  /* who sent it: an /etc/apps program, else the system */
    strlcpy(h->app, a >= 0 ? apps[a].name : "VibeOS", sizeof h->app);
    h->sym = a >= 0 ? app_sym(apps[a].icon) : ICON_BELL;
    h->born = ticks_ms; h->until = ticks_ms + D_NOTIF; h->wall = rtc_now();
    h->toast = !cn_open;
    int k = 0;
    for (int i = 0; i < nhist; i++) if (hist[i].toast && ++k > NVIS) hist[i].toast = false;
    if (!cn_open) unseen++;
    if (running) { toast_damage(); if (cn_open) cn_damage(); wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }
}

static void cn_toggle(void) {
    if (cn_open) { cn_close(); return; }
    close_popups();
    toast_damage();
    for (int i = 0; i < nhist; i++) hist[i].toast = false;
    unseen = 0; cn_off = 0; cn_hov = -1; cn_clr_hot = 0; cn_open = true;
    cn_born = ticks_ms;
    cn_damage();
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static void notif_remove(int i) {
    memmove(hist + i, hist + i + 1, (NQ - 1 - i) * sizeof hist[0]);
    nhist--;
    cn_off = imax(0, imin(cn_off, nhist - cn_vis()));
}

/* returns true when the click/scroll belonged to the centre */
static bool cn_mouse(int x, int y, bool down, int dz) {
    struct rect m = cn_rect();
    int hov = -1, clr = 0;
    if (inside(m, x, y) && y >= m.y + CN_HEAD + 1 && nhist) hov = imin(cn_vis() - 1, (y - m.y - CN_HEAD - 1) / CN_ROW);
    if (hov >= 0 && cn_off + hov >= nhist) hov = -1;
    if (nhist && inside(cn_clear(), x, y)) clr = 1;
    if (hov != cn_hov || clr != cn_clr_hot) { cn_hov = hov; cn_clr_hot = clr; cn_damage(); }
    if (!inside(m, x, y)) {
        if (!down) return false;
        int t = taskbar_at(x, y);                       /* clicking the clock or the bell again just closes */
        cn_close();
        return t == T_CLOCK || t == T_BELL;
    }
    if (dz && nhist > cn_vis()) { cn_off = imax(0, imin(nhist - cn_vis(), cn_off + (dz > 0 ? 1 : -1))); cn_damage(); }
    if (!down) return true;
    if (clr) { cn_damage(); nhist = 0; cn_off = 0; cn_hov = -1; cn_clr_hot = 0; cn_damage(); }
    else if (hov >= 0) { cn_damage(); notif_remove(cn_off + hov); cn_hov = -1; cn_damage(); }
    return true;
}

static bool toasts_mouse(int x, int y, bool down) {
    struct rect r[NVIS]; struct notif *n[NVIS];
    int k = toast_layout(r, n), hov = -1;
    for (int i = 0; i < k; i++) if (inside(r[i], x, y)) hov = i;
    if (hov != toast_hov) {
        if (hov < 0) for (int i = 0; i < nhist; i++) { if (hist[i].until < (uint32_t)ticks_ms + 2000) hist[i].until = ticks_ms + 2000; }   /* they waited while hovered */
        toast_hov = hov;
        toast_damage();
    }
    if (!down || hov < 0) return false;
    toast_damage();
    n[hov]->toast = false;                               /* a click anywhere on it (or its close box) dismisses it */
    toast_hov = -1;
    return true;
}

/* ---- quick settings ---- */
static void set_volume(int v) {
    vol_cache = ac97_volume(imax(0, imin(100, v)));
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
    if (qs_open) qs_damage();
}

static void qs_toggle(void) {
    if (qs_open) { qs_close(); return; }
    close_popups();
    qs_open = true; qs_hot = false;
    qs_born = ticks_ms;
    qs_damage();
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static void qs_slide(int x) { struct rect m = qs_rect(); set_volume((x - QS_SX0(m)) * 100 / imax(1, QS_SX1(m) - QS_SX0(m))); }

static bool qs_mouse(int x, int y, bool down, int dz) {
    struct rect m = qs_rect();
    int vy = m.y + SP_3;
    bool hot = vol_cache >= 0 && x >= m.x + SP_4 && x < m.x + SP_4 + H_CTL && y >= vy + (H_ROW2 - H_CTL) / 2 && y < vy + (H_ROW2 + H_CTL) / 2;
    if (hot != qs_hot) { qs_hot = hot; qs_damage(); }
    if (!inside(m, x, y)) {
        if (!down) return false;
        int t = taskbar_at(x, y);
        qs_close();
        return t == T_VOL || t == T_NET;
    }
    if (vol_cache < 0) return true;
    if (dz) set_volume(vol_cache - (dz > 0 ? 5 : -5));
    if (down && y >= vy && y < vy + H_ROW2) {
        static int saved = 60;
        if (hot) { if (vol_cache) saved = vol_cache; set_volume(vol_cache ? 0 : saved); }
        else if (x >= QS_SX0(m) - SLIDER_THUMB / 2 && x < QS_SX1(m) + SLIDER_THUMB / 2) { qs_drag = true; qs_slide(x); }
    }
    return true;
}

/* ---- menus ---- */
enum { A_TERM = 1, A_FILES, A_SYSMON, A_WALL, A_SETTINGS, A_ABOUT, A_MIN, A_MAX, A_TOP, A_CLOSE, A_WS = 20, A_TASK = 40 };
static int mids[16];                                     /* MK_TASKS: the window id behind each item */

static void m_close(void) {
    if (!mkind) return;
    dmg(mr, ELEV_PAD(E2) + SP_2);
    if (mkind == MK_TASKS) wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
    mkind = 0; mwin = 0; msel = -1;
}

static void m_add(const char *label, int ic, int act, const char *hint, bool sep, bool danger) {
    if (mn >= (int)ARRAY_LEN(mi)) return;
    struct mitem *m = &mi[mn++];
    strlcpy(m->label, label, sizeof m->label);
    strlcpy(m->hint, hint, sizeof m->hint);
    m->icon = ic; m->act = act; m->sep = sep && mn > 1; m->danger = danger;
}

/* lay the items out at x,y (above: grow up from y), flipping up/left where there's no room */
static void m_show(int kind, int x, int y, bool above) {
    bool ic = m_icons();
    int w = W_MENU_MIN, h = 2 * SP_2;
    for (int i = 0; i < mn; i++) {
        int iw = 2 * SP_4 + (ic ? ICON_SM + SP_3 : 0) + gfx_text_width(T_BODY, mi[i].label);
        if (mi[i].hint[0]) iw += SP_5 + gfx_text_width(T_BODY, mi[i].hint);
        w = imax(w, iw);
        h += m_ih(i);
    }
    w = imin(W_MENU_MAX, w);
    m_up = above || y + h > TB_Y - SP_2;
    if (x + w > SW - SP_2) x = imax(SP_2, x - w);
    if (m_up) y = imax(SP_2, y - h);
    mr = (struct rect){ x, y, w, h };
    mkind = kind; msel = -1; mkbd = false; m_born = ticks_ms;
    dmg(mr, ELEV_PAD(E2) + SP_2);
}

static void desk_menu(int x, int y) {
    close_popups();
    mn = 0;
    m_add("New terminal", ICON_TERMINAL, A_TERM, "Ctrl+Alt+T", false, false);
    m_add("Files", ICON_FOLDER, A_FILES, "", false, false);
    m_add("System Monitor", ICON_ACTIVITY, A_SYSMON, "", false, false);
    m_add("Change wallpaper", ICON_WALLPAPER, A_WALL, wallpaper_name(wall_theme), true, false);
    m_add("Settings", ICON_SETTINGS, A_SETTINGS, "", false, false);
    m_add("About VibeOS", ICON_INFO, A_ABOUT, "", false, false);
    m_show(MK_DESK, x, y, false);
}

static void win_menu(struct window *w, int x, int y) {
    close_popups();
    mn = 0;
    m_add("Minimise", ICON_MINIMIZE, A_MIN, "", false, false);
    if (!(w->flags & WIN_FLAG_NORESIZE)) m_add(w->maximized ? "Restore" : "Maximise", w->maximized ? ICON_RESTORE : ICON_MAXIMIZE, A_MAX, "F11", false, false);
    m_add("Always on top", w->topmost ? ICON_CHECK : -1, A_TOP, "", false, false);
    for (int k = 0, first = 1; k < WM_WS; k++) {
        if (k == w->ws) continue;
        char l[32], h[16];
        snprintf(l, sizeof l, "Move to workspace %d", k + 1);
        snprintf(h, sizeof h, "Super+Shift+%d", k + 1);
        m_add(l, -1, A_WS + k, h, first, false);
        first = 0;
    }
    m_add("Close", ICON_CLOSE, A_CLOSE, "Alt+F4", true, true);
    m_show(MK_WIN, x, y, false);
    mwin = w;
}

static void tasks_menu(void) {
    close_popups();
    panel_layout();
    mn = 0;
    for (int i = nshow; i < ntasks && mn < (int)ARRAY_LEN(mi); i++) { mids[mn] = tasks[i]->id; m_add(tasks[i]->title, tasks[i]->icon, A_TASK, "", false, false); }
    m_show(MK_TASKS, task_x(nshow), TB_Y - SP_2, true);
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
}

static bool alive(struct window *w) { for (struct window *o = wins; o; o = o->next) if (o == w) return true; return false; }

static void m_act(int i) {
    int a = mi[i].act, id = mids[i];
    struct window *w = mwin;
    m_close();
    if (a == A_TERM) termwin_open(0);
    else if (a == A_FILES) wm_spawn_app("/bin/files", 0);
    else if (a == A_SYSMON) wm_spawn_app("/bin/sysmon", 0);
    else if (a == A_WALL) set_wallpaper(wall_theme + 1);
    else if (a == A_SETTINGS) wm_spawn_app("/bin/settings", 0);
    else if (a == A_ABOUT) wm_spawn_app("/bin/about", 0);
    else if (a == A_TASK) { for (struct window *o = wins; o; o = o->next) if (o->id == id) wm_focus(o); }
    else if (!w || !alive(w)) return;
    else if (a == A_MIN) minimize(w);
    else if (a == A_MAX) toggle_max(w);
    else if (a == A_TOP) { w->topmost = !w->topmost; raise(w); damage_window(w); }
    else if (a == A_CLOSE) close_window(w);
    else if (a >= A_WS) move_to_ws(w, a - A_WS);
}

static bool m_mouse(int x, int y, bool click) {
    int i = -1;
    if (inside(mr, x, y)) for (int k = 0; k < mn; k++) if (y >= m_iy(k) && y < m_iy(k) + H_ROW) i = k;
    if (i != msel && (i >= 0 || !mkbd)) { msel = i; mkbd = false; dmg(mr, 0); }
    if (inside(mr, x, y)) {
        if (click && i >= 0) m_act(i);
        return true;
    }
    if (!click) return false;
    bool more = mkind == MK_TASKS && taskbar_at(x, y) == T_MORE;    /* the "+N" button again just closes */
    m_close();
    return more;
}

/* menus own the keyboard: arrows move, Enter picks, Esc closes */
static void m_key(const struct gui_event *ev) {
    if (ev->key == KEY_ESC) { m_close(); return; }
    if (ev->key == KEY_ENTER) { if (msel >= 0) m_act(msel); return; }
    if (ev->key != KEY_UP && ev->key != KEY_DOWN) return;
    int d = ev->key == KEY_UP ? mn - 1 : 1;
    msel = msel < 0 ? (d == 1 ? 0 : mn - 1) : (msel + d) % mn;
    mkbd = true;
    dmg(mr, 0);
}

/* ---- Alt+Tab ---- */
static void sw_start(bool back) {
    struct window *tmp[32];
    int n = 0;
    for (struct window *w = wins; w && n < 32; w = w->next)
        if (w->visible && onws(w) && !(w->flags & WIN_FLAG_FRAMELESS)) tmp[n++] = w;
    if (n == 1 && focused != tmp[0]) wm_focus(tmp[0]);
    if (n < 2) return;
    close_popups();
    sw_n = imin(n, 16);
    for (int i = 0; i < sw_n; i++) sw_list[i] = tmp[n - 1 - i];     /* most recently used first */
    sw_sel = back ? sw_n - 1 : focused == sw_list[0] ? 1 : 0;
    sw_open = true;
    sw_born = ticks_ms;
    sw_damage();
}

static void sw_step(int d) { sw_sel = (sw_sel + d + sw_n) % sw_n; sw_damage(); }

static void sw_close(bool commit) {
    if (!sw_open) return;
    sw_damage();
    sw_open = false;
    struct window *w = sw_list[sw_sel];
    if (commit && w) wm_focus(w);
}

static void sw_mouse(int x, int y, bool down) {
    for (int i = 0; i < sw_n; i++) {
        if (inside(sw_cell(i), x, y)) {
            if (i != sw_sel) { sw_sel = i; sw_damage(); }
            if (down) sw_close(true);
            return;
        }
    }
    if (down) sw_close(false);
}

/* ================= input ================= */

/* the top shown window whose frame holds x,y, or whose 6px resize border does (resizable, not maximised: edges = E_*) */
static struct window *window_at(int x, int y, bool *in_frame, int *edges) {
    struct window *hit = 0;
    *in_frame = false; *edges = 0;
    for (struct window *w = wins; w; w = w->next) {
        if (!shown(w)) continue;
        struct rect r;
        frame_rect(w, &r);
        if (inside(r, x, y)) { hit = w; *edges = 0; *in_frame = !(x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h); continue; }
        if ((w->flags & (WIN_FLAG_FRAMELESS | WIN_FLAG_NORESIZE)) || w->maximized) continue;
        struct rect o = { r.x - RESIZE_BORDER, r.y - RESIZE_BORDER, r.w + 2 * RESIZE_BORDER, r.h + 2 * RESIZE_BORDER };
        if (!inside(o, x, y)) continue;
        int e = (x < r.x ? E_L : x >= r.x + r.w ? E_R : 0) | (y < r.y ? E_T : y >= r.y + r.h ? E_B : 0);
        if (!(e & (E_T | E_B))) e |= y < r.y + 2 * SP_4 ? E_T : y >= r.y + r.h - 2 * SP_4 ? E_B : 0;   /* generous corners */
        if (!(e & (E_L | E_R))) e |= x < r.x + 2 * SP_4 ? E_L : x >= r.x + r.w - 2 * SP_4 ? E_R : 0;
        hit = w; *in_frame = true; *edges = e;
    }
    return hit;
}

static int title_button_at(struct window *w, int x, int y) {
    if (w->flags & WIN_FLAG_FRAMELESS) return -1;
    if (y < CTL_Y(w) || y >= CTL_Y(w) + H_WINCTL) return -1;
    for (int i = 0; i < 3; i++) if (x >= ctl_x(w, i) && x < ctl_x(w, i) + W_WINCTL) return i == 1 && (w->flags & WIN_FLAG_NORESIZE) ? -1 : i;
    return -1;
}

static int taskbar_at(int x, int y) {
    if (y < TB_Y || y >= SH) return T_NONE;
    panel_layout();
    if (x < H_PANEL) return T_LAUNCH;
    for (int k = 0; k < WM_WS; k++) { int a, b; ws_slot(k, &a, &b); if (x >= a && x < b) return T_WS0 - k; }
    if (x >= clock_x) return T_CLOCK;
    if (x >= tray_x) return cells[(x - tray_x) / TRAY_CELL];
    for (int i = 0; i < nshow; i++) if (x >= task_x(i) && x < task_x(i) + task_w) return i;
    if (nshow < ntasks && x >= task_x(nshow) && x < task_x(nshow) + MORE_W) return T_MORE;
    return T_GAP;
}

/* tooltips: a new target restarts the wait; a press suppresses it until the pointer moves to another one */
static bool tip_block;
static void tip_reset(int t) { tip_hide(); tip_for = t; tip_since = ticks_ms; tip_block = false; }
static bool tip_due(void) {
    return !tip_on && !tip_block && tip_for != T_NONE && tip_for != T_GAP && ticks_ms - tip_since >= D_TOOLTIP
        && !menu_open && !mkind && !qs_open && !cn_open && !sw_open && !drag_win && !resize_win;
}

static void set_hover(struct window *hw, int btn, int task) {
    if (hw != hover_win || btn != hover_btn) {
        if (hover_win && hover_btn >= 0) damage_title(hover_win);
        if (hw && btn >= 0) damage_title(hw);
    }
    if (task != hover_task) { wm_damage(0, TB_Y, SW, WM_TASKBAR_H); tip_reset(task); }
    hover_win = hw; hover_btn = btn; hover_task = task;
}

static void set_shape(int s) { if (s != cur_shape) { cur_shape = s; cursor_damage(cur_x, cur_y); } }

/* a drag ended (DRAG_FILE exists): tell the window under the pointer, which reads the payload */
static void drop_event(int x, int y) {
    struct vnode *v;
    if (vfs_lookup("/", DRAG_FILE, &v) < 0) return;
    vnode_put(v);
    bool fr; int ed;
    struct window *t = window_at(x, y, &fr, &ed);
    if (!t || fr) return;
    struct gui_event e = { .type = EV_DROP, .x = x - t->x, .y = y - t->y };
    send(t, &e);
}

static void mouse_event(int x, int y, int b, int dz) {
    int ox = cur_x, oy = cur_y, ob = cur_b;
    cur_x = x; cur_y = y; cur_b = b;
    if (ox != x || oy != y) { cursor_damage(ox, oy); cursor_damage(x, y); }
    bool down = (b & 1) && !(ob & 1), up = !(b & 1) && (ob & 1), rdown = (b & 2) && !(ob & 2);

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
        struct window *w = resize_win;
        if (up) { resize_win = 0; set_shape(0); return; }
        if (ox == x && oy == y) return;
        struct rect g = rs_from;
        int dx = x - drag_sx, dy = y - drag_sy;
        if (rs_edges & E_L) { int nw = imax(160, imin(g.w - dx, g.x + g.w)); g.x += g.w - nw; g.w = nw; }
        if (rs_edges & E_R) g.w = imax(160, imin(g.w + dx, SW - g.x));
        if (rs_edges & E_T) { int nh = imax(80, imin(g.h - dy, g.y + g.h - WM_TITLE_H)); g.y += g.h - nh; g.h = nh; }
        if (rs_edges & E_B) g.h = imax(80, imin(g.h + dy, TB_Y - g.y));
        set_geometry(w, g.x, g.y, g.w, g.h);
        return;
    }
    if (capture_win) {                                       /* button held inside a client */
        struct window *w = capture_win;
        struct gui_event e = { .type = up ? EV_MOUSE_UP : EV_MOUSE_MOVE, .x = x - w->x, .y = y - w->y, .button = up ? 1 : b };
        send(w, &e);
        if (up) { capture_win = 0; drop_event(x, y); }
        return;
    }

    if (qs_drag) {                                           /* dragging the volume slider */
        if (!(b & 1)) { qs_drag = false; qs_damage(); }
        else qs_slide(x);
        return;
    }
    if (sw_open) { sw_mouse(x, y, down); return; }
    if (down || rdown) tip_hide(), tip_block = true;
    if (mkind && m_mouse(x, y, down || rdown)) return;
    if (cn_open && cn_mouse(x, y, down, dz)) return;
    if (qs_open && qs_mouse(x, y, down, dz)) return;
    if (toasts_mouse(x, y, down)) return;
    if (menu_open && l_mouse(x, y, down, dz)) return;

    bool in_frame = false;
    int edges = 0;
    struct window *w = window_at(x, y, &in_frame, &edges);
    struct window *desk = w && (w->flags & WIN_FLAG_DESKTOP) ? w : 0;
    if (desk && rdown && !(desk->buf[(y - desk->y) * desk->canvas.stride + x - desk->x] >> 24 > 1)) w = desk = 0;   /* right-click on bare wallpaper: the desktop menu */
    int task = taskbar_at(x, y);
    if (task != T_NONE) w = 0, edges = 0;
    int btn = w && in_frame && !edges ? title_button_at(w, x, y) : -1;
    set_hover(desk ? 0 : w, btn, task == T_NONE ? -1 : task);
    set_shape(edges);

    if (w && !in_frame) {                                      /* motion inside a client (terminals take it too, for link hover) */
        if (last_in && last_in != w) { struct gui_event e = { .type = EV_MOUSE_LEAVE }; send(last_in, &e); }
        last_in = w;
        if (ox != x || oy != y) { struct gui_event e = { .type = EV_MOUSE_MOVE, .x = x - w->x, .y = y - w->y, .button = b }; send(w, &e); }
    }
    if (dz && task == T_VOL) set_volume(vol_cache - (dz > 0 ? 5 : -5));
    if (dz && w && !in_frame) { struct gui_event e = { .type = EV_SCROLL, .x = x - w->x, .y = y - w->y, .dz = dz }; send(w, &e); }

    for (int k = 2; k <= 4; k += 2)                                          /* right and middle */
        if ((b & k) && !(ob & k) && w && !in_frame) { struct gui_event e = { .type = EV_MOUSE_DOWN, .x = x - w->x, .y = y - w->y, .button = k }; send(w, &e); }
    if (rdown && w && in_frame && !edges && y < w->y && !(w->flags & WIN_FLAG_FRAMELESS)) {   /* window menu */
        wm_focus(w);
        win_menu(w, x, y);
        return;
    }
    if (rdown && !w && task == T_NONE) { desk_menu(x, y); return; }   /* desktop menu */
    if (!down) return;

    if (menu_open && task != T_LAUNCH) l_close();                  /* a click elsewhere closes the launcher */
    if (task == T_LAUNCH) { l_toggle(); return; }
    if (task >= 0) {
        struct window *tw = tasks[task];
        if (tw == focused && !tw->minimized) minimize(tw);
        else wm_focus(tw);
        return;
    }
    if (task == T_MORE) { tasks_menu(); return; }
    if (task <= T_WS0 && task > T_WS0 - WM_WS) { switch_ws(T_WS0 - task); return; }
    if (task == T_VOL || task == T_NET) { qs_toggle(); return; }
    if (task == T_CLOCK || task == T_BELL) { cn_toggle(); return; }
    if (task == T_GAP) return;
    if (!w || desk) { if (focused) { struct window *o = focused; focused = 0; damage_window(o); wm_damage(0, TB_Y, SW, WM_TASKBAR_H); } if (!w) return; }

    if ((keyboard_mods() & MOD_SUPER) && !(w->flags & WIN_FLAG_FRAMELESS)) {   /* Super+drag moves from anywhere */
        super_alone = false;
        wm_focus(w);
        drag_win = w; drag_dx = x - w->x; drag_dy = y - w->y; drag_sx = x; drag_sy = y;
        return;
    }
    if (!(w->flags & WIN_FLAG_FRAMELESS)) wm_focus(w);
    if (edges) {
        resize_win = w; rs_edges = edges; rs_from = (struct rect){ w->x, w->y, w->w, w->h };
        drag_sx = x; drag_sy = y;
        return;
    }
    if (in_frame) {
        if (btn == 0) { minimize(w); return; }
        if (btn == 1) { toggle_max(w); return; }
        if (btn == 2) { close_window(w); return; }
        if (y < w->y) {
            if (last_click_win == w && ticks_ms - last_click_ms < 400) { toggle_max(w); last_click_win = 0; return; }
            last_click_win = w; last_click_ms = ticks_ms;
            drag_win = w; drag_dx = x - w->x; drag_dy = y - w->y; drag_sx = x; drag_sy = y;
        }
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
        else if (super_alone) l_toggle();
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
        if (mkind) { m_key(ev); return; }
        if (menu_open && l_key(ev)) return;
        if (mods & MOD_SUPER) {                           /* workspaces + notification centre */
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
        if (ev->key == KEY_ESC && (menu_open || cn_open || qs_open)) { close_popups(); return; }
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
        w->owner_pid = current->proc->pid;
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
        struct window *w = wm_create(wi->title, wi->x, wi->y, wi->w, wi->h, wi->flags & 7);
        if (!w) return -ENOMEM;
        if (map_into_owner(w) < 0) { wm_destroy(w); kfree(w); return -ENOMEM; }
        w->vn.mode = S_IFCHR | 0600;
        w->vn.ops = &win_ops;
        w->vn.priv = w;
        struct file *f = vfs_open_vnode(&w->vn, O_RDWR);
        int fd = f ? fd_alloc(f) : -ENOMEM;
        if (fd < 0) { if (f) vfs_close(f); return fd; }
        current->proc->fd_cloexec |= 1u << fd;     /* the pixels are mapped into this image only */
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
        else set_geometry(w, w->x, w->y, imax(80, imin(r->w, SW)), imax(40, imin(r->h, TB_Y - WM_TITLE_H)));
        return 0;
    }
    case WIN_SET_PREFS:
        load_prefs(false);
        return 0;
    case WIN_SET_VISIBLE:
        if (!c && w->visible) ghost_make(w);
        if (c && !w->visible) w->born = ticks_ms;
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
    if (!noresize) { r->w = imin(r->w, SW); r->h = imin(r->h, TB_Y - WM_TITLE_H); }
    r->x = imax(0, imin(r->x, SW - r->w));
    r->y = imax(WM_TITLE_H, imin(r->y, TB_Y - r->h));
}

/* Allocate everything first (back buffer, wallpaper, bigger window buffers), so a failure leaves
 * the old mode intact; then program the adapter, swap the buffers in, re-lay-out the windows. */
int wm_set_mode(int nw, int nh) {
    if (nw == SW && nh == SH) return 0;
    struct regrow { struct window *w; uint32_t *buf, frames; int zone; } *rg;
    int n = 0, r = -ENOMEM;
    for (struct window *w = wins; w; w = w->next) n++;
    if (!(rg = kzalloc((n + 1) * sizeof *rg))) return -ENOMEM;
    ghost_free();
    big_free(under, SW, SH); big_free(ws_old, SW, SH); big_free(wp_stage, SW, SH);
    under = ws_old = wp_stage = 0;
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
    bool had_img = wall_img;
    render_wallpaper();
    if (had_img || wp_cfg[0] == '/') wp_redo = true;       /* the pushed image was for the old size: /bin/theme -W pushes /etc/wallpaper again */
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
        if (w->flags & WIN_FLAG_DESKTOP) { set_geometry(w, 0, 0, SW, TB_Y); continue; }
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

/* a theme switch (gui/theme.c): every window hears about it from the compositor thread, which then repaints it all */
static volatile bool theme_pending;
void wm_theme_changed(void) { if (running) { theme_pending = true; wm_wake(); } }
static void theme_broadcast(void) {
    struct gui_event e = { .type = EV_THEME, .x = theme_zoom_changed() };
    for (struct window *w = wins; w; w = w->next) send(w, &e);
    wm_damage(0, 0, SW, SH);
}

/* volume and network state can change behind our back (mixer ioctls, DHCP): poll cheaply, damage on change */
static void poll_tray(void) {
    int v = ac97_volume(-1);
    bool up = ni.up, pr = ni.present;
    uint32_t ip = ni.ip;
    if (v == vol_cache && up == net_up && pr == net_present && ip == net_ip) return;
    vol_cache = v; net_up = up; net_present = pr; net_ip = ip;
    wm_damage(0, TB_Y, SW, WM_TASKBAR_H);
    if (qs_open) qs_damage();
}

static int wm_thread(void *arg) {
    uint64_t last_tick = 0;
    for (;;) {
        uint32_t f = irq_save();
        while (inq_h == inq_t && !dirty.w && ticks_ms - last_tick < 250 && !tip_due())
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
        if (theme_pending) { theme_pending = false; theme_broadcast(); }
        for (struct window *w = wins, *n; w; w = n) {
            n = w->next;
            if (w->on_frame) w->on_frame(w);
        }
        if (ticks_ms - last_tick >= 250) {
            last_tick = ticks_ms;
            poll_tray();
            if (wp_redo && !(wp_pid && task_find(wp_pid))) { wp_redo = false; wp_pid = wm_spawn_app("/bin/theme", "-W"); }
            for (int i = 0, k = 0; i < nhist && toast_hov < 0; i++)   /* notifications time out (not while one is hovered) */
                if (hist[i].toast && (int32_t)((uint32_t)ticks_ms - hist[i].until) >= 0) { if (!k++) toast_damage(); hist[i].toast = false; }
            uint32_t t = rtc_now() + tz_minutes * 60;
            if ((int)((t / 60) % 60) != last_minute) { last_minute = (t / 60) % 60; wm_damage(0, TB_Y, SW, WM_TASKBAR_H); }
            if (menu_open && ticks_ms - l_typed < D_CARET_IDLE + 250) l_damage();   /* the search caret blinks */
        }
        if (tip_due()) { tip_on = true; wm_damage(0, TB_Y - 3 * H_PANEL, SW, 3 * H_PANEL); }
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

/* /etc/wallpaper (a theme name or an image path) and /etc/tz (minutes east of UTC); Settings asks for a reload */
static void load_prefs(bool init) {
    void *d; uint32_t n;
    if (vfs_read_all("/etc/wallpaper", &d, &n) == 0) {
        char cfg[sizeof wp_cfg];
        n = MIN(n, sizeof cfg - 1);
        memcpy(cfg, d, n);
        kfree(d);
        while (n && cfg[n - 1] <= ' ') n--;
        cfg[n] = 0;
        if (init || strcmp(cfg, wp_cfg)) {                 /* only a change in the file touches the wallpaper */
            strlcpy(wp_cfg, cfg, sizeof wp_cfg);
            if (cfg[0] == '/') {
                if (init) render_wallpaper();              /* stands in until /bin/theme has pushed the image */
                wp_redo = true;
            } else {
                for (int i = 0; i < wallpaper_count(); i++) if (!strncmp(cfg, wallpaper_name(i), strlen(wallpaper_name(i)))) wall_theme = i;
                render_wallpaper();
            }
        }
    } else if (init) render_wallpaper();
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
    theme_init();
    load_prefs(true);
    splash_progress(95);
    bezier_lut(ease_o, 0, 0, 0, 256);
    bezier_lut(ease_i, 102, 0, 256, 256);
    for (int i = 0; i < WM_RADIUS; i++)                    /* pixels not wholly inside the corner arc */
        corner_inset[i] = WM_RADIUS - isqrt(WM_RADIUS * WM_RADIUS - (WM_RADIUS - i) * (WM_RADIUS - i));
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
    wm_damage(0, 0, SW, SH);
    if (snap) { no_present = true; compose(); no_present = false; crossfade(snap, 32, 30); big_free(snap, SW, SH); }
    kthread_create("compositor", wm_thread, 0);
    kthread_create("chime", chime, 0);
    autostart();
    struct vnode *dv;
    if (vfs_lookup("/", "/bin/desktop", &dv) == 0) { vnode_put(dv); wm_spawn_app("/bin/desktop", 0); }     /* desktop icons */
    kprintf("[desktop] ready\n");
}
