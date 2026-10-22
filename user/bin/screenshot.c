/* screenshot [file] - save the composed screen (read from /dev/screen) as PNG; PrtSc runs this.
 * Default name: /home/Pictures/screenshot-N.png. Shows a short toast when run from the desktop. */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "image.h"

static void toast(const char *msg, bool bad) {
    struct gui_window *w = gui_window_create("Screenshot", 360, 48, WIN_FLAG_FRAMELESS | WIN_FLAG_NORESIZE);
    if (!w) return;
    struct canvas *c = &w->cv;
    gfx_fill(c, 0, 0, w->w, w->h, 0);
    gfx_round_fill(c, 0, 0, w->w, w->h, 12, RGB(0x2A, 0x27, 0x3D));
    gfx_round_rect(c, 0, 0, w->w, w->h, 12, RGBA(255, 255, 255, 50));
    gfx_circle_fill(c, 24, 24, 6, bad ? RGB(0xFF, 0x5F, 0x7A) : RGB(0x4B, 0xE0, 0x8A));
    struct canvas t = *c;
    canvas_clip(&t, 40, 0, w->w - 52, w->h);
    gfx_text(&t, &font_ui, 40, (w->h - font_ui.height) / 2, msg, RGB(0xED, 0xEB, 0xFF));
    gui_flip(w);
    usleep(1800000);
    gui_window_destroy(w);
}

int main(int argc, char **argv) {
    int fd = open("/dev/screen", O_RDONLY);
    uint32_t hdr[2];
    if (fd < 0 || read(fd, hdr, 8) != 8) { fprintf(stderr, "screenshot: no /dev/screen\n"); return 1; }
    int w = hdr[0], h = hdr[1];
    size_t n = (size_t)w * h * 4, got = 0;
    uint32_t *px = malloc(n);
    while (px && got < n) {
        int r = read(fd, (char *)px + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    if (!px || got < n) { fprintf(stderr, "screenshot: short read\n"); return 1; }
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xFF000000u;
    char path[PATH_MAX];
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else {
        mkdir("/home/Pictures", 0755);
        struct stat st;
        for (int i = 1; i < 10000; i++) {
            snprintf(path, sizeof path, "/home/Pictures/screenshot-%d.png", i);
            if (stat(path, &st)) break;
        }
    }
    bool ok = image_save_png(path, px, w, h) == 0;
    char msg[PATH_MAX + 32];
    snprintf(msg, sizeof msg, ok ? "Saved %s" : "Could not save %s", path);
    if (ok) puts(msg); else fprintf(stderr, "%s\n", msg);
    if (!isatty(1)) toast(msg, !ok);
    free(px);
    return ok ? 0 : 1;
}
