/* viewer [file] - image viewer: PNG/JPEG/GIF/BMP/PPM, fit-to-window, zoom (wheel, +/-), drag to pan, 1:1,
 * arrow keys browse the folder, checkerboard behind transparency, animated GIFs play (p pauses). No argument opens ~/Pictures. */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <syscall.h>
#include <math.h>
#include "image.h"

#define MAX_FILES 1024
#define CANVAS RGB(0x14, 0x15, 0x17)    /* neutral dark canvas in every theme (content, like a slide table) */
#define ZMIN 0.02f
#define ZMAX 32.0f

static char dir[PATH_MAX];
static char (*names)[96];
static int nnames, cur;
static struct image im;                         /* a view of an.px[frame], owned by `an` */
static struct image_anim an;
static int frame, plays;
static bool paused;
static uint32_t next_ms;                        /* when the next GIF frame is due */
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

static uint32_t frame_ms(void) { int d = an.delay[frame]; return d < 20 ? 100 : d; }     /* browsers treat 0/10 ms as 100 */

static void load_cur(void) {
    image_anim_free(&an);
    im.w = im.h = 0; im.px = NULL;
    have = false;
    frame = plays = 0;
    paused = false;
    vkey.ok = false;
    fit = true;
    if (cur < 0 || cur >= nnames) { failed = false; return; }
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", names[cur]);
    have = image_load_anim(p, &an) == 0;
    failed = !have;
    if (have) { im.w = an.w; im.h = an.h; im.px = an.px[0]; next_ms = (uint32_t)uptime_ms() + frame_ms(); }
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

static void draw_image(struct canvas *c, int oy, int vw, int vh, bool smooth) {
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
                uint32_t bg = ((x0 + x) >> 3 ^ (y0 + y) >> 3) & 1 ? RGB(0x3A, 0x3B, 0x3E) : RGB(0x2C, 0x2D, 0x30);
                view[y * rw + x] = blend(bg, p);
            }
        vkey.x = x0; vkey.y = y0; vkey.w = rw; vkey.h = rh; vkey.zoom = zoom; vkey.cx = cx; vkey.cy = cy; vkey.smooth = smooth; vkey.ok = true;
    }
    gfx_blit(c, x0, oy + y0, view, rw, rh, rw);
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Photos", 880, 640, 0);
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
    int timeout = -1;
    do {
        if (have && an.n > 1 && !paused && (int)((uint32_t)uptime_ms() - next_ms) >= 0) {     /* next GIF frame */
            if (++frame == an.n) {
                frame = 0;
                if (an.loop && ++plays >= an.loop) { frame = an.n - 1; paused = true; }
            }
            im.px = an.px[frame];
            vkey.ok = false;
            next_ms += frame_ms();
            if ((int)((uint32_t)uptime_ms() - next_ms) > 0) next_ms = (uint32_t)uptime_ms() + frame_ms();   /* fell behind: don't burst */
        }
        int W = w->w, H = w->h, vw = W, vy = H_TOOLBAR, vh = H - H_TOOLBAR - H_STATUS, my = ui.my - vy;
        if (vh < 1) vh = 1;
        bool in_view = my < vh && my >= 0;
        if (have) {
            if (fit) { zoom = fit_zoom(vw, vh); cx = im.w / 2.0f; cy = im.h / 2.0f; }
            if (ui.scroll && in_view) zoom_at(zoom * (ui.scroll < 0 ? 1.25f : 0.8f), ui.mx, my, vw, vh);
            if (ui.pressed && in_view) { dragging = true; lx = ui.mx; ly = ui.my; if (ui.dbl) { if (fit) zoom_at(1, ui.mx, my, vw, vh); else fit = true; dragging = false; } }
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
            else if (k.ch == 'p' && an.n > 1) { paused = !paused; if (!paused) { plays = 0; next_ms = (uint32_t)uptime_ms() + frame_ms(); } }
        }
        if (have) { if (fit) zoom = fit_zoom(vw, vh); clamp_pan(vw, vh); }

        ui_clear(&ui);
        struct canvas *c = ui.c;
        if (have) { gfx_fill(c, 0, vy, vw, vh, CANVAS); draw_image(c, vy, vw, vh, !dragging); }
        else if (failed) ui_empty(&ui, 0, vy, vw, vh, ICON_IMAGE, "Can't open this image", "PNG, JPEG, GIF, BMP and PPM images are supported.", NULL);
        else ui_empty(&ui, 0, vy, vw, vh, ICON_IMAGE, "No images here", "Open a picture from Files to browse its folder.", NULL);

        /* toolbar: browse, then zoom */
        ui_toolbar(&ui, 0, 0, W);
        int ty = (H_TOOLBAR - H_CTL) / 2, nav = nnames > 1 ? 0 : UI_DISABLED, zd = have ? 0 : UI_DISABLED, x = SP_3;
        if (ui_icon_button(&ui, x, ty, ICON_CHEVRON_LEFT, nav, "Previous · Left")) step(-1);
        x += H_CTL + SP_2;
        if (ui_icon_button(&ui, x, ty, ICON_CHEVRON_RIGHT, nav, "Next · Right")) step(1);
        x += H_CTL + SP_4;
        if (ui_icon_button(&ui, x, ty, ICON_ZOOM_OUT, zd, "Zoom out · −") && have) zoom_at(zoom * 0.8f, vw / 2, vh / 2, vw, vh);
        x += H_CTL + SP_2;
        if (ui_icon_button(&ui, x, ty, ICON_ZOOM_IN, zd, "Zoom in · +") && have) zoom_at(zoom * 1.25f, vw / 2, vh / 2, vw, vh);
        x += H_CTL + SP_2;
        if (ui_icon_button(&ui, x, ty, ICON_FIT, zd | (fit && have ? UI_ON : 0), "Fit to window · F") && have) fit = true;
        x += H_CTL + SP_2;
        int bw = ui_button_w(UI_NOICON, "100%");
        if (ui_button_ex(&ui, x, ty, bw, H_CTL, UI_NOICON, "100%", UI_GHOST | zd | (have && !fit && zoom == 1 ? UI_ON : 0)) && have) {
            if (zoom == 1 && !fit) fit = true; else zoom_at(1, vw / 2, vh / 2, vw, vh);
        }
        ui_tooltip(&ui, x, ty, bw, H_CTL, "Actual size · 1");

        /* status bar: file name left, size, zoom and position right */
        char info[128], z[16];
        snprintf(z, sizeof z, "%d%%", (int)(zoom * 100 + 0.5f));
        if (have && an.n > 1) snprintf(info, sizeof info, "%d × %d  ·  %s  ·  frame %d of %d  ·  %d of %d", im.w, im.h, z, frame + 1, an.n, cur + 1, nnames);
        else if (have) snprintf(info, sizeof info, "%d × %d  ·  %s  ·  %d of %d", im.w, im.h, z, cur + 1, nnames);
        else snprintf(info, sizeof info, "%d image%s", nnames, nnames == 1 ? "" : "s");
        ui_status(&ui, 0, H - H_STATUS, W, nnames ? names[cur] : NULL, info);

        if (have) snprintf(title, sizeof title, "%s — Photos", names[cur]);
        else snprintf(title, sizeof title, "Photos");
        if (strcmp(title, lasttitle)) { gui_set_title(w, title); strcpy(lasttitle, title); }
        ui_end(&ui);
        timeout = -1;
        if (have && an.n > 1 && !paused) { int d = (int)(next_ms - (uint32_t)uptime_ms()); timeout = d < 1 ? 1 : d; }
    } while (ui_poll(&ui, timeout));
    gui_window_destroy(w);
    return 0;
}
