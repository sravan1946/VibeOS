/* clock - analog/digital clock, stopwatch and countdown timer */
#include <ui.h>
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

/* the dial (shadow, ticks, numerals) never changes: render it once per radius into a buffer */
static void draw_dial(struct canvas *c, int cx, int cy, int r) {
    gfx_shadow(c, cx - r, cy - r, 2 * r, 2 * r, r, 14, 90);
    gfx_circle_fill(c, cx, cy, r, ui_th.hover);
    gfx_circle_fill(c, cx, cy, r - 3, UI_PANEL);
    gfx_circle_fill(c, cx, cy, r - 5, UI_BG);
    for (int i = 0; i < 60; i++) {
        float a = i * (float)M_PI / 30, s = sinf(a), co = cosf(a);
        bool major = i % 5 == 0;
        float r0 = r - (major ? 22 : 13), r1 = r - 9;
        gfx_line_aa(c, cx + s * r0, cy - co * r0, cx + s * r1, cy - co * r1, major ? 3.0f : 1.4f, major ? UI_ACCENT : UI_OV(80));
    }
    static const char *const num[] = { "12", "3", "6", "9" };
    static const int nx[] = { 0, 1, 0, -1 }, ny[] = { -1, 0, 1, 0 };
    for (int i = 0; i < 4; i++) {
        int tw = gfx_text_width(&font_ui_bold, num[i]);
        int px = cx + nx[i] * (r - 40) - tw / 2, py = cy + ny[i] * (r - 40) - font_ui_bold.height / 2;
        gfx_text(c, &font_ui_bold, px, py, num[i], UI_TEXT);
    }
}

static uint32_t *dial_buf;
static int dial_r;
static uint32_t dial_ser;                      /* theme the cached dial was drawn in */
#define DIAL_PAD 20

static void draw_face(struct ui *ui, int cx, int cy, int r, double hh, double mm, double ss) {
    struct canvas *c = ui->c;
    int sz = 2 * (r + DIAL_PAD);
    if (dial_r != r || dial_ser != ui_th.accent + ui_th.bg) {
        dial_ser = ui_th.accent + ui_th.bg;
        free(dial_buf);
        dial_buf = calloc(sz * sz, 4);
        dial_r = r;
        if (dial_buf) {
            struct canvas dc;
            canvas_init(&dc, dial_buf, sz, sz, sz);
            gfx_fill(&dc, 0, 0, sz, sz, UI_BG);          /* tab background: lets us blit opaque */
            draw_dial(&dc, r + DIAL_PAD, r + DIAL_PAD, r);
        }
    }
    if (dial_buf) gfx_blit(c, cx - r - DIAL_PAD, cy - r - DIAL_PAD, dial_buf, sz, sz, sz);
    else draw_dial(c, cx, cy, r);
    float cxf = cx, cyf = cy;
    hand(c, cxf + 2, cyf + 3, (float)(hh * M_PI / 6), r * 0.5f, 0, 6.0f, RGBA(0, 0, 0, 70));     /* soft shadows */
    hand(c, cxf + 2, cyf + 3, (float)(mm * M_PI / 30), r * 0.74f, 0, 4.5f, RGBA(0, 0, 0, 70));
    hand(c, cxf, cyf, (float)(hh * M_PI / 6), r * 0.5f, 6, 6.0f, UI_TEXT);
    hand(c, cxf, cyf, (float)(mm * M_PI / 30), r * 0.74f, 8, 4.0f, UI_ACCENT);
    hand(c, cxf, cyf, (float)(ss * M_PI / 30), r * 0.84f, r * 0.16f, 2.0f, UI_ACCENT2);
    gfx_circle_fill(c, cx, cy, 7, UI_ACCENT2);
    gfx_circle_fill(c, cx, cy, 3, UI_BG);
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

    int infoh = 124;
    int avail_h = H - top - infoh - 12, avail_w = W - 24;
    int r = (avail_h < avail_w ? avail_h : avail_w) / 2 - 6;
    if (r < 40) r = 40;
    draw_face(ui, W / 2, top + 6 + r, r, hh, mm, ss);

    int y = top + 12 + 2 * r;
    ui_panel(ui, 12, y, W - 24, infoh - 4 < H - y - 12 ? H - y - 12 : infoh - 4);
    char buf[64];
    snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    gfx_text_center(c, &font_big, 0, y + 8, W, buf, UI_TEXT);
    snprintf(buf, sizeof buf, "%s, %d %s %d", DAY[tm.tm_wday % 7], tm.tm_mday, MONTH[tm.tm_mon % 12], tm.tm_year + 1900);
    gfx_text_center(c, &font_ui, 0, y + 8 + font_big.height + 2, W, buf, UI_TEXT_DIM);
    int off = tz_min;
    snprintf(buf, sizeof buf, "UTC%c%02d:%02d", off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
    gfx_text_center(c, &font_ui, 0, y + 8 + font_big.height + 22, W, buf, UI_TEXT_DIM);
}

static void tab_stopwatch(struct ui *ui, int top) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    uint32_t el = sw_elapsed();
    char buf[48];
    fmt_ms(buf, sizeof buf, el, true);
    ui_panel(ui, 12, top, W - 24, 110);
    gfx_text_center(c, &font_big, 0, top + 28, W, buf, sw_run ? UI_TEXT : UI_TEXT_DIM);
    int bw = (W - 24 - 20) / 3, by = top + 124;
    if (ui_button(ui, 12, by, bw, 40, sw_run ? "Stop" : "Start", sw_run ? UI_NORMAL_BTN : UI_ACCENT_BTN)) {
        if (sw_run) { sw_acc += uptime_ms() - sw_start; sw_run = false; }
        else { sw_start = uptime_ms(); sw_run = true; }
    }
    if (ui_button(ui, 22 + bw, by, bw, 40, "Lap", UI_NORMAL_BTN) && (sw_run || el) && nlaps < MAX_LAPS) {
        laps[nlaps++] = el;
        lap_scroll = 1 << 20;
    }
    if (ui_button(ui, 32 + 2 * bw, by, bw, 40, "Reset", UI_GHOST_BTN)) { sw_run = false; sw_acc = 0; nlaps = 0; }
    int ly = by + 54, lh = H - ly - 12;
    if (lh < 40) return;
    ui_panel(ui, 12, ly, W - 24, lh);
    int rowh = 26, vis = (lh - 12) / rowh;
    if (!nlaps) { gfx_text_center(c, &font_ui, 0, ly + lh / 2 - 8, W, "No laps yet", UI_TEXT_DIM); return; }
    if (ui_hover(ui, 12, ly, W - 24, lh) && ui->scroll) lap_scroll += ui->scroll;
    if (lap_scroll > nlaps - vis) lap_scroll = nlaps - vis;
    if (lap_scroll < 0) lap_scroll = 0;
    ui_vscroll(ui, W - 24, ly + 6, lh - 12, nlaps, vis, &lap_scroll);
    struct canvas cc = *c;
    canvas_clip(&cc, 12, ly + 4, W - 24, lh - 8);
    for (int i = lap_scroll; i < nlaps && i < lap_scroll + vis; i++) {
        int y = ly + 8 + (i - lap_scroll) * rowh;
        char a[32], b[32];
        fmt_ms(a, sizeof a, laps[i] - (i ? laps[i - 1] : 0), true);
        fmt_ms(b, sizeof b, laps[i], true);
        snprintf(buf, sizeof buf, "Lap %d", i + 1);
        gfx_text(&cc, &font_ui, 28, y + 2, buf, UI_TEXT_DIM);
        gfx_text_center(&cc, &font_mono, W / 2 - 30, y + 1, 100, a, UI_TEXT);
        gfx_text(&cc, &font_mono, W - 36 - gfx_text_width(&font_mono, b), y + 1, b, UI_ACCENT);
    }
}

static int tm_remaining_ms(void) {
    if (tm_done) return 0;
    if (!tm_run) return tm_left_ms;
    int d = (int)(tm_end - uptime_ms());
    return d > 0 ? d : 0;
}

static void tab_timer(struct ui *ui, int top) {
    struct canvas *c = ui->c;
    int W = ui->win->w, H = ui->win->h;
    int left = tm_remaining_ms();
    bool idle = !tm_run && !tm_done && tm_left_ms == tm_set * 1000;
    int r = (H - top - 130 < W - 60 ? H - top - 130 : W - 60) / 2;
    if (r < 50) r = 50;
    int cx = W / 2, cy = top + 14 + r;
    /* ring */
    float frac = tm_set ? (float)left / (tm_set * 1000.0f) : 0;
    gfx_circle_fill(ui->c, cx, cy, r + 2, UI_SURFACE);
    gfx_circle_fill(ui->c, cx, cy, r - 12, UI_BG);
    if (tm_done) {
        bool flash = (uptime_ms() / 400) & 1;
        arc(c, cx, cy, r - 5, 0, 2 * (float)M_PI, 8, flash ? UI_BAD : UI_WARN);
    } else if (frac > 0.001f) {
        arc(c, cx, cy, r - 5, 0, 2 * (float)M_PI * frac, 8, UI_ACCENT);
    }
    char buf[32];
    uint32_t secs = (left + 999) / 1000;
    if (secs >= 3600) snprintf(buf, sizeof buf, "%u:%02u:%02u", secs / 3600, secs / 60 % 60, secs % 60);
    else snprintf(buf, sizeof buf, "%02u:%02u", secs / 60, secs % 60);
    gfx_text_center(c, &font_big, cx - r, cy - font_big.height / 2 - 4, 2 * r, buf, tm_done ? UI_WARN : UI_TEXT);
    gfx_text_center(c, &font_ui, cx - r, cy + font_big.height / 2 + 2, 2 * r, tm_done ? "Time is up!" : tm_run ? "Running" : idle ? "Ready" : "Paused", UI_TEXT_DIM);

    int y = cy + r + 16;
    if (idle) {
        /* adjust minutes / seconds */
        int bx = W / 2 - 150;
        static const struct { const char *l; int d; } adj[] = { { "-5m", -300 }, { "-1m", -60 }, { "-10s", -10 }, { "+10s", 10 }, { "+1m", 60 }, { "+5m", 300 } };
        for (int i = 0; i < 6; i++)
            if (ui_button(ui, bx + i * 50, y, 46, 30, adj[i].l, UI_NORMAL_BTN)) {
                tm_set += adj[i].d;
                if (tm_set < 10) tm_set = 10;
                if (tm_set > 99 * 60 + 59) tm_set = 99 * 60 + 59;
                tm_left_ms = tm_set * 1000;
            }
    }
    y += 40;                                          /* keep Start/Reset from jumping */
    int bw = (W - 24 - 10) / 2;
    if (y + 40 > H - 6) y = H - 46;
    const char *go = tm_run ? "Pause" : tm_done ? "Restart" : "Start";
    if (ui_button(ui, 12, y, bw, 40, go, tm_run ? UI_NORMAL_BTN : UI_ACCENT_BTN)) {
        if (tm_run) { tm_left_ms = tm_remaining_ms(); tm_run = false; }
        else {
            if (tm_done) { tm_done = false; tm_left_ms = tm_set * 1000; beeps_left = 0; }
            if (tm_left_ms <= 0) tm_left_ms = tm_set * 1000;
            tm_end = uptime_ms() + tm_left_ms;
            tm_run = true;
        }
    }
    if (ui_button(ui, 22 + bw, y, bw, 40, "Reset", UI_GHOST_BTN)) {
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
        ui_tabs(&ui, 12, 12, w->w - 24, 36, names, 3, &tab);
        int top = 60;
        if (tab == 0) tab_clock(&ui, top);
        else if (tab == 1) tab_stopwatch(&ui, top);
        else tab_timer(&ui, top);
        ui_end(&ui);
        if (beeps_left > 0) { beep(880, 120); beeps_left--; if (beeps_left) usleep(80000); }
    } while (ui_poll(&ui, tab == 0 ? 100 : (tab == 1 && sw_run) || tm_run || (tab == 2 && tm_done) ? 40 : 500));
    gui_window_destroy(w);
    return 0;
}
