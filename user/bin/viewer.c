/* viewer [file] - image viewer: PNG/BMP/PPM, fit-to-window, zoom (wheel, +/-), drag to pan, 1:1,
 * arrow keys browse the folder, checkerboard behind transparency. No argument opens ~/Pictures. */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <math.h>
#include "image.h"

#define MAX_FILES 1024
#define BAR_H 40
#define ZMIN 0.02f
#define ZMAX 32.0f

static char dir[PATH_MAX];
static char (*names)[96];
static int nnames, cur;
static struct image im;
static bool have, failed;
static float zoom = 1, cx, cy;                  /* cx, cy: image point at the viewport centre */
static bool fit = true, dragging;
static uint32_t *view;                          /* the visible part of the image, checkerboard-blended */
static size_t view_cap;
static struct { int x, y, w, h; float zoom, cx, cy; bool smooth, ok; } vkey;   /* what `view` currently shows */

static int cmp_name(const void *a, const void *b) { return strcasecmp(a, b); }

static void scan(const char *keep) {
    DIR *d = opendir(dir);
    nnames = 0;
    if (!names) names = malloc(MAX_FILES * sizeof *names);
    if (!d || !names) return;
    struct dirent *de;
    while ((de = readdir(d)) && nnames < MAX_FILES)
        if (image_is_ext(de->d_name)) snprintf(names[nnames++], sizeof *names, "%s", de->d_name);
    closedir(d);
    qsort(names, nnames, sizeof *names, cmp_name);
    cur = 0;
    for (int i = 0; i < nnames; i++) if (keep && !strcmp(names[i], keep)) cur = i;
}

static void load_cur(void) {
    image_free(&im);
    have = false;
    vkey.ok = false;
    fit = true;
    if (cur < 0 || cur >= nnames) { failed = false; return; }
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", names[cur]);
    have = image_load(p, &im) == 0;
    failed = !have;
    cx = im.w / 2.0f; cy = im.h / 2.0f;
}

static void step(int d) {
    if (nnames < 2) return;
    cur = (cur + d + nnames) % nnames;
    load_cur();
}

static float fit_zoom(int vw, int vh) {
    float z = (float)vw / im.w, zy = (float)vh / im.h;
    if (zy < z) z = zy;
    return z > 1 ? 1 : z;
}

static void clamp_pan(int vw, int vh) {
    float hw = vw / 2.0f / zoom, hh = vh / 2.0f / zoom;
    cx = im.w <= 2 * hw ? im.w / 2.0f : cx < hw ? hw : cx > im.w - hw ? im.w - hw : cx;
    cy = im.h <= 2 * hh ? im.h / 2.0f : cy < hh ? hh : cy > im.h - hh ? im.h - hh : cy;
    if (zoom == 1) { cx = floorf(cx + 0.5f); cy = floorf(cy + 0.5f); }          /* pixel-exact at 1:1 */
}

/* zoom keeping the image point under (px, py) fixed */
static void zoom_at(float nz, int px, int py, int vw, int vh) {
    if (nz < ZMIN) nz = ZMIN;
    if (nz > ZMAX) nz = ZMAX;
    float u = cx + (px - vw / 2) / zoom, v = cy + (py - vh / 2) / zoom;
    zoom = nz;
    cx = u - (px - vw / 2) / zoom;
    cy = v - (py - vh / 2) / zoom;
    fit = false;
}

static void draw_image(struct canvas *c, int vw, int vh, bool smooth) {
    float ix0 = vw / 2 - cx * zoom, iy0 = vh / 2 - cy * zoom;
    int x0 = (int)ix0 < 0 ? 0 : (int)ix0, y0 = (int)iy0 < 0 ? 0 : (int)iy0;
    int x1 = (int)(ix0 + im.w * zoom + 0.999f), y1 = (int)(iy0 + im.h * zoom + 0.999f);
    if (x1 > vw) x1 = vw;
    if (y1 > vh) y1 = vh;
    int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0) return;
    if (!vkey.ok || vkey.x != x0 || vkey.y != y0 || vkey.w != rw || vkey.h != rh || vkey.zoom != zoom ||
        vkey.cx != cx || vkey.cy != cy || vkey.smooth != smooth) {
        if ((size_t)rw * rh > view_cap) {
            free(view);
            view = malloc((view_cap = (size_t)rw * rh) * 4);
            if (!view) { view_cap = 0; return; }
        }
        image_sample(&im, view, rw, rh, rw, (x0 - ix0) / zoom, (y0 - iy0) / zoom, zoom, smooth);
        for (int y = 0; y < rh; y++)                       /* blend onto a screen-fixed checkerboard */
            for (int x = 0; x < rw; x++) {
                uint32_t p = view[y * rw + x];
                if (p >> 24 == 255) continue;
                uint32_t bg = ((x0 + x) >> 3 ^ (y0 + y) >> 3) & 1 ? RGB(0x5A, 0x58, 0x6A) : RGB(0x44, 0x42, 0x54);
                view[y * rw + x] = blend(bg, p);
            }
        vkey.x = x0; vkey.y = y0; vkey.w = rw; vkey.h = rh; vkey.zoom = zoom; vkey.cx = cx; vkey.cy = cy; vkey.smooth = smooth; vkey.ok = true;
    }
    gfx_blit(c, x0, y0, view, rw, rh, rw);
    gfx_rect(c, x0 - 1, y0 - 1, rw + 2, rh + 2, RGBA(255, 255, 255, 40));
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Viewer", 880, 640, 0);
    if (!w) { perror("viewer"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    char path[PATH_MAX] = "/home/Pictures", rp[PATH_MAX];
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    if (!realpath(path, rp)) snprintf(rp, sizeof rp, "%s", path);
    const char *keep = NULL;
    if (argc > 1 && image_is_ext(rp)) {                       /* a file: browse its folder */
        char *s = strrchr(rp, '/');
        keep = s ? s + 1 : rp;
        if (s == rp) snprintf(dir, sizeof dir, "/"); else if (s) { *s = 0; snprintf(dir, sizeof dir, "%s", rp); }
    } else snprintf(dir, sizeof dir, "%s", rp);
    scan(keep);
    if (nnames) load_cur();
    char title[96] = "", lasttitle[96] = "";
    int lx = 0, ly = 0;
    do {
        int W = w->w, H = w->h, vw = W, vh = H - BAR_H;
        if (vh < 1) vh = 1;
        bool in_view = ui.my < vh && ui.my >= 0;
        if (have) {
            if (fit) { zoom = fit_zoom(vw, vh); cx = im.w / 2.0f; cy = im.h / 2.0f; }
            if (ui.scroll && in_view) zoom_at(zoom * (ui.scroll < 0 ? 1.25f : 0.8f), ui.mx, ui.my, vw, vh);
            if (ui.pressed && in_view) { dragging = true; lx = ui.mx; ly = ui.my; if (ui.dbl) { if (fit) zoom_at(1, ui.mx, ui.my, vw, vh); else fit = true; dragging = false; } }
            else if (dragging && (ui.buttons & 1)) { cx -= (ui.mx - lx) / zoom; cy -= (ui.my - ly) / zoom; lx = ui.mx; ly = ui.my; fit = false; }
            else dragging = false;
        }
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            if (k.key == KEY_RIGHT || k.key == KEY_PGDN || k.key == KEY_DOWN || k.ch == ' ') step(1);
            else if (k.key == KEY_LEFT || k.key == KEY_PGUP || k.key == KEY_UP) step(-1);
            else if (k.key == KEY_HOME && nnames) { cur = 0; load_cur(); }
            else if (k.key == KEY_END && nnames) { cur = nnames - 1; load_cur(); }
            else if (!have) continue;
            else if (k.ch == '+' || k.ch == '=') zoom_at(zoom * 1.25f, vw / 2, vh / 2, vw, vh);
            else if (k.ch == '-' || k.ch == '_') zoom_at(zoom * 0.8f, vw / 2, vh / 2, vw, vh);
            else if (k.ch == '1') { if (fit || zoom != 1) zoom_at(1, vw / 2, vh / 2, vw, vh); else fit = true; }
            else if (k.ch == 'f' || k.ch == '0') fit = true;
        }
        if (have) { if (fit) zoom = fit_zoom(vw, vh); clamp_pan(vw, vh); }

        ui_clear(&ui);
        struct canvas *c = ui.c;
        gfx_fill(c, 0, 0, vw, vh, RGB(0x10, 0x0F, 0x1A));
        if (have) draw_image(c, vw, vh, !dragging);
        else gfx_text_center(c, &font_ui, 0, vh / 2 - 8, vw,
                             failed ? "Cannot open this image (PNG, BMP and PPM are supported)" : "No images here. Open a PNG, BMP or PPM from Files.", UI_TEXT_DIM);

        /* bottom bar */
        gfx_fill(c, 0, vh, W, BAR_H, UI_PANEL);
        gfx_fill(c, 0, vh, W, 1, UI_BORDER);
        int by = vh + 6;
        if (ui_tool(&ui, 8, by, 32, 28, "<", false)) step(-1);
        if (ui_tool(&ui, 44, by, 32, 28, ">", false)) step(1);
        if (ui_tool(&ui, 92, by, 32, 28, "-", false) && have) zoom_at(zoom * 0.8f, vw / 2, vh / 2, vw, vh);
        if (ui_tool(&ui, 128, by, 32, 28, "+", false) && have) zoom_at(zoom * 1.25f, vw / 2, vh / 2, vw, vh);
        if (ui_tool(&ui, 164, by, 44, 28, "Fit", fit) && have) fit = true;
        if (ui_tool(&ui, 212, by, 44, 28, "1:1", !fit && zoom == 1) && have) { if (zoom == 1 && !fit) fit = true; else zoom_at(1, vw / 2, vh / 2, vw, vh); }
        char info[128], z[16];
        snprintf(z, sizeof z, "%d%%", (int)(zoom * 100 + 0.5f));
        if (have) snprintf(info, sizeof info, "%d x %d   %s   %d/%d", im.w, im.h, z, cur + 1, nnames);
        else snprintf(info, sizeof info, "%d image%s", nnames, nnames == 1 ? "" : "s");
        int iw = gfx_text_width(&font_ui, info);
        gfx_text(c, &font_ui, W - 14 - iw, vh + (BAR_H - font_ui.height) / 2, info, UI_TEXT_DIM);
        if (nnames) ui_text_fit(&ui, &font_ui, 272, vh + (BAR_H - font_ui.height) / 2, W - 272 - iw - 30, names[cur], UI_TEXT);

        if (have) snprintf(title, sizeof title, "%s - %dx%d - %s", names[cur], im.w, im.h, z);
        else snprintf(title, sizeof title, "Viewer");
        if (strcmp(title, lasttitle)) { gui_set_title(w, title); strcpy(lasttitle, title); }
        ui_end(&ui);
    } while (ui_poll(&ui, -1));
    gui_window_destroy(w);
    return 0;
}
