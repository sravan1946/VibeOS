/* about - VibeOS info card */
#include <ui.h>
#include <design.h>
#include <icons.h>
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

/* a card of label / value rows (44 tall, values right-aligned in text2, ellipsised) */
static void facts(struct ui *ui, int x, int y, int w, const char *const *k, char (*v)[96], int n) {
    ui_card(ui, x, y, w, n * H_ROW2);
    for (int i = 0; i < n; i++) {
        int yy = y + i * H_ROW2, vw = gfx_text_width(T_BODY, v[i]), maxw = w - 2 * SP_4 - 112;
        ui_card_row(ui, x, yy, w, H_ROW2, k[i], NULL, i == 0);
        ui_text_fit(ui, T_BODY, x + w - SP_4 - (vw < maxw ? vw : maxw), yy + (H_ROW2 - T_BODY->height) / 2, maxw, v[i], UI_TEXT2);
    }
}

int main(void) {
    struct gui_window *w = gui_window_create("About VibeOS", 460, 474, WIN_FLAG_NORESIZE);
    if (!w) { perror("about"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    char cpu[96], ver[96];
    read_line("/proc/cpuinfo", "model name", cpu, sizeof cpu);
    if (!cpu[0]) read_line("/proc/cpuinfo", "", cpu, sizeof cpu);
    read_line("/proc/version", "", ver, sizeof ver);
    static const char *const k[] = { "Processor", "Memory", "Display", "Uptime", "Font licences", "Icon licences" };
    do {
        struct sysinfo si;
        sysinfo(&si);
        ui_clear(&ui);
        struct canvas *c = ui.c;
        int W = w->w, x = SP_6, cw = W - 2 * SP_6, y = SP_6;
        const struct aicon *logo = appicon_get(APPICON_ABOUT, ICON_APP);
        if (logo) ui_aicon(&ui, (W - logo->w) / 2, y, logo);
        y += ICON_APP + SP_3;
        gfx_text_center(c, T_TITLE, 0, y, W, "VibeOS", UI_TEXT);
        y += T_TITLE->height + SP_1;
        ui_text_fit(&ui, T_BODY, x + (cw - (gfx_text_width(T_BODY, ver) < cw ? gfx_text_width(T_BODY, ver) : cw)) / 2, y, cw, ver[0] ? ver : "VibeOS", UI_TEXT2);
        y += T_BODY->height + SP_6;
        char v[6][96] = { [4] = "Inter, JetBrains Mono, Noto Sans \xc2\xb7 OFL 1.1", [5] = "Lucide \xc2\xb7 ISC, Feather \xc2\xb7 MIT" };
        snprintf(v[0], 96, "%s", cpu[0] ? cpu : "i686");
        snprintf(v[1], 96, "%u MiB, %u MiB free", si.mem_total_kb / 1024, si.mem_free_kb / 1024);
        snprintf(v[2], 96, "%u \xc3\x97 %u, 32-bit colour", si.screen_w, si.screen_h);
        uint32_t s = si.uptime_ms / 1000;
        snprintf(v[3], 96, "%uh %um %us", s / 3600, s / 60 % 60, s % 60);
        facts(&ui, x, y, cw, k, v, 6);
        int by = w->h - SP_6 - H_CTL;
        gfx_text(c, T_CAPTION, x, by + (H_CTL - T_CAPTION->height) / 2, "Licence texts are in /usr/share/licenses", UI_TEXT3);
        int bw = ui_button_w(UI_NOICON, "Close");
        if (bw < W_CTL_MIN) bw = W_CTL_MIN;
        if (ui_button(&ui, W - SP_6 - bw, by, bw, H_CTL, "Close", UI_SECONDARY)) break;
        ui_end(&ui);
    } while (ui_poll(&ui, 1000));
    gui_window_destroy(w);
    return 0;
}
