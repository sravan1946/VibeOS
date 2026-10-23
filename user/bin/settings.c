/* settings - appearance, wallpaper, display mode and time zone; the toolkit's reference app (docs/design/a-launcher.png).
 * Themes go through /dev/theme (and /etc/theme for the next boot); the desktop reloads wallpaper/time zone prefs via
 * WIN_SET_PREFS; modes go through /dev/display (and /etc/display) */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <wallpaper.h>
#include <image.h>
#include <dirent.h>
#include <term.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscall.h>

#define W 676
#define H 648
#define X0 (W_SIDEBAR + SP_7)                     /* content column: padding 20 24 24 */
#define CW (W - X0 - SP_7)
#define TW 132
#define TH 80
#define MAXW 32                                   /* wallpapers listed: the procedural ones plus images found */

static const struct { const char *name; int off; } zones[] = {
    { "Honolulu (UTC-10:00)", -600 }, { "Los Angeles (UTC-08:00)", -480 }, { "Denver (UTC-07:00)", -420 },
    { "Chicago (UTC-06:00)", -360 }, { "New York (UTC-05:00)", -300 }, { "Sao Paulo (UTC-03:00)", -180 },
    { "UTC", 0 }, { "London (UTC+00:00)", 0 }, { "Berlin, Paris (UTC+01:00)", 60 }, { "Cairo (UTC+02:00)", 120 },
    { "Moscow (UTC+03:00)", 180 }, { "Dubai (UTC+04:00)", 240 }, { "Karachi (UTC+05:00)", 300 },
    { "India (UTC+05:30)", 330 }, { "Kathmandu (UTC+05:45)", 345 }, { "Dhaka (UTC+06:00)", 360 },
    { "Bangkok (UTC+07:00)", 420 }, { "Singapore (UTC+08:00)", 480 }, { "Tokyo (UTC+09:00)", 540 },
    { "Sydney (UTC+10:00)", 600 }, { "Auckland (UTC+12:00)", 720 },
};
#define NZONES (int)(sizeof zones / sizeof zones[0])

static int read_file(const char *path, char *buf, int n) {
    int fd = open(path, O_RDONLY), r = -1;
    if (fd >= 0) { r = read(fd, buf, n - 1); close(fd); }
    buf[r > 0 ? r : 0] = 0;
    return r;
}

static bool write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok;
}

/* /dev/display: one "WxH" per line, the current one ends in " *" */
#define MAXMODES 32
static char mode_buf[MAXMODES][16];
static const char *mode_names[MAXMODES];
static int load_modes(int *cur) {
    char buf[512];
    int n = 0;
    read_file("/dev/display", buf, sizeof buf);
    *cur = -1;
    for (char *l = strtok(buf, "\n"); l && n < MAXMODES; l = strtok(0, "\n")) {
        if (strchr(l, '*')) *cur = n;
        l[strcspn(l, " ")] = 0;
        strlcpy(mode_buf[n], l, sizeof mode_buf[n]);
        mode_names[n] = mode_buf[n];
        n++;
    }
    return n;
}

static uint32_t now_ms(void) { struct sysinfo si; sysinfo(&si); return si.uptime_ms; }

#define KEEP_MS 15000

/* wallpaper choices: the procedural themes, then every PNG/JPEG in /usr/share/wallpapers and /home/Pictures */
struct wpe { char label[40], path[120]; int proc; uint32_t *thumb; bool tried; };
static struct wpe wps[MAXW];
static int nwps;

static bool is_img(const char *n) {
    int l = strlen(n);
    return (l > 4 && !strcasecmp(n + l - 4, ".png")) || (l > 4 && !strcasecmp(n + l - 4, ".jpg")) || (l > 5 && !strcasecmp(n + l - 5, ".jpeg"));
}

static void scan(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    int first = nwps;
    for (struct dirent *e; (e = readdir(d)) && nwps < MAXW; ) {
        if (e->d_name[0] == '.' || !is_img(e->d_name)) continue;
        struct wpe *w = &wps[nwps++];
        memset(w, 0, sizeof *w);
        snprintf(w->path, sizeof w->path, "%s/%s", dir, e->d_name);
        strlcpy(w->label, e->d_name, sizeof w->label);
        char *dot = strrchr(w->label, '.');
        if (dot) *dot = 0;
    }
    closedir(d);
    for (int i = first + 1; i < nwps; i++)                       /* insertion sort by name */
        for (int j = i; j > first && strcmp(wps[j].label, wps[j - 1].label) < 0; j--) { struct wpe t = wps[j]; wps[j] = wps[j - 1]; wps[j - 1] = t; }
}

/* thumbnail of an image, covering TW x TH; made when the card first scrolls into view */
static void make_thumb(struct wpe *w) {
    w->tried = true;
    struct image im;
    if (image_load(w->path, &im) < 0) return;
    if ((w->thumb = malloc(TW * TH * 4))) {
        float k = (float)TW / im.w > (float)TH / im.h ? (float)TW / im.w : (float)TH / im.h;
        image_sample(&im, w->thumb, TW, TH, TW, im.w / 2.0f - TW / (2 * k), im.h / 2.0f - TH / (2 * k), k, 1);
        for (int i = 0; i < TW * TH; i++) w->thumb[i] = blend(0xFF000000u, w->thumb[i]);
    }
    image_free(&im);
}

/* a miniature window on the theme's own surfaces (168 x 72) */
static void preview(struct canvas *c, int x, int y, const struct theme *t) {
    int w = 168, h = 72;
    gfx_round_fill(c, x, y, w, h, R_2, t->base);
    gfx_fill(c, x, y + h - 6, w, 6, mix(t->base, t->line_strong, 128));          /* the panel */
    int wx = x + 20, wy = y + 12, ww = w - 40, wh = h - 12 - 14;
    gfx_round_fill(c, wx, wy, ww, wh, 3, t->dark ? t->window : t->raised);         /* the focused window, as it reads */
    gfx_ring_out(c, wx, wy, ww, wh, 3, t->line_strong);
    gfx_round_fill(c, wx + 10, wy + 10, 56, 4, 2, t->line_strong);
    gfx_round_fill(c, wx + 10, wy + 20, 40, 4, 2, t->line_strong);
    gfx_round_fill(c, wx + ww - 8 - 24, wy + wh - 6 - 8, 24, 8, 2, t->accent);
}

/* a theme card: 4px padding around the preview, 2px accent_text ring when selected, the name 8px below */
static bool theme_card(struct ui *ui, int x, int y, const struct theme *t, const char *name, bool on) {
    int w = 176, h = 80;
    bool hov = ui_hover(ui, x, y, w, h + 8 + 18);
    preview(ui->c, x + 4, y + 4, t);
    if (on) gfx_rrect_ring(ui->c, x - 2, y - 2, w + 4, h + 4, RADII(R_3 + 2, R_3 + 2, R_3 + 2, R_3 + 2), 2, ui_th.accent_text);
    else gfx_round_rect(ui->c, x, y, w, h, R_3, hov ? ui_th.line_strong : ui_th.line);
    gfx_text(ui->c, on ? T_BODY_B : T_BODY, x + 4, y + h + 8, name, on ? ui_th.text : ui_th.text2);
    return hov && ui->pressed;
}

static void page_title(struct ui *ui, const char *title, const char *desc) {
    gfx_text(ui->c, T_TITLE, X0, SP_6, title, ui_th.text);
    if (desc) gfx_text(ui->c, T_BODY, X0, SP_6 + T_TITLE->height + SP_2, desc, ui_th.text2);
}
static int heading(struct ui *ui, int y, const char *s) { gfx_text(ui->c, T_HEADING, X0, y, s, ui_th.text); return y + T_HEADING->height + SP_3; }

static const char *const sizes[] = { "11", "13", "15", "18" };   /* terminal font px, see common/termfonts.c */
static char accent_names[THEME_ACCENTS + 1][24];
static const char *accent_items[THEME_ACCENTS + 1];

/* sidebar pages and the words the search field matches */
static const struct { int icon; const char *name, *words; } pages[] = {
    { ICON_PALETTE, "Appearance", "theme dark light style colour color accent contrast animations motion text size terminal font" },
    { ICON_WALLPAPER, "Wallpaper", "background picture image desktop" },
    { ICON_MONITOR, "Display", "resolution screen mode monitor size" },
    { ICON_CLOCK, "Date & Time", "clock time zone timezone utc" },
    { ICON_INFO, "About", "system version memory uptime vibeos" },
};
#define NPAGES 5

static bool matches(int p, const char *q) {
    if (!q[0]) return true;
    char hay[160];
    snprintf(hay, sizeof hay, "%s %s", pages[p].name, pages[p].words);
    for (const char *h = hay; *h; h++) {
        int i = 0;
        while (q[i] && h[i] && (h[i] | 0x20) == (q[i] | 0x20)) i++;
        if (!q[i]) return true;
    }
    return false;
}

static struct ui ui;
static void apply(const char *cmd, const char *fail) {
    if (gui_theme_set(cmd) < 0 || gui_theme_save() < 0) ui_toast(&ui, fail, NULL);
}

int main(void) {
    struct gui_window *w = gui_window_create("Appearance \xe2\x80\x94 Settings", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("settings"); return 1; }
    ui_init(&ui, w);

    char buf[32], query[48] = "";
    int tz = 0, zsel = -1, zscroll = 0, page = 0, shown = -1;
    char wpcur[128];                                     /* /etc/wallpaper: a procedural name or an image path */
    read_file("/etc/wallpaper", wpcur, sizeof wpcur);
    wpcur[strcspn(wpcur, "\n")] = 0;
    if (!wpcur[0]) strlcpy(wpcur, wallpaper_name(0), sizeof wpcur);
    if (read_file("/etc/tz", buf, sizeof buf) > 0) tz = atoi(buf);
    for (int i = 0; i < NZONES && zsel < 0; i++) if (zones[i].off == tz) zsel = i;
    zscroll = zsel > 3 ? zsel - 3 : 0;
    accent_items[0] = "Theme default";
    for (int i = 0; i < THEME_ACCENTS; i++) {
        strlcpy(accent_names[i], theme_accents[i].name, sizeof accent_names[i]);
        accent_names[i][0] &= ~0x20;
        accent_items[i + 1] = accent_names[i];
    }

    for (int i = 0; i < wallpaper_count() && nwps < MAXW; i++) {      /* real renders of each procedural wallpaper, made once */
        struct wpe *e = &wps[nwps++];
        memset(e, 0, sizeof *e);
        strlcpy(e->label, wallpaper_name(i), sizeof e->label);
        strlcpy(e->path, wallpaper_name(i), sizeof e->path);
        e->label[0] &= ~0x20;
        e->proc = 1; e->tried = true;
        if (!(e->thumb = malloc(TW * TH * 4))) continue;
        struct canvas tc;
        canvas_init(&tc, e->thumb, TW, TH, TW);
        wallpaper_render(&tc, i);
    }
    scan("/usr/share/wallpapers");
    scan("/home/Pictures");
    int wscroll = 0;
    struct theme_state ts;
    gui_theme_state(&ts);
    const char *names[NZONES];
    for (int i = 0; i < NZONES; i++) names[i] = zones[i].name;
    int mcur, msel, mscroll = 0, nmodes = load_modes(&mcur);
    char prev[16] = "";                                  /* set while asking to keep a new mode */
    uint32_t keep_until = 0;
    msel = mcur;
    ui.focus = ui_id(SP_3, SP_3, W_SIDEBAR - 2 * SP_3, H_SEARCH);    /* type to search right away */

    do {
        struct canvas *c = ui.c;
        if (ui.themed || ui.pressed || ui.nkeys) gui_theme_state(&ts);
        ui_clear(&ui);
        /* ---- sidebar: search, then the pages that match it ---- */
        ui_sidebar(&ui, 0, 0, W_SIDEBAR, H);
        ui_search(&ui, SP_3, SP_3, W_SIDEBAR - 2 * SP_3, query, sizeof query, "Search settings");
        int ry = SP_3 + H_SEARCH + SP_4, nm = 0, firstm = -1;
        for (int i = 0; i < NPAGES; i++) if (matches(i, query)) { nm++; if (firstm < 0) firstm = i; }
        if (query[0]) {
            char r[24];
            snprintf(r, sizeof r, nm == 1 ? "1 result" : "%d results", nm);
            gfx_text(c, T_CAPTION, SP_5, ry, nm ? r : "No results", ui_th.text2);
            ry += T_CAPTION->height + SP_2;
            if (nm && !matches(page, query)) page = firstm;
        }
        for (int i = 0; i < NPAGES; i++) {
            if (!matches(i, query)) continue;
            if (ui_side_item(&ui, 0, ry, W_SIDEBAR, pages[i].icon, pages[i].name, NULL, i == page) && !prev[0]) page = i;
            ry += H_ROW;
        }
        if (page != shown) {
            char t[64];
            snprintf(t, sizeof t, "%s \xe2\x80\x94 Settings", pages[page].name);
            gui_set_title(w, t);
            shown = page;
        }

        if (page == 0) {
            page_title(&ui, "Appearance", "Applies at once to the desktop, the panel and every app.");
            int y = heading(&ui, SP_6 + T_TITLE->height + SP_2 + T_BODY->height + SP_6, "Style");
            bool hc = !strcmp(theme_get(ts.index)->name, "contrast");
            for (int i = 0; i < 2; i++) {
                const struct theme *t = theme_get(i);
                if (theme_card(&ui, X0 + i * (176 + SP_5), y, t, t->label, ts.index == i || (hc && i == 0)) && ts.index != i)
                    apply(t->name, "Couldn't change the theme");
            }
            y = heading(&ui, y + 80 + SP_3 + T_BODY->height + SP_7, "Windows and text");
            int ch = 3 * H_ROW2 + 2;
            ui_card(&ui, X0, y, CW, ch);
            int sx = X0 + CW - SP_4 - SWITCH_W;
            ui_card_row(&ui, X0, y, CW, H_ROW2, "Window animations", "Fade and scale windows as they open and close", true);
            bool anim = ts.anim;
            if (ui_switch(&ui, sx, y + (H_ROW2 - SWITCH_H) / 2, &anim, 0)) apply(anim ? "anim=1" : "anim=0", "Couldn't save the setting");
            ui_card_row(&ui, X0, y + H_ROW2, CW, H_ROW2 + 1, "High contrast", "Black and white surfaces with strong edges", false);
            bool hcv = hc;
            if (ui_switch(&ui, sx, y + H_ROW2 + 1 + (H_ROW2 - SWITCH_H) / 2, &hcv, 0)) apply(hcv ? "contrast" : "graphite", "Couldn't change the theme");
            int ty = y + 2 * H_ROW2 + 2;
            ui_card_row(&ui, X0, ty - 1, CW, H_ROW2 + 1, "Terminal text size", NULL, false);
            int zsz = ts.zoom, slx = X0 + CW - SP_4 - 40 - SP_4 - 200;
            if (ui_slider(&ui, slx, ty + (H_ROW2 - HIT_MIN) / 2, 200, &zsz, 0, TERM_ZOOMS - 1) && zsz != (int)ts.zoom) {
                char cmd[16];
                snprintf(cmd, sizeof cmd, "zoom=%d", zsz);
                apply(cmd, "Couldn't save the setting");
            }
            char px[16];
            snprintf(px, sizeof px, "%s px", sizes[ts.zoom < TERM_ZOOMS ? ts.zoom : 1]);
            gfx_text(c, T_BODY, X0 + CW - SP_4 - gfx_text_width(T_BODY, px), ty + (H_ROW2 - T_BODY->height) / 2, px, ui_th.text);
            y = heading(&ui, y + ch + SP_7, "Accent");
            ui_card(&ui, X0, y, CW, H_ROW2);
            ui_card_row(&ui, X0, y, CW, H_ROW2, "Accent colour", NULL, true);
            int asel = 0;
            for (int i = 0; i < THEME_ACCENTS; i++) if (ts.accent == theme_accents[i].rgb) asel = i + 1;
            int a0 = asel;
            if (ui_dropdown(&ui, X0 + CW - SP_4 - 200, y + (H_ROW2 - H_CTL) / 2, 200, accent_items, THEME_ACCENTS + 1, &asel, 0) && asel != a0) {
                char cmd[40];
                snprintf(cmd, sizeof cmd, "accent=%s", asel ? theme_accents[asel - 1].name : "default");
                apply(cmd, "Couldn't change the accent");
            }
            int rw = ui_button_w(UI_NOICON, "Reset appearance");
            if (ui_button(&ui, W - SP_7 - rw, H - SP_7 - H_CTL, rw, H_CTL, "Reset appearance", UI_SECONDARY)) {
                apply("graphite accent=default anim=1 zoom=1", "Couldn't reset the appearance");
                ui_toast(&ui, "Appearance reset to the defaults", NULL);
            }
        } else if (page == 1) {
            page_title(&ui, "Wallpaper", "A generated backdrop, or any PNG or JPEG picture.");
            int top = SP_6 + T_TITLE->height + SP_2 + T_BODY->height + SP_6, viewh = H - top, rows = (nwps + 2) / 3, pitch = TH + 4 + SP_3 + T_BODY->height + SP_5;
            int maxs = rows * pitch - viewh + SP_5;
            if (maxs < 0) maxs = 0;
            if (ui_hover(&ui, W_SIDEBAR, top, W - W_SIDEBAR, viewh) && ui.scroll) wscroll += ui.scroll * 40;
            if (wscroll > maxs) wscroll = maxs;
            if (wscroll < 0) wscroll = 0;
            struct canvas cc = *c;
            canvas_clip(&cc, W_SIDEBAR, top - 4, W - W_SIDEBAR, viewh + 4);
            int gap = (CW - 3 * TW) / 2;
            for (int i = 0; i < nwps; i++) {
                struct wpe *e = &wps[i];
                int tx = X0 + (i % 3) * (TW + gap), ty = top + (i / 3) * pitch - wscroll;
                if (ty + pitch < top - 4 || ty > H) continue;
                if (!e->tried) make_thumb(e);
                bool cur = !strcmp(e->path, wpcur), vis = ty >= top - 4 && ty + TH <= H, hov = vis && ui_hover(&ui, tx, ty, TW, TH);
                if (e->thumb) gfx_blit(&cc, tx, ty, e->thumb, TW, TH, TW);
                else { gfx_round_fill(&cc, tx, ty, TW, TH, R_2, ui_th.raised); gfx_text_center(&cc, T_CAPTION, tx, ty + TH / 2 - 7, TW, "Can't read it", ui_th.text2); }
                if (cur) gfx_rrect_ring(&cc, tx - 3, ty - 3, TW + 6, TH + 6, RADII(R_2 + 3, R_2 + 3, R_2 + 3, R_2 + 3), 2, ui_th.accent_text);
                else gfx_round_rect(&cc, tx, ty, TW, TH, 0, hov ? ui_th.line_strong : ui_th.line);
                struct canvas lc = cc;
                canvas_clip(&lc, tx, ty + TH, TW, 40);
                struct canvas *o = ui.c;
                ui.c = &lc;
                ui_text_fit(&ui, cur ? T_BODY_B : T_BODY, tx, ty + TH + 4 + SP_3, TW, e->label, cur ? ui_th.text : ui_th.text2);
                ui.c = o;
                if (vis && ui_clicked(&ui, tx, ty, TW, TH) && !cur) {
                    if (write_file("/etc/wallpaper", e->path)) { strlcpy(wpcur, e->path, sizeof wpcur); win_set(w->fd, WIN_SET_PREFS, 0); }
                    else ui_toast(&ui, "Couldn't write /etc/wallpaper", NULL);
                }
            }
            ui_vscroll(&ui, W - 12, top, viewh, rows * pitch + SP_5, viewh, &wscroll);
        } else if (page == 2) {
            if (!prev[0]) nmodes = load_modes(&mcur);       /* `display WxH` may have switched it meanwhile */
            char cur[48];
            snprintf(cur, sizeof cur, "The screen is %s now.", mcur >= 0 ? mode_names[mcur] : "at an unknown resolution");
            page_title(&ui, "Display", mcur >= 0 ? cur : "No display adapter that can change modes.");
            int y = heading(&ui, SP_6 + T_TITLE->height + SP_2 + T_BODY->height + SP_6, "Resolution");
            int lh = H - y - SP_7 - H_CTL - SP_6;
            lh -= lh % H_ROW;
            ui_card(&ui, X0, y, CW, lh + 2 * SP_2);
            int dummy = msel;
            ui_list(&ui, X0, y + SP_2, CW, lh, mode_names, nmodes, prev[0] ? &dummy : &msel, &mscroll);
            bool can = msel >= 0 && msel != mcur && !prev[0];
            int bw = ui_button_w(UI_NOICON, "Apply resolution");
            if (ui_button(&ui, W - SP_7 - bw, H - SP_7 - H_CTL, bw, H_CTL, "Apply resolution", UI_PRIMARY | (can ? 0 : UI_DISABLED))) {
                char old[16];
                strlcpy(old, mcur >= 0 ? mode_names[mcur] : "", sizeof old);
                if (write_file("/dev/display", mode_names[msel])) {
                    nmodes = load_modes(&mcur);
                    strlcpy(prev, old, sizeof prev); keep_until = now_ms() + KEEP_MS;
                } else {
                    char m[64];
                    snprintf(m, sizeof m, "This display can't show %s", mode_names[msel]);
                    ui_toast(&ui, m, NULL);
                }
            }
        } else if (page == 3) {
            time_t now = time(NULL) + tz * 60;
            struct tm tm;
            gmtime_r(&now, &tm);
            char t[32], d[64];
            strftime(t, sizeof t, "%H:%M:%S", &tm);
            strftime(d, sizeof d, "%A %e %B %Y", &tm);
            page_title(&ui, "Date & Time", NULL);
            int y = SP_6 + T_TITLE->height + SP_3;
            gfx_text(c, T_DISPLAY, X0, y, t, ui_th.text);
            gfx_text(c, T_BODY, X0, y + T_DISPLAY->height, d, ui_th.text2);
            y = heading(&ui, y + T_DISPLAY->height + T_BODY->height + SP_7, "Time zone");
            int lh = H - y - SP_7 - T_CAPTION->height - SP_3 - 2 * SP_2;
            lh -= lh % H_ROW;
            ui_card(&ui, X0, y, CW, lh + 2 * SP_2);
            int pz = zsel;
            ui_list(&ui, X0, y + SP_2, CW, lh, names, NZONES, &zsel, &zscroll);
            if (zsel != pz && zsel >= 0) {
                snprintf(buf, sizeof buf, "%d", zones[zsel].off);
                if (write_file("/etc/tz", buf)) { tz = zones[zsel].off; win_set(w->fd, WIN_SET_PREFS, 0); }
                else ui_toast(&ui, "Couldn't write /etc/tz", NULL);
            }
            gfx_text(c, T_CAPTION, X0, y + lh + 2 * SP_2 + SP_3, "No daylight saving: pick the offset that's in effect now.", ui_th.text2);
        } else {
            struct sysinfo si;
            sysinfo(&si);
            page_title(&ui, "About", "Bootloader, kernel, desktop, libc and apps, written from scratch.");
            int y = heading(&ui, SP_6 + T_TITLE->height + SP_2 + T_BODY->height + SP_6, "This computer");
            read_file("/proc/version", buf, sizeof buf);
            buf[strcspn(buf, "\n")] = 0;
            const char *k[] = { "System", "Memory", "Display", "Uptime" };
            char v[4][64];
            snprintf(v[0], 64, "%s", buf);
            snprintf(v[1], 64, "%u MiB, %u MiB free", si.mem_total_kb / 1024, si.mem_free_kb / 1024);
            snprintf(v[2], 64, "%u \xc3\x97 %u", si.screen_w, si.screen_h);
            snprintf(v[3], 64, "%u min", si.uptime_ms / 60000);
            ui_card(&ui, X0, y, CW, 4 * H_ROW2 + 3);
            for (int i = 0; i < 4; i++) {
                int yy = y + i * (H_ROW2 + 1) - (i ? 1 : 0);
                ui_card_row(&ui, X0, yy, CW, H_ROW2 + (i ? 1 : 0), k[i], NULL, i == 0);
                int vw = gfx_text_width(T_BODY, v[i]);
                ui_text_fit(&ui, T_BODY, X0 + CW - SP_4 - (vw < CW - 140 ? vw : CW - 140), yy + (H_ROW2 + (i ? 1 : 0) - T_BODY->height) / 2, CW - 140, v[i], ui_th.text2);
            }
        }
        if (prev[0]) {                                /* "Keep this resolution?", reverts on its own */
            int left = (int)(keep_until - now_ms());
            char msg[64];
            snprintf(msg, sizeof msg, "The display goes back to %s in %d s.", prev, left > 0 ? (left + 999) / 1000 : 0);
            int r = ui_dialog(&ui, "Keep this resolution?", msg, "Revert", "Keep changes", UI_PRIMARY);
            if (left <= 0) r = -1;
            snprintf(buf, sizeof buf, "%s\n", mode_names[mcur]);
            if (r > 0 && !write_file("/etc/display", buf)) ui_toast(&ui, "Couldn't write /etc/display", NULL);
            if (r < 0) { write_file("/dev/display", prev); nmodes = load_modes(&mcur); msel = mcur; }
            if (r) prev[0] = 0;
        }
        ui_end(&ui);
    } while (ui_poll(&ui, page == 3 || prev[0] ? 1000 : -1));
    gui_window_destroy(w);
    return 0;
}
