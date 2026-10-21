/* about - VibeOS info card */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

static void read_line(const char *path, const char *key, char *out, int n) {
    FILE *f = fopen(path, "r");
    char line[256];
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key))) continue;
        char *v = *key ? strchr(line, ':') : 0;
        v = v ? v + 1 : line;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, "\n")] = 0;
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}

int main(void) {
    struct gui_window *w = gui_window_create("About VibeOS", 460, 330, WIN_FLAG_NORESIZE);
    if (!w) { perror("about"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    char cpu[96], ver[96];
    read_line("/proc/cpuinfo", "model name", cpu, sizeof cpu);
    if (!cpu[0]) read_line("/proc/cpuinfo", "", cpu, sizeof cpu);
    read_line("/proc/version", "", ver, sizeof ver);
    do {
        struct sysinfo si;
        sysinfo(&si);
        ui_clear(&ui);
        struct canvas *c = ui.c;
        gfx_vgradient(c, 0, 0, w->w, 120, RGB(0x3A, 0x22, 0x6B), UI_BG);
        int cx = w->w / 2;                                   /* diamond logo */
        for (int i = 0; i < 26; i++) {
            color_t col = mix(UI_ACCENT, UI_ACCENT2, i * 256 / 26);
            gfx_fill(c, cx - i, 22 + i, 2 * i + 1, 1, col);
            gfx_fill(c, cx - i, 73 - i, 2 * i + 1, 1, col);
        }
        gfx_text_center(c, &font_title, 0, 92, w->w, "VibeOS", UI_TEXT);
        gfx_text_center(c, &font_ui, 0, 122, w->w, ver[0] ? ver : "VibeOS", UI_TEXT_DIM);
        ui_panel(&ui, 20, 152, w->w - 40, 112);
        char buf[128];
        const char *k[] = { "Processor", "Memory", "Display", "Uptime" };
        char v[4][96];
        snprintf(v[0], 96, "%s", cpu[0] ? cpu : "i686");
        snprintf(v[1], 96, "%u MiB (%u MiB free)", si.mem_total_kb / 1024, si.mem_free_kb / 1024);
        snprintf(v[2], 96, "%ux%u, 32-bit colour", si.screen_w, si.screen_h);
        uint32_t s = si.uptime_ms / 1000;
        snprintf(v[3], 96, "%uh %um %us", s / 3600, s / 60 % 60, s % 60);
        for (int i = 0; i < 4; i++) {
            gfx_text(c, &font_ui_bold, 36, 164 + i * 24, k[i], UI_TEXT_DIM);
            int maxw = w->w - 40 - 140;
            snprintf(buf, sizeof buf, "%s", v[i]);
            while (gfx_text_width(&font_ui, buf) > maxw && strlen(buf) > 3) strcpy(buf + strlen(buf) - 4, "...");
            gfx_text(c, &font_ui, 150, 164 + i * 24, buf, UI_TEXT);
        }
        if (ui_button(&ui, w->w / 2 - 60, 280, 120, 34, "Close", UI_ACCENT_BTN)) break;
        ui_end(&ui);
    } while (ui_poll(&ui, 1000));
    gui_window_destroy(w);
    return 0;
}
