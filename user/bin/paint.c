/* paint - bitmap canvas with pencil, line, rectangle, ellipse, bucket fill, eraser, undo, BMP export */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscall.h>

#define DOC_W 800
#define DOC_H 520
#define UNDO_N 8

enum { T_PENCIL, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_ERASER, T_COUNT };

static uint32_t *doc;
static struct canvas dc;                      /* canvas over doc, so the gfx primitives clip for us */
static uint32_t *undo[UNDO_N];
static int nundo;
static bool dirty, again;                      /* again: a button changed the canvas after it was drawn */

static int tool = T_PENCIL, brush = 3;
static bool fill_shapes;
static color_t fg = RGB(0x1A, 0x10, 0x2E), bg = RGB(255, 255, 255);

static const color_t palette[] = {
    RGB(0, 0, 0), RGB(0x55, 0x55, 0x55), RGB(0x99, 0x99, 0x99), RGB(255, 255, 255),
    RGB(0xFF, 0x5F, 0x7A), RGB(0xE5, 0x1C, 0x23), RGB(0x8B, 0x1A, 0x1A), RGB(0xFF, 0x8A, 0x3D),
    RGB(0xFF, 0xC2, 0x4B), RGB(0xFF, 0xEB, 0x3B), RGB(0x9B, 0xE5, 0x64), RGB(0x4B, 0xE0, 0x8A),
    RGB(0x2E, 0xC5, 0xA0), RGB(0x1B, 0x7F, 0x4B), RGB(0x3D, 0x8B, 0xFD), RGB(0x1E, 0x4E, 0xB8),
    RGB(0x5A, 0x64, 0xFF), RGB(0x7C, 0x4D, 0xFF), RGB(0xB3, 0x88, 0xFF), RGB(0xFF, 0x7A, 0xC6),
    RGB(0xFF, 0x4F, 0xA3), RGB(0xA0, 0x52, 0x2D), RGB(0xD2, 0xB4, 0x8C), RGB(0x2A, 0x27, 0x3D),
};
#define NPAL ((int)(sizeof palette / sizeof palette[0]))

static char msg[128];
static uint32_t msg_until;
static bool msg_bad;
static char savepath[PATH_MAX] = "/home/drawing.bmp";
static bool saving;

static void say(bool bad, const char *fmt, const char *arg) {
    snprintf(msg, sizeof msg, fmt, arg);
    msg_bad = bad;
    msg_until = uptime_ms() + 4000;
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
    if (!nundo) { say(true, "%s", "Nothing to undo"); return; }
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

/* ---- tool icons ---- */
static void tool_icon(struct canvas *c, int t, int cx, int cy, color_t col) {
    switch (t) {
    case T_PENCIL:
        gfx_line_aa(c, cx - 8, cy + 8, cx + 6, cy - 6, 5.0f, col);
        gfx_line_aa(c, cx - 9, cy + 9, cx - 7, cy + 7, 3.0f, UI_ACCENT2);
        break;
    case T_LINE: gfx_line_aa(c, cx - 9, cy + 9, cx + 9, cy - 9, 2.4f, col); break;
    case T_RECT:
        gfx_line_aa(c, cx - 9, cy - 7, cx + 9, cy - 7, 2.2f, col);
        gfx_line_aa(c, cx + 9, cy - 7, cx + 9, cy + 7, 2.2f, col);
        gfx_line_aa(c, cx + 9, cy + 7, cx - 9, cy + 7, 2.2f, col);
        gfx_line_aa(c, cx - 9, cy + 7, cx - 9, cy - 7, 2.2f, col);
        break;
    case T_ELLIPSE: {
        float px = cx + 10, py = cy;
        for (int i = 1; i <= 24; i++) {
            float a = i * 2 * (float)M_PI / 24, x = cx + 10 * cosf(a), y = cy + 7 * sinf(a);
            gfx_line_aa(c, px, py, x, y, 2.2f, col);
            px = x; py = y;
        }
        break;
    }
    case T_FILL:                                  /* tilted bucket with a drop */
        gfx_line_aa(c, cx - 8, cy - 1, cx, cy - 9, 2.2f, col);
        gfx_line_aa(c, cx - 8, cy - 1, cx - 1, cy + 6, 2.2f, col);
        gfx_line_aa(c, cx, cy - 9, cx + 7, cy - 2, 2.2f, col);
        gfx_line_aa(c, cx - 1, cy + 6, cx + 7, cy - 2, 2.2f, col);
        gfx_circle_fill(c, cx + 9, cy + 6, 3, UI_ACCENT);
        break;
    case T_ERASER:
        gfx_round_fill(c, cx - 9, cy - 5, 18, 11, 3, col);
        gfx_round_fill(c, cx - 9, cy - 5, 7, 11, 3, UI_ACCENT2);
        break;
    }
}

static const char *const TOOL_NAME[] = { "Pencil", "Line", "Rectangle", "Ellipse", "Fill", "Eraser" };

int main(void) {
    struct gui_window *w = gui_window_create("Paint", 960, 680, 0);
    if (!w) { perror("paint"); return 1; }
    doc = malloc(DOC_W * DOC_H * 4);
    if (!doc) { perror("paint"); return 1; }
    for (int i = 0; i < DOC_W * DOC_H; i++) doc[i] = bg;
    canvas_init(&dc, doc, DOC_W, DOC_H, DOC_W);
    struct ui ui;
    ui_init(&ui, w);

    bool drawing = false;
    int dbtn = 1, lx = 0, ly = 0, sx = 0, sy = 0;     /* active button, last point, shape start */
    do {
        int W = w->w, H = w->h;
        int sideW = 88, palH = 72;
        bool modal = saving;
        int smx = ui.mx, smy = ui.my;
        bool sp = ui.pressed, sr = ui.released, srp = ui.rpressed;
        int nk = ui.nkeys;
        struct gui_event saved[32];
        memcpy(saved, ui.keys, sizeof saved);
        if (modal) { ui.mx = ui.my = -1; ui.pressed = ui.released = ui.rpressed = false; ui.nkeys = 0; }

        /* canvas placement */
        int ax = sideW, ay = 0, aw = W - sideW, ah = H - palH;
        int cx0 = ax + (aw > DOC_W + 24 ? (aw - DOC_W) / 2 : 12), cy0 = ay + (ah > DOC_H + 24 ? (ah - DOC_H) / 2 : 12);
        int mx = ui.mx - cx0, my = ui.my - cy0;
        bool over = ui_hover(&ui, ax, ay, aw, ah);

        /* keys */
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            if (k.mods & MOD_CTRL) {
                if (k.key == 'z') { do_undo(); drawing = false; }
                else if (k.key == 's') saving = true;
            } else if (k.ch == '[' && brush > 1) brush--;
            else if (k.ch == ']' && brush < 40) brush++;
            else if (k.ch >= '1' && k.ch <= '6') tool = k.ch - '1';
        }

        /* drawing interaction */
        if (!drawing && over && (ui.pressed || ui.rpressed) && mx >= -20 && my >= -20) {
            dbtn = ui.pressed ? 1 : 2;
            color_t col = tool == T_ERASER ? bg : dbtn == 1 ? fg : bg;
            push_undo();
            dirty = true;
            sx = lx = mx; sy = ly = my;
            if (tool == T_FILL) flood(mx, my, col);
            else {
                drawing = true;
                if (tool == T_PENCIL || tool == T_ERASER) stamp(mx, my, brush, col);
            }
        } else if (drawing) {
            color_t col = tool == T_ERASER ? bg : dbtn == 1 ? fg : bg;
            if (tool == T_PENCIL || tool == T_ERASER) {
                stroke(lx, ly, mx, my, brush, col);
            } else if (nundo) {                                   /* shapes: redraw from the pre-stroke snapshot */
                memcpy(doc, undo[nundo - 1], DOC_W * DOC_H * 4);
                if (tool == T_LINE) stroke(sx, sy, mx, my, brush, col);
                else if (tool == T_RECT) shape_rect(sx, sy, mx, my, brush, col, fill_shapes);
                else shape_ellipse(sx, sy, mx, my, brush, col, fill_shapes);
            }
            lx = mx; ly = my;
            if (ui.released || !(ui.buttons & 3)) drawing = false;
        }

        /* ---- draw ---- */
redraw:
        ui_clear(&ui);
        struct canvas *c = ui.c;
        /* canvas */
        gfx_fill(c, ax, ay, aw, ah, RGB(0x10, 0x0F, 0x1A));
        struct canvas cc = *c;
        canvas_clip(&cc, ax, ay, aw, ah);
        gfx_shadow(&cc, cx0, cy0, DOC_W, DOC_H, 4, 12, 140);
        gfx_blit(&cc, cx0, cy0, doc, DOC_W, DOC_H, DOC_W);
        gfx_rect(&cc, cx0 - 1, cy0 - 1, DOC_W + 2, DOC_H + 2, RGBA(255, 255, 255, 50));
        if (over && !modal && mx >= 0 && my >= 0 && mx < DOC_W && my < DOC_H && tool != T_FILL) {
            int r = brush <= 1 ? 1 : brush / 2;
            gfx_circle_fill(&cc, ui.mx, ui.my, r + 1, RGBA(255, 255, 255, 60));
            gfx_circle_fill(&cc, ui.mx, ui.my, r, RGBA(0x1A, 0x10, 0x2E, 90));
        }

        /* sidebar */
        gfx_fill(c, 0, 0, sideW, H - palH, UI_PANEL);
        gfx_fill(c, sideW - 1, 0, 1, H - palH, UI_BORDER);
        for (int t = 0; t < T_COUNT; t++) {
            int bx = 8 + (t % 2) * 38, by = 10 + (t / 2) * 40;
            bool hov = ui_hover(&ui, bx, by, 36, 36);
            if (ui.pressed && hov) tool = t;
            if (t == tool) gfx_round_fill(c, bx, by, 36, 36, 8, UI_ACCENT);
            else if (hov) gfx_round_fill(c, bx, by, 36, 36, 8, UI_HOVER);
            else gfx_round_fill(c, bx, by, 36, 36, 8, UI_SURFACE);
            tool_icon(c, t, bx + 18, by + 18, t == tool ? RGB(0x1A, 0x10, 0x2E) : UI_TEXT);
            if (hov) {                                          /* tooltip under the toolbar */
                int tw = gfx_text_width(&font_ui, TOOL_NAME[t]);
                gfx_round_fill(c, 8, 138, tw + 16, 24, 6, RGB(0x2A, 0x27, 0x3D));
                gfx_text(c, &font_ui, 16, 142, TOOL_NAME[t], UI_TEXT);
            }
        }
        gfx_text(c, &font_ui_bold, 12, 172, "Size", UI_TEXT_DIM);
        char b[16];
        snprintf(b, sizeof b, "%d", brush);
        gfx_text(c, &font_ui, sideW - 14 - gfx_text_width(&font_ui, b), 172, b, UI_TEXT);
        ui_slider(&ui, 16, 194, sideW - 36, &brush, 1, 40);
        /* brush preview */
        gfx_round_fill(c, 10, 222, sideW - 20, 44, 8, RGB(0xF4, 0xF2, 0xFF));
        {
            struct canvas pc = *c;
            canvas_clip(&pc, 10, 222, sideW - 20, 44);
            int r = brush <= 1 ? 1 : brush / 2;
            if (brush <= 1) gfx_pixel(&pc, sideW / 2, 244, fg);
            else if (brush == 2) gfx_fill(&pc, sideW / 2 - 1, 243, 2, 2, fg);
            else gfx_circle_fill(&pc, sideW / 2, 244, r > 20 ? 20 : r, fg);
        }
        if (tool == T_RECT || tool == T_ELLIPSE) ui_checkbox(&ui, 14, 280, "Filled", &fill_shapes);
        int by = 316;
        if (ui_button(&ui, 10, by, sideW - 20, 32, "Undo", UI_NORMAL_BTN)) { do_undo(); drawing = false; }
        if (ui_button(&ui, 10, by + 40, sideW - 20, 32, "Clear", UI_NORMAL_BTN)) clear_doc();
        if (ui_button(&ui, 10, by + 80, sideW - 20, 32, "Save", UI_ACCENT_BTN)) saving = true;

        /* palette bar */
        gfx_fill(c, 0, H - palH, W, palH, UI_PANEL);
        gfx_fill(c, 0, H - palH, W, 1, UI_BORDER);
        int py = H - palH + 10;
        gfx_round_fill(c, 36, py + 22, 32, 32, 6, bg);                          /* secondary, behind */
        gfx_round_rect(c, 36, py + 22, 32, 32, 6, RGBA(255, 255, 255, 80));
        gfx_round_fill(c, 14, py + 4, 36, 36, 6, fg);
        gfx_round_rect(c, 14, py + 4, 36, 36, 6, RGBA(255, 255, 255, 120));
        for (int i = 0; i < NPAL; i++) {
            int col = i % 12, row = i / 12;
            int sx2 = 92 + col * 30, sy2 = py + 4 + row * 30;
            bool hov = ui_hover(&ui, sx2, sy2, 26, 26);
            gfx_round_fill(c, sx2, sy2, 26, 26, 6, palette[i]);
            gfx_round_rect(c, sx2, sy2, 26, 26, 6, hov ? RGB(255, 255, 255) : RGBA(255, 255, 255, 50));
            if (hov && ui.pressed) fg = palette[i];
            if (hov && ui.rpressed) bg = palette[i];
        }
        char st[64];
        snprintf(st, sizeof st, "%d, %d", mx < 0 ? 0 : mx >= DOC_W ? DOC_W - 1 : mx, my < 0 ? 0 : my >= DOC_H ? DOC_H - 1 : my);
        gfx_text(c, &font_ui, W - 14 - gfx_text_width(&font_ui, st), H - palH + 12, st, UI_TEXT_DIM);
        if (msg[0] && uptime_ms() < msg_until) {
            int mw = gfx_text_width(&font_ui, msg);
            gfx_text(c, &font_ui, W - 14 - mw, H - palH + 40, msg, msg_bad ? UI_BAD : UI_GOOD);
        }
        gfx_text(c, &font_ui, 92 + 12 * 30 + 12, H - palH + 12, "Left click: colour 1   Right click: colour 2", UI_TEXT_DIM);

        if (again) { again = false; ui.pressed = ui.released = ui.rpressed = false; goto redraw; }
        if (modal) {
            ui.mx = smx; ui.my = smy; ui.pressed = sp; ui.released = sr; ui.rpressed = srp;
            ui.nkeys = nk;
            memcpy(ui.keys, saved, sizeof saved);
        }
        if (saving) {
            int r = ui_prompt(&ui, "Save as BMP", "Save", savepath, sizeof savepath);
            if (r == 1) {
                gfx_text_center(ui.c, &font_ui_bold, 0, H / 2 + 60, W, "Saving...", UI_TEXT);   /* the ext2 write takes a few seconds */
                ui_end(&ui);                                          /* show it before the slow write */
                if (save_bmp(savepath)) { say(false, "Saved %s", savepath); dirty = false; }
                else say(true, "Could not write %s", savepath);
            }
            if (r) saving = false;
        }
        ui_end(&ui);
    } while (ui_poll(&ui, -1));
    gui_window_destroy(w);
    return 0;
}
