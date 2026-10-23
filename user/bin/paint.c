/* paint - bitmap canvas with pencil, line, rectangle, ellipse, bucket fill, eraser, undo, PNG/BMP save, PNG/BMP/PPM open */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscall.h>
#include "image.h"

#define DOC_W 800
#define DOC_H 520
#define UNDO_N 8

enum { TL_PENCIL, TL_LINE, TL_RECT, TL_ELLIPSE, TL_FILL, TL_ERASER, TL_COUNT };

static uint32_t *doc;
static struct canvas dc;                      /* canvas over doc, so the gfx primitives clip for us */
static uint32_t *undo[UNDO_N];
static int nundo;
static bool dirty, again;                      /* again: a button changed the canvas after it was drawn */

static int tool = TL_PENCIL, brush = 3;
static bool fill_shapes;
static color_t fg = RGB(0x1B, 0x1C, 0x1F), bg = RGB(255, 255, 255);

/* drawing colours (content, not chrome): a grey ramp and muted hues that sit with Graphite in both themes */
static const color_t palette[] = {
    RGB(0x1B, 0x1C, 0x1F), RGB(0x55, 0x58, 0x5E), RGB(0x8A, 0x8D, 0x94), RGB(0xC5, 0xC8, 0xCD), RGB(0xFF, 0xFF, 0xFF),
    RGB(0x7A, 0x2B, 0x27), RGB(0xC2, 0x45, 0x3E), RGB(0xD9, 0x82, 0x2B), RGB(0xE8, 0xA3, 0x3D), RGB(0xE3, 0xC7, 0x5A),
    RGB(0x9A, 0xA5, 0x48), RGB(0x4F, 0x9A, 0x5E), RGB(0x2F, 0x5E, 0x3A), RGB(0x3E, 0x8F, 0x8A), RGB(0x4A, 0x7F, 0xC1),
    RGB(0x2A, 0x4E, 0x7A), RGB(0x5B, 0x67, 0xB0), RGB(0x8A, 0x63, 0xA8), RGB(0x4E, 0x3A, 0x6B), RGB(0xC2, 0x66, 0x8F),
    RGB(0x8B, 0x5E, 0x3C), RGB(0x5A, 0x3E, 0x28), RGB(0xC9, 0xA9, 0x7E), RGB(0xE9, 0xDC, 0xC4),
};
#define NPAL ((int)(sizeof palette / sizeof palette[0]))

static char savepath[PATH_MAX] = "/home/Pictures/drawing.png";
static bool saving, opening;
static struct ui *U;

static void say(const char *fmt, const char *arg) {
    char m[96];
    snprintf(m, sizeof m, fmt, arg);
    ui_toast(U, m, NULL);
}

/* ---- undo ---- */
static void push_undo(void) {
    uint32_t *copy;
    if (nundo == UNDO_N) {
        copy = undo[0];                          /* recycle the oldest snapshot */
        memmove(undo, undo + 1, (UNDO_N - 1) * sizeof *undo);
        nundo--;
    } else copy = malloc(DOC_W * DOC_H * 4);
    if (!copy) return;
    memcpy(copy, doc, DOC_W * DOC_H * 4);
    undo[nundo++] = copy;
}
static void do_undo(void) {
    if (!nundo) { say("%s", "Nothing to undo"); return; }
    again = true;
    memcpy(doc, undo[--nundo], DOC_W * DOC_H * 4);
    free(undo[nundo]);
    undo[nundo] = NULL;
}
static void clear_doc(void) {
    push_undo();
    again = true;
    for (int i = 0; i < DOC_W * DOC_H; i++) doc[i] = bg;
    dirty = true;
}

/* ---- primitives on the document ---- */
/* hard-edged round brush (the gfx circle is anti-aliased, which leaves halos for the bucket fill) */
static void stamp(int x, int y, int size, color_t col) {
    if (size <= 1) { gfx_pixel(&dc, x, y, col); return; }
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            int a = 2 * i - (size - 1), b = 2 * j - (size - 1);
            if (a * a + b * b <= size * size) gfx_pixel(&dc, x - size / 2 + i, y - size / 2 + j, col);
        }
}

static void stroke(int x0, int y0, int x1, int y1, int size, color_t col) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        stamp(x0, y0, size, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void shape_rect(int x0, int y0, int x1, int y1, int size, color_t col, bool fill) {
    int lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0, ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
    if (fill) { gfx_fill(&dc, lx, ly, hx - lx + 1, hy - ly + 1, col); return; }
    stroke(lx, ly, hx, ly, size, col);
    stroke(hx, ly, hx, hy, size, col);
    stroke(hx, hy, lx, hy, size, col);
    stroke(lx, hy, lx, ly, size, col);
}

static void shape_ellipse(int x0, int y0, int x1, int y1, int size, color_t col, bool fill) {
    float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f, rx = abs(x1 - x0) / 2.0f, ry = abs(y1 - y0) / 2.0f;
    if (fill) {
        for (int y = (int)(cy - ry); y <= (int)(cy + ry + 0.5f); y++) {
            float t = ry > 0 ? (y - cy) / ry : 0;
            float k = 1 - t * t;
            if (k < 0) continue;
            int hw = (int)(rx * sqrtf(k) + 0.5f);
            gfx_fill(&dc, (int)cx - hw, y, 2 * hw + 1, 1, col);
        }
        return;
    }
    int n = (int)((rx > ry ? rx : ry) * 6) + 8;
    int px = (int)(cx + rx), py = (int)cy;
    for (int i = 1; i <= n; i++) {
        float a = i * 2 * (float)M_PI / n;
        int x = (int)(cx + rx * cosf(a) + 0.5f), y = (int)(cy + ry * sinf(a) + 0.5f);
        stroke(px, py, x, y, size, col);
        px = x; py = y;
    }
}

static void flood(int x, int y, color_t col) {
    if (x < 0 || y < 0 || x >= DOC_W || y >= DOC_H) return;
    uint32_t target = doc[y * DOC_W + x];
    if (target == col) return;
    int cap = 4096, sp = 0;
    int *st = malloc(cap * 2 * sizeof(int));
    if (!st) return;
    st[sp * 2] = x; st[sp * 2 + 1] = y; sp++;
    while (sp) {
        sp--;
        int cx = st[sp * 2], cy = st[sp * 2 + 1];
        int l = cx, r = cx;
        if (doc[cy * DOC_W + cx] != target) continue;
        while (l > 0 && doc[cy * DOC_W + l - 1] == target) l--;
        while (r < DOC_W - 1 && doc[cy * DOC_W + r + 1] == target) r++;
        for (int i = l; i <= r; i++) doc[cy * DOC_W + i] = col;
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = cy + dy;
            if (ny < 0 || ny >= DOC_H) continue;
            bool run = false;
            for (int i = l; i <= r; i++) {
                bool match = doc[ny * DOC_W + i] == target;
                if (match && !run) {
                    if (sp == cap) { cap *= 2; st = realloc(st, cap * 2 * sizeof(int)); if (!st) return; }
                    st[sp * 2] = i; st[sp * 2 + 1] = ny; sp++;
                }
                run = match;
            }
        }
    }
    free(st);
}

/* ---- BMP export (24-bit, bottom-up) ---- */
/* the ext2 driver occasionally reports a transient EIO / short write under load: retry instead of giving up */
static bool write_all(int fd, const void *buf, int n) {
    const char *p = buf;
    int fails = 0;
    while (n > 0) {
        int r = write(fd, p, n);
        if (r > 0) { p += r; n -= r; fails = 0; continue; }
        if (++fails > 8) return false;
        usleep(100000);
    }
    return true;
}

static bool save_bmp(const char *path) {
    int fd = -1;
    for (int i = 0; i < 4 && fd < 0; i++) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) return false;
    int rowb = (DOC_W * 3 + 3) & ~3;
    uint32_t size = 54 + rowb * DOC_H;
    uint8_t h[54] = { 'B', 'M' };
    memcpy(h + 2, &size, 4);
    uint32_t off = 54, dib = 40, w = DOC_W, hh = DOC_H, sz = rowb * DOC_H;
    uint16_t planes = 1, bpp = 24;
    memcpy(h + 10, &off, 4); memcpy(h + 14, &dib, 4); memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
    memcpy(h + 26, &planes, 2); memcpy(h + 28, &bpp, 2); memcpy(h + 34, &sz, 4);
    bool ok = write_all(fd, h, 54);
    uint8_t *row = calloc(1, rowb);
    if (!row) ok = false;
    for (int y = DOC_H - 1; y >= 0 && ok; y--) {
        for (int x = 0; x < DOC_W; x++) {
            uint32_t p = doc[y * DOC_W + x];
            row[x * 3] = p & 255; row[x * 3 + 1] = (p >> 8) & 255; row[x * 3 + 2] = (p >> 16) & 255;
        }
        ok = write_all(fd, row, rowb);
    }
    free(row);
    close(fd);
    return ok;
}

/* the format follows the extension: .bmp is a 24-bit BMP, anything else a PNG */
static bool save_doc(const char *path) {
    const char *e = strrchr(path, '.');
    if (e && !strcasecmp(e, ".bmp")) return save_bmp(path);
    return image_save_png(path, doc, DOC_W, DOC_H) == 0;
}

/* load an image onto a fresh canvas, top-left aligned and cropped (transparency shows the background colour) */
static bool open_doc(const char *path) {
    struct image im;
    if (image_load(path, &im)) return false;
    push_undo();
    again = true;
    for (int i = 0; i < DOC_W * DOC_H; i++) doc[i] = bg;
    gfx_blit_alpha(&dc, 0, 0, im.px, im.w, im.h, im.w);
    image_free(&im);
    dirty = false;
    return true;
}

static const char *const TOOL_TIP[] = { "Pencil \xc2\xb7 1", "Line \xc2\xb7 2", "Rectangle \xc2\xb7 3", "Ellipse \xc2\xb7 4", "Fill \xc2\xb7 5", "Eraser \xc2\xb7 6" };
static const char *const TOOL_NAME[] = { "Pencil", "Line", "Rectangle", "Ellipse", "Fill", "Eraser" };
static const int TOOL_ICON[] = { ICON_PENCIL, ICON_LINE, ICON_RECT, ICON_ELLIPSE, ICON_FILL, ICON_ERASER };

int main(void) {
    struct gui_window *w = gui_window_create("Paint", 880, 652, 0);
    if (!w) { perror("paint"); return 1; }
    doc = malloc(DOC_W * DOC_H * 4);
    if (!doc) { perror("paint"); return 1; }
    for (int i = 0; i < DOC_W * DOC_H; i++) doc[i] = bg;
    canvas_init(&dc, doc, DOC_W, DOC_H, DOC_W);
    struct ui ui;
    ui_init(&ui, w);
    U = &ui;

    bool drawing = false;
    int dbtn = 1, lx = 0, ly = 0, sx = 0, sy = 0;     /* active button, last point, shape start */
    do {
        int W = w->w, H = w->h;
        int palH = H_TOOLBAR + H_STATUS;                /* swatch strip + status bar */
        bool modal = saving || opening;
        int smx = ui.mx, smy = ui.my;
        bool sp = ui.pressed, sr = ui.released, srp = ui.rpressed;
        int nk = ui.nkeys;
        struct gui_event saved[32];
        memcpy(saved, ui.keys, sizeof saved);
        if (modal) { ui.mx = ui.my = -1; ui.pressed = ui.released = ui.rpressed = false; ui.nkeys = 0; }

        /* canvas placement */
        int ax = 0, ay = H_TOOLBAR, aw = W, ah = H - H_TOOLBAR - palH;
        int cx0 = ax + (aw > DOC_W + 24 ? (aw - DOC_W) / 2 : 12), cy0 = ay + (ah > DOC_H + 24 ? (ah - DOC_H) / 2 : 12);
        int mx = ui.mx - cx0, my = ui.my - cy0;
        bool over = ui_hover(&ui, ax, ay, aw, ah);

        /* keys */
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            if (k.mods & MOD_CTRL) {
                if (k.key == 'z') { do_undo(); drawing = false; }
                else if (k.key == 's') saving = true;
                else if (k.key == 'o') opening = true;
            } else if (k.ch == '[' && brush > 1) brush--;
            else if (k.ch == ']' && brush < 40) brush++;
            else if (k.ch >= '1' && k.ch <= '6') tool = k.ch - '1';
        }

        /* drawing interaction */
        if (!drawing && over && (ui.pressed || ui.rpressed) && mx >= -20 && my >= -20) {
            dbtn = ui.pressed ? 1 : 2;
            color_t col = tool == TL_ERASER ? bg : dbtn == 1 ? fg : bg;
            push_undo();
            dirty = true;
            sx = lx = mx; sy = ly = my;
            if (tool == TL_FILL) flood(mx, my, col);
            else {
                drawing = true;
                if (tool == TL_PENCIL || tool == TL_ERASER) stamp(mx, my, brush, col);
            }
        } else if (drawing) {
            color_t col = tool == TL_ERASER ? bg : dbtn == 1 ? fg : bg;
            if (tool == TL_PENCIL || tool == TL_ERASER) {
                stroke(lx, ly, mx, my, brush, col);
            } else if (nundo) {                                   /* shapes: redraw from the pre-stroke snapshot */
                memcpy(doc, undo[nundo - 1], DOC_W * DOC_H * 4);
                if (tool == TL_LINE) stroke(sx, sy, mx, my, brush, col);
                else if (tool == TL_RECT) shape_rect(sx, sy, mx, my, brush, col, fill_shapes);
                else shape_ellipse(sx, sy, mx, my, brush, col, fill_shapes);
            }
            lx = mx; ly = my;
            if (ui.released || !(ui.buttons & 3)) drawing = false;
        }

        /* ---- draw ---- */
redraw:
        ui_clear(&ui);
        struct canvas *c = ui.c;
        /* the document on a neutral pasteboard */
        gfx_fill(c, ax, ay, aw, ah, ui_th.base);
        struct canvas cc = *c;
        canvas_clip(&cc, ax, ay, aw, ah);
        gfx_elev(&cc, cx0, cy0, DOC_W, DOC_H, 0, E1, ui_th.elev[E1]);
        gfx_blit(&cc, cx0, cy0, doc, DOC_W, DOC_H, DOC_W);
        gfx_ring_out(&cc, cx0, cy0, DOC_W, DOC_H, 0, ui_th.line);
        if (over && !modal && mx >= 0 && my >= 0 && mx < DOC_W && my < DOC_H && tool != TL_FILL) {
            int r = brush <= 1 ? 1 : brush / 2;                   /* brush outline: reads on any paint colour */
            gfx_circle_fill(&cc, ui.mx, ui.my, r + 1, RGBA(255, 255, 255, 60));
            gfx_circle_fill(&cc, ui.mx, ui.my, r, RGBA(0, 0, 0, 90));
        }

        /* toolbar: tools | size, filled | undo, clear | open, save */
        ui_toolbar(&ui, 0, 0, W);
        int ty = (H_TOOLBAR - H_CTL) / 2, tx = SP_3;
        for (int t = 0; t < TL_COUNT; t++, tx += H_CTL + SP_2)
            if (ui_icon_button(&ui, tx, ty, TOOL_ICON[t], t == tool ? UI_ON : 0, TOOL_TIP[t])) tool = t;
        tx += SP_4 - SP_2;
        gfx_text(c, T_BODY, tx, (H_TOOLBAR - T_BODY->height) / 2, "Size", UI_TEXT2);
        tx += gfx_text_width(T_BODY, "Size") + SP_4;
        ui_slider(&ui, tx, (H_TOOLBAR - HIT_MIN) / 2, 112, &brush, 1, 40);
        tx += 112 + SP_3;
        char b[16];
        snprintf(b, sizeof b, "%d px", brush);
        gfx_text(c, T_BODY, tx + 40 - gfx_text_width(T_BODY, b), (H_TOOLBAR - T_BODY->height) / 2, b, UI_TEXT);
        tx += 40 + SP_4;
        if (tool == TL_RECT || tool == TL_ELLIPSE) ui_checkbox(&ui, tx, ty, "Filled", &fill_shapes);
        int rx = W - SP_3 - H_CTL;
        if (ui_icon_button(&ui, rx, ty, ICON_SAVE, 0, "Save \xc2\xb7 Ctrl+S")) saving = true;
        rx -= H_CTL + SP_2;
        if (ui_icon_button(&ui, rx, ty, ICON_FOLDER_OPEN, 0, "Open \xc2\xb7 Ctrl+O")) opening = true;
        rx -= H_CTL + SP_4;
        if (ui_icon_button(&ui, rx, ty, ICON_TRASH, 0, "Clear canvas")) clear_doc();
        rx -= H_CTL + SP_2;
        if (ui_icon_button(&ui, rx, ty, ICON_UNDO, nundo ? 0 : UI_DISABLED, "Undo \xc2\xb7 Ctrl+Z")) { do_undo(); drawing = false; }

        /* colour strip: the colour 1 / colour 2 pair, then the swatches */
        int py = H - palH;
        gfx_fill(c, 0, py, W, H_TOOLBAR, UI_BG);
        gfx_hline(c, 0, py, W, UI_LINE);
        gfx_round_fill(c, SP_3 + 12, py + 14, 20, 20, R_2, bg);                 /* colour 2, behind */
        gfx_round_rect(c, SP_3 + 12, py + 14, 20, 20, R_2, ui_th.control);
        gfx_round_fill(c, SP_3, py + 6, 20, 20, R_2, ui_th.window);
        gfx_round_fill(c, SP_3 + 1, py + 7, 18, 18, R_2, fg);
        gfx_round_rect(c, SP_3 + 1, py + 7, 18, 18, R_2, ui_th.control);
        ui_tooltip(&ui, SP_3, py + 6, 32, 28, "Colour 1 (left click) and colour 2 (right click)");
        int sx0 = SP_3 + 32 + SP_5;
        for (int i = 0; i < NPAL; i++) {
            int sx2 = sx0 + i * (20 + SP_2), sy2 = py + (H_TOOLBAR - 20) / 2;
            bool hov = ui_hover(&ui, sx2 - SP_1, sy2 - SP_1, 24, 24);
            gfx_round_fill(c, sx2, sy2, 20, 20, R_2, palette[i]);
            gfx_round_rect(c, sx2, sy2, 20, 20, R_2, hov ? UI_TEXT : ui_th.line_strong);
            if (hov && ui.pressed) fg = palette[i];
            if (hov && ui.rpressed) bg = palette[i];
        }
        char st[64], lt[96];
        snprintf(st, sizeof st, "%d, %d", mx < 0 ? 0 : mx >= DOC_W ? DOC_W - 1 : mx, my < 0 ? 0 : my >= DOC_H ? DOC_H - 1 : my);
        snprintf(lt, sizeof lt, "%s, %d px \xc2\xb7 Left click paints colour 1, right click colour 2", TOOL_NAME[tool], brush);
        ui_status(&ui, 0, H - H_STATUS, W, lt, st);

        if (again) { again = false; ui.pressed = ui.released = ui.rpressed = false; goto redraw; }
        if (modal) {
            ui.mx = smx; ui.my = smy; ui.pressed = sp; ui.released = sr; ui.rpressed = srp;
            ui.nkeys = nk;
            memcpy(ui.keys, saved, sizeof saved);
        }
        if (saving) {
            int r = ui_prompt(&ui, "Save as PNG or BMP", "Save", savepath, sizeof savepath);
            if (r == 1) {
                gfx_text_center(ui.c, T_BODY_B, 0, H / 2 + 60, W, "Saving\xe2\x80\xa6", UI_TEXT);   /* the ext2 write takes a few seconds */
                ui_end(&ui);                                          /* show it before the slow write */
                if (save_doc(savepath)) { say("Saved %s", savepath); dirty = false; }
                else say("Couldn't write %s", savepath);
            }
            if (r) saving = false;
        } else if (opening) {
            int r = ui_prompt(&ui, "Open image", "Open", savepath, sizeof savepath);
            if (r == 1) { if (open_doc(savepath)) say("Opened %s", savepath); else say("Couldn't open %s", savepath); }
            if (r) opening = false;
        }
        if (again) { again = false; ui.pressed = ui.released = ui.rpressed = false; goto redraw; }   /* the open replaced the canvas */
        ui_end(&ui);
    } while (ui_poll(&ui, -1));
    gui_window_destroy(w);
    return 0;
}
