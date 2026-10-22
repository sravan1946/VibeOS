/* theme - desktop theme, accent, animations, terminal text size and wallpaper (/dev/theme, /dev/wallpaper).
 *   theme                  list themes, accents and the current settings
 *   theme [-s] NAME        switch (-s: also save to /etc/theme, applied at boot)
 *   theme [-s] accent X    X = preset name, RRGGBB or default;   anim on|off;   zoom 0-3 (terminal text size)
 *   theme [-s] wallpaper X X = a procedural name (see `theme`), or an image path (PNG/JPEG...)
 *   theme -W               push the image named in /etc/wallpaper to the desktop (the desktop runs this) */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <wallpaper.h>
#include <term.h>

static int usage(void) {
    fprintf(stderr, "usage: theme [-s] [NAME | accent X | anim on|off | zoom 0-3 | wallpaper NAME|FILE]\n       theme -W\n");
    return 2;
}

static int read_cfg(char *buf, int n) {
    int fd = open("/etc/wallpaper", O_RDONLY), r = fd >= 0 ? read(fd, buf, n - 1) : -1;
    if (fd >= 0) close(fd);
    if (r < 0) return -1;
    while (r > 0 && buf[r - 1] <= ' ') r--;
    buf[r] = 0;
    return r;
}

static int write_cfg(const char *s) {
    int fd = open("/etc/wallpaper", O_WRONLY | O_CREAT | O_TRUNC, 0644), r = -1;
    if (fd >= 0) { r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1; close(fd); }
    return r;
}

int main(int argc, char **argv) {
    int save = 0, i = 1;
    char cmd[96] = "", wp[160];
    if (argc > 1 && !strcmp(argv[1], "-W")) {                       /* run by the desktop at boot / after a mode change */
        if (read_cfg(wp, sizeof wp) <= 0 || wp[0] != '/') return 0;
        return gui_wallpaper_push(wp) < 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-s")) { save = 1; i++; }
    if (i >= argc) {
        struct theme_state st;
        if (gui_theme_state(&st) < 0) { fprintf(stderr, "theme: no /dev/theme\n"); return 1; }
        for (int k = 0; k < theme_count(); k++) printf("%c %-15s %s\n", k == st.index ? '*' : ' ', theme_get(k)->name, theme_get(k)->label);
        printf("accent:    ");
        for (int k = 0; k < THEME_ACCENTS; k++) printf("%s ", theme_accents[k].name);
        if (st.accent) printf("(now %06X)\n", st.accent & 0xFFFFFF); else printf("(now default)\n");
        printf("anim:      %s\nzoom:      %d (terminal text size, 0-%d)\nwallpaper: ", st.anim ? "on" : "off", (int)st.zoom, TERM_ZOOMS - 1);
        for (int k = 0; k < wallpaper_count(); k++) printf("%s ", wallpaper_name(k));
        printf("or an image path");
        if (read_cfg(wp, sizeof wp) > 0) printf(" (now %s)", wp);
        printf("\n");
        return 0;
    }
    const char *a = argv[i];
    if (!strcmp(a, "wallpaper") && i + 1 < argc) {
        const char *w = argv[i + 1];
        int ok = w[0] == '/' ? gui_wallpaper_push(w) : 0;
        if (w[0] != '/') {
            int fd = open("/dev/wallpaper", O_WRONLY);
            ok = fd >= 0 && write(fd, w, strlen(w)) == (ssize_t)strlen(w) ? 0 : -1;
            if (fd >= 0) close(fd);
        }
        if (ok < 0) { fprintf(stderr, "theme: can't use wallpaper %s\n", w); return 1; }
        if (save && write_cfg(w) < 0) { fprintf(stderr, "theme: can't write /etc/wallpaper\n"); return 1; }
        return 0;
    }
    if ((!strcmp(a, "accent") || !strcmp(a, "anim") || !strcmp(a, "zoom")) && i + 1 < argc) {
        const char *v = argv[i + 1];
        if (!strcmp(v, "on")) v = "1"; else if (!strcmp(v, "off")) v = "0";
        snprintf(cmd, sizeof cmd, "%s=%s", a, v);
    } else if (i + 1 == argc) snprintf(cmd, sizeof cmd, "%s", a);
    else return usage();
    if (gui_theme_set(cmd) < 0) { fprintf(stderr, "theme: bad setting '%s' (try `theme` for the list)\n", cmd); return 1; }
    if (save && gui_theme_save() < 0) { fprintf(stderr, "theme: can't write /etc/theme\n"); return 1; }
    return 0;
}
