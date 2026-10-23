/* clock - analog/digital clock, stopwatch and countdown timer */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <syscall.h>

static int tz_min;
static const char *const MONTH[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static const char *const DAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

/* ---- shared state ---- */
static uint32_t last_unix, sec_origin;      /* for the sub-second fraction of the RTC */

/* stopwatch */
static bool sw_run;
static uint32_t sw_start, sw_acc;           /* ms: segment start, accumulated before it */
#define MAX_LAPS 64
static uint32_t laps[MAX_LAPS];
static int nlaps, lap_scroll;

/* timer */
static int tm_set = 5 * 60;                 /* seconds configured */
static bool tm_run, tm_done;
static uint32_t tm_end;                     /* uptime ms when it fires */
static int tm_left_ms;                      /* when paused */
static int beeps_left;

static uint32_t sw_elapsed(void) { return sw_acc + (sw_run ? uptime_ms() - sw_start : 0); }

static void read_tz(void) {
    FILE *f = fopen("/etc/tz", "r");
    if (!f) return;
    char b[16] = { 0 };
    if (fgets(b, sizeof b, f)) tz_min = atoi(b);
    fclose(f);
}

static void fmt_ms(char *out, int n, uint32_t ms, bool cs) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(out, n, cs ? "%u:%02u:%02u.%02u" : "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60, ms / 10 % 100);
    else snprintf(out, n, cs ? "%02u:%02u.%02u" : "%02u:%02u", s / 60, s % 60, ms / 10 % 100);
}

/* ---- drawing helpers ---- */
static void arc(struct canvas *c, float cx, float cy, float r, float a0, float a1, float w, color_t col) {
    int steps = (int)((a1 - a0) * r / 5) + 2;
    float px = cx + r * sinf(a0), py = cy - r * cosf(a0);
    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * i / steps;
        float x = cx + r * sinf(a), y = cy - r * cosf(a);
        gfx_line_aa(c, px, py, x, y, w, col);
        px = x; py = y;
    }
}

static void hand(struct canvas *c, float cx, float cy, float ang, float len, float tail, float w, color_t col) {
    float s = sinf(ang), co = cosf(ang);
    gfx_line_aa(c, cx - s * tail, cy + co * tail, cx + s * len, cy - co * len, w, col);
}

/* the dial (ticks, numerals) never changes: render it once per radius and theme into a buffer */
static void draw_dial(struct canvas *c, int cx, int cy, int r) {
    gfx_circle_fill(c, cx, cy, r + 1, ui_th.line);
    gfx_circle_fill(c, cx, cy, r, ui_th.raised);
    for (int i = 0; i < 60; i++) {
        float a = i * (float)M_PI / 30, s = sinf(a), co = cosf(a);
        bool major = i % 5 == 0;
        float r0 = r - (major ? 16 : 10), r1 = r - 6;
        gfx_line_aa(c, cx + s * r0, cy - co * r0, cx + s * r1, cy - co * r1, major ? 2.0f : 1.0f, major ? ui_th.text2 : ui_th.control);
    }
    static const char *const num[] = { "12", "3", "6", "9" };
    static const int nx[] = { 0, 1, 0, -1 }, ny[] = { -1, 0, 1, 0 };
    for (int i = 0; i < 4; i++) {
        const struct font *f = T_HEADING;
        int tw = gfx_text_width(f, num[i]);
        int px = cx + nx[i] * (r - 32) - tw / 2, py = cy + ny[i] * (r - 32) - f->height / 2;
        gfx_text(c, f, px, py, num[i], ui_th.text2);
    }
}

static uint32_t *dial_buf;
static int dial_r;
static uint32_t dial_ser;                      /* theme the cached dial was drawn in */
#define DIAL_PAD 4

static void draw_face(struct ui *ui, int cx, int cy, int r, double hh, double mm, double ss) {
    struct canvas *c = ui->c;
    int sz = 2 * (r + DIAL_PAD);
    uint32_t ser = ui_th.raised ^ ui_th.text2 * 3 ^ ui_th.window * 7;
    if (dial_r != r || dial_ser != ser) {
        dial_ser = ser;
        free(dial_buf);
        dial_buf = calloc(sz * sz, 4);
        dial_r = r;
        if (dial_buf) {
            struct canvas dc;
            canvas_init(&dc, dial_buf, sz, sz, sz);
            gfx_fill(&dc, 0, 0, sz, sz, UI_BG);          /* window background: lets us blit opaque */
            draw_dial(&dc, r + DIAL_PAD, r + DIAL_PAD, r);
        }
    }
    if (dial_buf) gfx_blit(c, cx - r - DIAL_PAD, cy - r - DIAL_PAD, dial_buf, sz, sz, sz);
    else draw_dial(c, cx, cy, r);
    float cxf = cx, cyf = cy;
    hand(c, cxf, cyf, (float)(hh * M_PI / 6), r * 0.5f, 0, 4.0f, UI_TEXT);
    hand(c, cxf, cyf, (float)(mm * M_PI / 30), r * 0.74f, 0, 3.0f, UI_TEXT);
    hand(c, cxf, cyf, (float)(ss * M_PI / 30), r * 0.84f, r * 0.14f, 1.5f, ui_th.accent_line);
    gfx_circle_fill(c, cx, cy, 4, ui_th.accent_line);
}

/* ---- tabs ---- */
static void tab_clock(struct ui *ui, int top) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    uint32_t u = time(NULL);
    uint32_t now = uptime_ms();
    if (u != last_unix) { last_unix = u; sec_origin = now; }
    uint32_t frac = now - sec_origin;
    if (frac > 999) frac = 999;
    int64_t local = (int64_t)u + tz_min * 60;
    time_t lt = (time_t)local;
    struct tm tm;
    gmtime_r(&lt, &tm);
    double ss = tm.tm_sec + frac / 1000.0, mm = tm.tm_min + ss / 60, hh = (tm.tm_hour % 12) + mm / 60;

    int infoh = T_DISPLAY->height + SP_2 + T_BODY->height + T_CAPTION->height + SP_7;
    int avail_h = H - top - infoh - 2 * SP_7, avail_w = W - 2 * SP_7;
    int r = (avail_h < avail_w ? avail_h : avail_w) / 2;
    if (r < 40) r = 40;
    draw_face(ui, W / 2, top + SP_7 + r, r, hh, mm, ss);

    int y = top + SP_7 + 2 * r + SP_7;
    char buf[64];
    snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    gfx_text_center(c, T_DISPLAY, 0, y, W, buf, UI_TEXT);
    y += T_DISPLAY->height + SP_2;
    snprintf(buf, sizeof buf, "%s, %d %s %d", DAY[tm.tm_wday % 7], tm.tm_mday, MONTH[tm.tm_mon % 12], tm.tm_year + 1900);
    gfx_text_center(c, T_BODY, 0, y, W, buf, UI_TEXT2);
    int off = tz_min;
    snprintf(buf, sizeof buf, "UTC%c%02d:%02d", off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
    gfx_text_center(c, T_CAPTION, 0, y + T_BODY->height, W, buf, UI_TEXT3);
}

static void tab_stopwatch(struct ui *ui, int top) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h, m = SP_5;
    uint32_t el = sw_elapsed();
    char buf[48];
    fmt_ms(buf, sizeof buf, el, true);
    int y = top + SP_8;
    gfx_text_center(c, T_DISPLAY, 0, y, W, buf, sw_run || el ? UI_TEXT : UI_TEXT2);
    y += T_DISPLAY->height + SP_7;
    const char *go = sw_run ? "Stop" : el ? "Resume" : "Start";
    int bw = ui_button_w(ICON_PLAY, "Resume"), lw = ui_button_w(ICON_FLAG, "Lap"), rw = ui_button_w(ICON_RESTART, "Reset");
    int bx = (W - bw - lw - rw - 2 * SP_3) / 2;
    if (ui_button_ex(ui, bx, y, bw, H_CTL, sw_run ? ICON_PAUSE : ICON_PLAY, go, sw_run ? UI_SECONDARY : UI_PRIMARY)) {
        if (sw_run) { sw_acc += uptime_ms() - sw_start; sw_run = false; }
        else { sw_start = uptime_ms(); sw_run = true; }
    }
    if (ui_button_ex(ui, bx + bw + SP_3, y, lw, H_CTL, ICON_FLAG, "Lap", UI_SECONDARY | ((sw_run || el) && nlaps < MAX_LAPS ? 0 : UI_DISABLED))) {
        laps[nlaps++] = el;
        lap_scroll = 1 << 20;
    }
    if (ui_button_ex(ui, bx + bw + lw + 2 * SP_3, y, rw, H_CTL, ICON_RESTART, "Reset", UI_GHOST | (el || nlaps ? 0 : UI_DISABLED))) {
        sw_run = false; sw_acc = 0; nlaps = 0;
    }
    int ly = y + H_CTL + SP_7, lh = H - ly - m;
    if (lh < H_ROW + 2 * SP_2) return;
    ui_card(ui, m, ly, W - 2 * m, lh);
    if (!nlaps) { gfx_text_center(c, T_BODY, 0, ly + (lh - T_BODY->height) / 2, W, "Laps appear here while the stopwatch runs", UI_TEXT2); return; }
    int vis = (lh - 2 * SP_2) / H_ROW;
    if (ui_hover(ui, m, ly, W - 2 * m, lh) && ui->scroll) lap_scroll += ui->scroll;
    if (lap_scroll > nlaps - vis) lap_scroll = nlaps - vis;
    if (lap_scroll < 0) lap_scroll = 0;
    struct canvas cc = *c;
    canvas_clip(&cc, m, ly + SP_2, W - 2 * m, lh - 2 * SP_2);
    for (int i = lap_scroll; i < nlaps && i < lap_scroll + vis; i++) {
        int ry = ly + SP_2 + (i - lap_scroll) * H_ROW, ty = ry + (H_ROW - T_BODY->height) / 2;
        char a[32], b[32];
        fmt_ms(a, sizeof a, laps[i] - (i ? laps[i - 1] : 0), true);
        fmt_ms(b, sizeof b, laps[i], true);
        snprintf(buf, sizeof buf, "Lap %d", i + 1);
        if (i > lap_scroll) gfx_hline(&cc, m + SP_4, ry, W - 2 * m - 2 * SP_4, UI_LINE);
        gfx_text(&cc, T_BODY, m + SP_4, ty, buf, UI_TEXT2);
        gfx_text(&cc, T_BODY, W / 2 + 40 - gfx_text_width(T_BODY, a), ty, a, UI_TEXT);
        gfx_text(&cc, T_BODY, W - m - SP_4 - SP_4 - gfx_text_width(T_BODY, b), ty, b, UI_TEXT2);
    }
    ui_vscroll(ui, W - m - SP_4, ly + SP_2, lh - 2 * SP_2, nlaps, vis, &lap_scroll);
}

static int tm_remaining_ms(void) {
    if (tm_done) return 0;
    if (!tm_run) return tm_left_ms;
    int d = (int)(tm_end - uptime_ms());
    return d > 0 ? d : 0;
}

static void tab_timer(struct ui *ui, int top) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h, m = SP_5;
    int left = tm_remaining_ms();
    bool idle = !tm_run && !tm_done && tm_left_ms == tm_set * 1000;
    int r = (H - top - 2 * SP_7 - 2 * H_CTL - SP_5 - SP_7 < W - 2 * SP_8 ? H - top - 2 * SP_7 - 2 * H_CTL - SP_5 - SP_7 : W - 2 * SP_8) / 2;
    if (r < 50) r = 50;
    int cx = W / 2, cy = top + SP_7 + r;
    /* progress ring: line track, accent fill (DESIGN §9 Progress, bent round) */
    float frac = tm_set ? (float)left / (tm_set * 1000.0f) : 0;
    arc(c, cx, cy, r - 2, 0, 2 * (float)M_PI, 4, tm_done ? ui_th.warning : ui_th.line);
    if (!tm_done && frac > 0.001f) arc(c, cx, cy, r - 2, 0, 2 * (float)M_PI * frac, 4, ui_th.accent_line);
    char buf[32];
    uint32_t secs = (left + 999) / 1000;
    if (secs >= 3600) snprintf(buf, sizeof buf, "%u:%02u:%02u", secs / 3600, secs / 60 % 60, secs % 60);
    else snprintf(buf, sizeof buf, "%02u:%02u", secs / 60, secs % 60);
    int ty = cy - (T_DISPLAY->height + T_BODY->height) / 2;
    gfx_text_center(c, T_DISPLAY, cx - r, ty, 2 * r, buf, UI_TEXT);
    const char *st = tm_done ? "Time is up" : tm_run ? "Running" : idle ? "Ready" : "Paused";
    ty += T_DISPLAY->height;
    if (tm_done) {                                     /* warning hue sits near the accent: always with its icon */
        int sw = ICON_SM + SP_2 + gfx_text_width(T_BODY, st), sx = cx - sw / 2;
        ui_icon(ui, sx, ty + 1, ICON_WARNING, ICON_SM, ui_th.warning);
        gfx_text(c, T_BODY, sx + ICON_SM + SP_2, ty, st, ui_th.warning);
    } else gfx_text_center(c, T_BODY, cx - r, ty, 2 * r, st, UI_TEXT2);

    int y = cy + r + SP_7;
    if (idle) {
        static const struct { const char *l; int d; } adj[] = { { "\xe2\x88\x92" "5m", -300 }, { "\xe2\x88\x92" "1m", -60 }, { "\xe2\x88\x92" "10s", -10 },
                                                            { "+10s", 10 }, { "+1m", 60 }, { "+5m", 300 } };
        int aw[6], tw = 5 * SP_2;
        for (int i = 0; i < 6; i++) tw += aw[i] = ui_button_w(UI_NOICON, adj[i].l);
        int bx = (W - tw) / 2;
        for (int i = 0; i < 6; i++) {
            if (ui_button(ui, bx, y, aw[i], H_CTL, adj[i].l, UI_SECONDARY)) {
                tm_set += adj[i].d;
                if (tm_set < 10) tm_set = 10;
                if (tm_set > 99 * 60 + 59) tm_set = 99 * 60 + 59;
                tm_left_ms = tm_set * 1000;
            }
            bx += aw[i] + SP_2;
        }
    }
    y += H_CTL + SP_5;                                   /* keep Start/Reset from jumping */
    if (y + H_CTL > H - m) y = H - m - H_CTL;
    const char *go = tm_run ? "Pause" : tm_done ? "Restart" : idle ? "Start" : "Resume";
    int gw = ui_button_w(ICON_PLAY, "Restart"), rw = ui_button_w(ICON_RESTART, "Reset"), gx = (W - gw - rw - SP_3) / 2;
    if (ui_button_ex(ui, gx, y, gw, H_CTL, tm_run ? ICON_PAUSE : ICON_PLAY, go, tm_run ? UI_SECONDARY : UI_PRIMARY)) {
        if (tm_run) { tm_left_ms = tm_remaining_ms(); tm_run = false; }
        else {
            if (tm_done) { tm_done = false; tm_left_ms = tm_set * 1000; beeps_left = 0; }
            if (tm_left_ms <= 0) tm_left_ms = tm_set * 1000;
            tm_end = uptime_ms() + tm_left_ms;
            tm_run = true;
        }
    }
    if (ui_button_ex(ui, gx + gw + SP_3, y, rw, H_CTL, ICON_RESTART, "Reset", UI_GHOST | (idle ? UI_DISABLED : 0))) {
        tm_run = tm_done = false; beeps_left = 0; tm_left_ms = tm_set * 1000;
    }
}

int main(void) {
    struct gui_window *w = gui_window_create("Clock", 420, 560, 0);
    if (!w) { perror("clock"); return 1; }
    read_tz();
    tm_left_ms = tm_set * 1000;
    struct ui ui;
    ui_init(&ui, w);
    int tab = 0;
    static const char *const names[] = { "Clock", "Stopwatch", "Timer" };
    do {
        if (tm_run && tm_remaining_ms() == 0) { tm_run = false; tm_done = true; beeps_left = 3; }   /* fires on any tab */
        ui_clear(&ui);
        ui_tabbar(&ui, 0, 0, w->w, names, 3, &tab, 0);
        int top = H_TAB;
        if (tab == 0) tab_clock(&ui, top);
        else if (tab == 1) tab_stopwatch(&ui, top);
        else tab_timer(&ui, top);
        ui_end(&ui);
        if (beeps_left > 0) { beep(880, 120); beeps_left--; if (beeps_left) usleep(80000); }
    } while (ui_poll(&ui, tab == 0 ? 100 : (tab == 1 && sw_run) || tm_run || (tab == 2 && tm_done) ? 40 : 500));
    gui_window_destroy(w);
    return 0;
}
