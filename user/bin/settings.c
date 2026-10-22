/* settings - wallpaper and time zone; the desktop reloads them via WIN_SET_PREFS */
#include <ui.h>
#include <wallpaper.h>
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

int main(void) {
    struct gui_window *w = gui_window_create("Settings", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("settings"); return 1; }
    struct ui ui;
    ui_init(&ui, w);

    char buf[32];
    int theme = 0, tz = 0, zsel = -1, zscroll = 0, page = 0;
    read_file("/etc/wallpaper", buf, sizeof buf);
    for (int i = 0; i < wallpaper_count(); i++) if (!strncmp(buf, wallpaper_name(i), strlen(wallpaper_name(i)))) theme = i;
    if (read_file("/etc/tz", buf, sizeof buf) > 0) tz = atoi(buf);
    for (int i = 0; i < NZONES && zsel < 0; i++) if (zones[i].off == tz) zsel = i;
    zscroll = zsel > 3 ? zsel - 3 : 0;

    uint32_t *thumb[8] = { 0 };                          /* real renders of each theme, made once */
    for (int i = 0; i < wallpaper_count() && i < 8; i++) {
        if (!(thumb[i] = malloc(TW * TH * 4))) continue;
        struct canvas tc;
        canvas_init(&tc, thumb[i], TW, TH, TW);
        wallpaper_render(&tc, i);
    }
    const char *names[NZONES];
    for (int i = 0; i < NZONES; i++) names[i] = zones[i].name;
    char status[96] = "";

    do {
        struct canvas *c = ui.c;
        ui_clear(&ui);
        gfx_fill(c, 0, 0, SIDE, H, UI_PANEL);
        gfx_fill(c, SIDE, 0, 1, H, UI_BORDER);
        gfx_text(c, &font_title, 20, 18, "Settings", UI_TEXT);
        static const char *const pages[] = { "Appearance", "Date & Time", "About" };
        for (int i = 0; i < 3; i++) {
            int y = 64 + i * 42;
            if (i == page) gfx_round_fill(c, 10, y, SIDE - 20, 36, UI_RADIUS, RGBA(0xB3, 0x88, 0xFF, 70));
            else if (ui_hover(&ui, 10, y, SIDE - 20, 36)) gfx_round_fill(c, 10, y, SIDE - 20, 36, UI_RADIUS, UI_SURFACE);
            gfx_text(c, i == page ? &font_ui_bold : &font_ui, 24, y + (36 - font_ui.height) / 2, pages[i], UI_TEXT);
            if (ui_clicked(&ui, 10, y, SIDE - 20, 36)) { page = i; status[0] = 0; }
        }
        int x0 = SIDE + 28;

        if (page == 0) {
            gfx_text(c, &font_title, x0, 18, "Wallpaper", UI_TEXT);
            gfx_text(c, &font_ui, x0, 52, "Procedural aurora themes, rendered live.", UI_TEXT_DIM);
            for (int i = 0; i < wallpaper_count() && i < 8; i++) {
                int tx = x0 + (i % 3) * (TW + 18), ty = 88 + (i / 3) * (TH + 52);
                bool hov = ui_hover(&ui, tx, ty, TW, TH);
                if (i == theme) gfx_round_rect(c, tx - 4, ty - 4, TW + 8, TH + 8, 10, UI_ACCENT);
                else if (hov) gfx_round_rect(c, tx - 4, ty - 4, TW + 8, TH + 8, 10, RGBA(255, 255, 255, 60));
                if (thumb[i]) gfx_blit(c, tx, ty, thumb[i], TW, TH, TW);
                gfx_text_center(c, i == theme ? &font_ui_bold : &font_ui, tx, ty + TH + 10, TW, wallpaper_name(i), i == theme ? UI_TEXT : UI_TEXT_DIM);
                if (ui_clicked(&ui, tx, ty, TW, TH) && i != theme) {
                    if (write_file("/etc/wallpaper", wallpaper_name(i))) { theme = i; win_set(w->fd, WIN_SET_PREFS, 0); }
                    else snprintf(status, sizeof status, "Couldn't write /etc/wallpaper");
                }
            }
        } else if (page == 1) {
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
        ui_end(&ui);
    } while (ui_poll(&ui, page == 1 ? 1000 : -1));
    gui_window_destroy(w);
    return 0;
}
