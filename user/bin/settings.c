/* settings - theme, accent, wallpaper, display mode and time zone. Themes go through /dev/theme (and /etc/theme for the
 * next boot); the desktop reloads wallpaper/time zone prefs via WIN_SET_PREFS; modes go through /dev/display (and /etc/display) */
#include <ui.h>
#include <design.h>
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

#define W 760
#define H 500
#define SIDE 180
#define TW 160
#define TH 100
#define MAXW 32                         /* wallpapers listed: the procedural ones plus images found */

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

/* a miniature desktop in theme t: window, button, taskbar */
static void preview(struct canvas *c, int x, int y, int w, int h, const struct theme *t) {
    gfx_round_fill(c, x, y, w, h, 8, mix(t->bg, t->accent, 24));
    int wx = x + 12, wy = y + 10, ww = w - 44, wh = h - 32;
    gfx_round_fill(c, x + w - 62, y + 26, 50, 38, 5, t->surface);                  /* a second window behind */
    gfx_round_fill(c, wx, wy, ww, wh, 5, t->frame);
    gfx_fill(c, wx + 1, wy + 13, ww - 2, wh - 14, t->panel);
    for (int i = 0; i < 3; i++) gfx_circle_fill(c, wx + 7 + i * 8, wy + 6, 2, i == 0 ? RGB(0xFF, 0x5F, 0x7A) : i == 1 ? RGB(0xFF, 0xC2, 0x4B) : RGB(0x4B, 0xE0, 0x8A));
    gfx_fill(c, wx + 8, wy + 20, ww * 6 / 10, 3, t->text);
    gfx_fill(c, wx + 8, wy + 27, ww * 4 / 10, 3, t->dim);
    gfx_fill(c, wx + 8, wy + 34, ww / 2, 3, t->dim);
    gfx_round_fill(c, wx + 8, wy + wh - 16, 30, 9, 3, t->accent);
    gfx_round_fill(c, x + 6, y + h - 13, w - 12, 8, 4, t->bar | 0xFF000000u);
    gfx_round_fill(c, x + 10, y + h - 11, 18, 4, 2, t->accent);
}

static const char *accent_label(uint32_t rgb) {
    if (!rgb) return "theme default";
    for (int i = 0; i < THEME_ACCENTS; i++) if (theme_accents[i].rgb == rgb) return theme_accents[i].name;
    return "custom";
}

static const char *const sizes[] = { "11", "13", "15", "18" };   /* terminal font px, see common/termfonts.c */

int main(void) {
    struct gui_window *w = gui_window_create("Settings", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("settings"); return 1; }
    struct ui ui;
    ui_init(&ui, w);

    char buf[32];
    int tz = 0, zsel = -1, zscroll = 0, page = 0;
    char wpcur[128];                                     /* /etc/wallpaper: a procedural name or an image path */
    read_file("/etc/wallpaper", wpcur, sizeof wpcur);
    wpcur[strcspn(wpcur, "\n")] = 0;
    if (!wpcur[0]) strlcpy(wpcur, wallpaper_name(0), sizeof wpcur);
    if (read_file("/etc/tz", buf, sizeof buf) > 0) tz = atoi(buf);
    for (int i = 0; i < NZONES && zsel < 0; i++) if (zones[i].off == tz) zsel = i;
    zscroll = zsel > 3 ? zsel - 3 : 0;

    for (int i = 0; i < wallpaper_count() && nwps < MAXW; i++) {      /* real renders of each procedural wallpaper, made once */
        struct wpe *e = &wps[nwps++];
        memset(e, 0, sizeof *e);
        strlcpy(e->label, wallpaper_name(i), sizeof e->label);
        strlcpy(e->path, wallpaper_name(i), sizeof e->path);
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
    char status[96] = "";
    int mcur, msel, mscroll = 0, nmodes = load_modes(&mcur);
    char prev[16] = "";                                  /* set while asking to keep a new mode */
    uint32_t keep_until = 0;
    msel = mcur;

    do {
        struct canvas *c = ui.c;
        ui_clear(&ui);
        gfx_fill(c, 0, 0, SIDE, H, UI_PANEL);
        gfx_fill(c, SIDE, 0, 1, H, UI_BORDER);
        gfx_text(c, &font_title, 20, 18, "Settings", UI_TEXT);
        static const char *const pages[] = { "Appearance", "Wallpaper", "Display", "Date & Time", "About" };
        for (int i = 0; i < 5; i++) {
            int y = 64 + i * 42;
            if (i == page) gfx_round_fill(c, 10, y, SIDE - 20, 36, UI_RADIUS, UI_ACC(70));
            else if (ui_hover(&ui, 10, y, SIDE - 20, 36)) gfx_round_fill(c, 10, y, SIDE - 20, 36, UI_RADIUS, UI_SURFACE);
            gfx_text(c, i == page ? &font_ui_bold : &font_ui, 24, y + (36 - font_ui.height) / 2, pages[i], UI_TEXT);
            if (ui_clicked(&ui, 10, y, SIDE - 20, 36) && !prev[0]) { page = i; status[0] = 0; }
        }
        int x0 = SIDE + 28;

        if (page == 0) {
            gfx_text(c, &font_title, x0, 18, "Appearance", UI_TEXT);
            gfx_text(c, &font_ui, x0, 52, "Applies at once to the desktop, terminals and apps.", UI_TEXT_DIM);
            gui_theme_state(&ts);
            int cw = (W - x0 - 28 - 2 * 16) / 3, ch = 90;
            for (int i = 0; i < theme_count(); i++) {
                int tx = x0 + (i % 3) * (cw + 16), ty = 84 + (i / 3) * (ch + 36);
                const struct theme *t = theme_get(i);
                bool hov = ui_hover(&ui, tx, ty, cw, ch);
                preview(c, tx, ty, cw, ch, t);
                if (i == ts.index) { gfx_round_rect(c, tx - 3, ty - 3, cw + 6, ch + 6, 10, UI_ACCENT); gfx_round_rect(c, tx - 4, ty - 4, cw + 8, ch + 8, 11, UI_ACCENT); }
                else gfx_round_rect(c, tx - 1, ty - 1, cw + 2, ch + 2, 9, hov ? UI_OV(90) : UI_BORDER);
                gfx_text_center(c, i == ts.index ? &font_ui_bold : &font_ui, tx, ty + ch + 8, cw, t->label, i == ts.index ? UI_TEXT : UI_TEXT_DIM);
                if (ui_clicked(&ui, tx, ty, cw, ch) && i != ts.index) {
                    if (gui_theme_set(t->name) < 0 || gui_theme_save() < 0) snprintf(status, sizeof status, "Couldn't change the theme");
                    else status[0] = 0;
                }
            }
            int ay = 84 + (theme_count() + 2) / 3 * (ch + 36) + 4;
            gfx_text(c, &font_ui_bold, x0, ay, "Accent colour", UI_TEXT_DIM);
            for (int i = 0; i <= THEME_ACCENTS; i++) {
                int sx = x0 + i * 38, sy = ay + 26;
                color_t col = i ? theme_accents[i - 1].rgb : theme_get(ts.index)->accent;
                bool on = i ? ts.accent == theme_accents[i - 1].rgb : !ts.accent, hov = ui_hover(&ui, sx, sy, 28, 28);
                if (on || hov) gfx_circle_fill(c, sx + 14, sy + 14, 16, on ? UI_TEXT : UI_OV(70));
                gfx_circle_fill(c, sx + 14, sy + 14, on || hov ? 13 : 14, UI_BG);
                gfx_circle_fill(c, sx + 14, sy + 14, 11, col);
                if (!i) gfx_text_center(c, &font_ui_bold, sx, sy + 14 - font_ui_bold.height / 2, 28, "A", ts.accent ? UI_TEXT : theme_get(ts.index)->on_accent);
                if (ui_clicked(&ui, sx, sy, 28, 28) && !on) {
                    char cmd[40];
                    snprintf(cmd, sizeof cmd, "accent=%s", i ? theme_accents[i - 1].name : "default");
                    if (gui_theme_set(cmd) < 0 || gui_theme_save() < 0) snprintf(status, sizeof status, "Couldn't change the accent");
                    else status[0] = 0;
                }
            }
            gfx_text(c, &font_ui, x0 + (THEME_ACCENTS + 1) * 38 + 4, ay + 26 + 6, accent_label(ts.accent), UI_TEXT_DIM);
            int oy = ay + 74;
            bool anim = ts.anim;
            if (ui_checkbox(&ui, x0, oy + 6, "Window animations", &anim)) {
                if (gui_theme_set(anim ? "anim=1" : "anim=0") < 0 || gui_theme_save() < 0) snprintf(status, sizeof status, "Couldn't save the setting");
            }
            gfx_text(c, &font_ui, x0 + 214, oy + 8, "Terminal text (px)", UI_TEXT_DIM);
            int zsz = ts.zoom;
            if (ui_tabs(&ui, x0 + 350, oy, W - x0 - 28 - 350, 30, sizes, TERM_ZOOMS, &zsz)) {
                char cmd[16];
                snprintf(cmd, sizeof cmd, "zoom=%d", zsz);
                if (gui_theme_set(cmd) < 0 || gui_theme_save() < 0) snprintf(status, sizeof status, "Couldn't save the setting");
            }
        } else if (page == 1) {
            gfx_text(c, &font_title, x0, 18, "Wallpaper", UI_TEXT);
            gfx_text(c, &font_ui, x0, 52, "A generated backdrop, or any PNG/JPEG picture.", UI_TEXT_DIM);
            int top = 84, viewh = H - top - 8, rows = (nwps + 2) / 3, pitch = TH + 52;
            int maxs = rows * pitch - viewh;
            if (maxs < 0) maxs = 0;
            if (ui_hover(&ui, SIDE, top, W - SIDE, viewh) && ui.scroll) wscroll += ui.scroll * 40;
            if (wscroll > maxs) wscroll = maxs;
            if (wscroll < 0) wscroll = 0;
            struct canvas cc = *c;
            canvas_clip(&cc, SIDE + 1, top - 6, W - SIDE - 1, viewh + 6);
            for (int i = 0; i < nwps; i++) {
                struct wpe *e = &wps[i];
                int tx = x0 + (i % 3) * (TW + 18), ty = top + (i / 3) * pitch - wscroll;
                if (ty + TH + 30 < top - 6 || ty > H) continue;
                if (!e->tried) make_thumb(e);
                bool cur = !strcmp(e->path, wpcur), vis = ty >= top - 6 && ty + TH <= H - 6, hov = vis && ui_hover(&ui, tx, ty, TW, TH);
                if (cur) gfx_round_rect(&cc, tx - 4, ty - 4, TW + 8, TH + 8, 10, UI_ACCENT);
                else if (hov) gfx_round_rect(&cc, tx - 4, ty - 4, TW + 8, TH + 8, 10, UI_OV(60));
                if (e->thumb) gfx_blit(&cc, tx, ty, e->thumb, TW, TH, TW);
                else { gfx_round_fill(&cc, tx, ty, TW, TH, 6, UI_SURFACE); gfx_text_center(&cc, &font_ui, tx, ty + TH / 2 - 8, TW, "can't read", UI_TEXT_DIM); }
                struct canvas lc = cc;
                canvas_clip(&lc, tx - 8, ty + TH, TW + 16, 30);
                gfx_text_center(&lc, cur ? &font_ui_bold : &font_ui, tx - 8, ty + TH + 10, TW + 16, e->label, cur ? UI_TEXT : UI_TEXT_DIM);
                if (vis && ui_clicked(&ui, tx, ty, TW, TH) && !cur) {
                    if (write_file("/etc/wallpaper", e->path)) { strlcpy(wpcur, e->path, sizeof wpcur); win_set(w->fd, WIN_SET_PREFS, 0); status[0] = 0; }
                    else snprintf(status, sizeof status, "Couldn't write /etc/wallpaper");
                }
            }
            ui_vscroll(&ui, W - 14, top, viewh, rows * pitch, viewh, &wscroll);
        } else if (page == 2) {
            if (!prev[0]) nmodes = load_modes(&mcur);       /* `display WxH` may have switched it meanwhile */
            gfx_text(c, &font_title, x0, 18, "Display", UI_TEXT);
            char cur[48];
            snprintf(cur, sizeof cur, "Resolution: %s", mcur >= 0 ? mode_names[mcur] : "unknown");
            gfx_text(c, &font_ui, x0, 52, cur, UI_TEXT_DIM);
            int lh = H - 88 - 64;
            int dummy = msel;
            ui_list(&ui, x0, 88, W - x0 - 28, lh, mode_names, nmodes, prev[0] ? &dummy : &msel, &mscroll);
            bool can = msel >= 0 && msel != mcur && !prev[0];
            if (ui_button(&ui, W - 28 - 110, H - 52, 110, 34, "Apply", can ? UI_ACCENT_BTN : UI_NORMAL_BTN) && can) {
                char old[16];
                strlcpy(old, mcur >= 0 ? mode_names[mcur] : "", sizeof old);
                if (write_file("/dev/display", mode_names[msel])) {
                    nmodes = load_modes(&mcur);
                    strlcpy(prev, old, sizeof prev); keep_until = now_ms() + KEEP_MS;
                    status[0] = 0;
                } else snprintf(status, sizeof status, "%s isn't supported by this display", mode_names[msel]);
            }
            if (mcur < 0) snprintf(status, sizeof status, "No display adapter that can change modes");
        } else if (page == 3) {
            time_t now = time(NULL) + tz * 60;
            struct tm tm;
            gmtime_r(&now, &tm);
            char t[32], d[64];
            strftime(t, sizeof t, "%H:%M:%S", &tm);
            strftime(d, sizeof d, "%A, %e %B %Y", &tm);
            gfx_text(c, &font_title, x0, 18, "Date & Time", UI_TEXT);
            gfx_text(c, &font_big, x0, 52, t, UI_TEXT);
            gfx_text(c, &font_ui, x0, 52 + font_big.height + 2, d, UI_TEXT_DIM);
            int ly = 52 + font_big.height + 2 + font_ui.height + 44;
            gfx_text(c, &font_ui_bold, x0, ly - 26, "Time zone", UI_TEXT_DIM);
            int prev = zsel;
            ui_list(&ui, x0, ly, W - x0 - 28, H - ly - 50, names, NZONES, &zsel, &zscroll);
            if (zsel != prev && zsel >= 0) {
                snprintf(buf, sizeof buf, "%d", zones[zsel].off);
                if (write_file("/etc/tz", buf)) { tz = zones[zsel].off; win_set(w->fd, WIN_SET_PREFS, 0); }
                else snprintf(status, sizeof status, "Couldn't write /etc/tz");
            }
            gfx_text(c, &font_ui, x0, H - 36, "No daylight saving: pick the offset that's in effect now.", UI_TEXT_DIM);
        } else {
            struct sysinfo si;
            sysinfo(&si);
            gfx_text(c, &font_title, x0, 18, "About", UI_TEXT);
            read_file("/proc/version", buf, sizeof buf);
            buf[strcspn(buf, "\n")] = 0;
            char line[96];
            const char *k[] = { "System", "Memory", "Display", "Uptime" };
            char v[4][64];
            snprintf(v[0], 64, "%s", buf);
            snprintf(v[1], 64, "%u MiB, %u MiB free", si.mem_total_kb / 1024, si.mem_free_kb / 1024);
            snprintf(v[2], 64, "%ux%u", si.screen_w, si.screen_h);
            snprintf(v[3], 64, "%u min", si.uptime_ms / 60000);
            ui_panel(&ui, x0, 64, W - x0 - 28, 4 * 30 + 24);
            for (int i = 0; i < 4; i++) {
                gfx_text(c, &font_ui_bold, x0 + 18, 76 + i * 30, k[i], UI_TEXT_DIM);
                ui_text_fit(&ui, &font_ui, x0 + 120, 76 + i * 30, W - x0 - 180, v[i], UI_TEXT);
            }
            snprintf(line, sizeof line, "Bootloader, kernel, desktop, libc and apps written from scratch.");
            gfx_text(c, &font_ui, x0, 230, line, UI_TEXT_DIM);
        }
        if (status[0]) gfx_text(c, &font_ui, x0, H - 36, status, UI_BAD);
        if (prev[0]) {                                /* "Keep this resolution?", reverts on its own */
            int left = (int)(keep_until - now_ms());
            /* an alert (docs/DESIGN.md §9 Dialogs): 400 wide, 20 inset, title, body, primary rightmost */
            int pw = W_ALERT, ph = 2 * SP_6 + T_HEADING->height + SP_3 + T_BODY->height + SP_6 + H_CTL, px = (W - pw) / 2, py = (H - ph) / 2;
            gfx_fill(c, 0, 0, W, H, ui_th.scrim);
            gfx_elev(c, px, py, pw, ph, R_3, E4, ui_th.elev[E4]);
            gfx_round_fill(c, px, py, pw, ph, R_3, ui_th.overlay);
            gfx_round_rect(c, px, py, pw, ph, R_3, ui_th.line);
            gfx_text(c, T_HEADING, px + SP_6, py + SP_6, "Keep this resolution?", ui_th.text);
            char msg[64];
            snprintf(msg, sizeof msg, "It goes back to %s in %d s unless you keep it.", prev, left > 0 ? (left + 999) / 1000 : 0);
            gfx_text(c, T_BODY, px + SP_6, py + SP_6 + T_HEADING->height + SP_3, msg, ui_th.text2);
            int r = 0, bw1 = gfx_text_width(T_BODY_B, "Keep changes") + 2 * SP_4, bw0 = gfx_text_width(T_BODY_B, "Revert") + 2 * SP_4;
            if (bw0 < W_CTL_MIN) bw0 = W_CTL_MIN;
            int by = py + ph - SP_6 - H_CTL, bx1 = px + pw - SP_6 - bw1;
            if (ui_button(&ui, bx1, by, bw1, H_CTL, "Keep changes", UI_ACCENT_BTN)) r = 1;
            if (ui_button(&ui, bx1 - SP_3 - bw0, by, bw0, H_CTL, "Revert", UI_NORMAL_BTN) || left <= 0) r = -1;
            struct gui_event k;
            while (ui_key(&ui, &k)) { if (k.key == KEY_ESC) r = -1; else if (k.key == KEY_ENTER) r = 1; }
            snprintf(buf, sizeof buf, "%s\n", mode_names[mcur]);
            if (r > 0 && !write_file("/etc/display", buf)) snprintf(status, sizeof status, "Couldn't write /etc/display");
            if (r < 0) { write_file("/dev/display", prev); nmodes = load_modes(&mcur); msel = mcur; }
            if (r) prev[0] = 0;
        }
        ui_end(&ui);
    } while (ui_poll(&ui, page == 3 || prev[0] ? 1000 : -1));
    gui_window_destroy(w);
    return 0;
}
