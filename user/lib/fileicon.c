/* fileicon.c - see fileicon.h */
#include "fileicon.h"
#include <string.h>
#include <ctype.h>

static const color_t kcol[] = {                      /* by FK_*: accent of the badge / glyph */
    [FK_DIR] = RGB(0xFF, 0xC2, 0x4B), [FK_EXE] = RGB(0x7C, 0x4D, 0xFF), [FK_TEXT] = RGB(0x5B, 0x8C, 0xFF),
    [FK_CODE] = RGB(0x2E, 0xC5, 0xA0), [FK_IMAGE] = RGB(0xE8, 0x5D, 0x9F), [FK_AUDIO] = RGB(0xFF, 0x8A, 0x3D),
    [FK_HTML] = RGB(0x24, 0xB8, 0xE8), [FK_FILE] = RGB(0x8E, 0x88, 0xB5),
};

void fileicon(struct canvas *c, int kind, int x, int y, int s, const char *name) {
    color_t a = kcol[kind];
    int r = s / 6 + 1;
    if (kind == FK_DIR) {
        color_t body = RGB(0xFF, 0xC2, 0x4B), back = RGB(0xE0, 0x9A, 0x2B);
        gfx_round_fill(c, x, y + s / 10, s * 2 / 5, s / 4, r / 2 + 1, back);
        gfx_round_fill(c, x, y + s / 5, s, s * 7 / 10, r, back);
        gfx_round_fill(c, x, y + s * 3 / 10, s, s * 3 / 5, r, body);
        gfx_fill(c, x + 2, y + s * 3 / 10 + 1, s - 4, 1, RGBA(255, 255, 255, 90));
        return;
    }
    if (kind == FK_EXE || kind == FK_AUDIO) {
        color_t hi = kind == FK_EXE ? RGB(0x95, 0x6C, 0xFF) : RGB(0xFF, 0xA8, 0x66);
        gfx_round_fill(c, x + s / 20, y + s / 20, s * 9 / 10, s * 9 / 10, r + 1, a);
        gfx_round_fill(c, x + s / 20, y + s / 20, s * 9 / 10, s * 9 / 20, r + 1, hi);
        int cx = x + s / 2, cy = y + s / 2, g = s / 4;
        color_t w = RGB(255, 255, 255);
        if (kind == FK_EXE) {                               /* play triangle */
            for (int i = -g; i <= g; i++) gfx_fill(c, cx - g / 2, cy + i, (g - (i < 0 ? -i : i)) * 3 / 2 + 1, 1, w);
        } else {                                            /* eighth note */
            int sw = s / 14 + 1, hx = cx - g / 2, hy = cy + g, sx = hx + g / 2 - sw + 1;
            gfx_circle_fill(c, hx, hy, g / 2 + 1, w);
            gfx_fill(c, sx, cy - g, sw, hy - cy + g, w);
            gfx_fill(c, sx, cy - g, g, sw + 1, w);
        }
        return;
    }
    if (kind == FK_IMAGE) {
        gfx_round_fill(c, x + s / 12, y + s / 8, s * 5 / 6, s * 3 / 4, r, RGB(0x3B, 0x4A, 0x8A));
        gfx_round_fill(c, x + s / 12, y + s / 8, s * 5 / 6, s * 3 / 8, r, RGB(0x6A, 0x8C, 0xE8));
        gfx_circle_fill(c, x + s * 2 / 3, y + s * 3 / 8, s / 9 + 1, RGB(0xFF, 0xE0, 0x8A));
        int bx = x + s / 12, by = y + s * 7 / 8, bw = s * 5 / 6;                                  /* hills */
        for (int i = 0; i < bw; i++) {
            int d = i < bw / 2 ? i : bw - 1 - i, h = d * 2 / 3 + s / 10;
            if (h > s / 3) h = s / 3;
            gfx_fill(c, bx + i, by - h, 1, h - (i < r || i >= bw - r ? 1 : 0), RGB(0x3E, 0xB8, 0x8A));
        }
        return;
    }
    /* page with a folded corner; a badge with the extension on big icons, text lines on small ones */
    int pw = s * 3 / 4, px = x + (s - pw) / 2, fold = s / 4 + 1;
    gfx_round_fill(c, px, y, pw, s, r, RGB(0xD9, 0xD5, 0xF0));
    gfx_fill(c, px + pw - fold, y, fold, fold, RGB(0x8E, 0x88, 0xB5));
    gfx_fill(c, px + pw - fold, y, fold - 1, 1, RGB(0xD9, 0xD5, 0xF0));
    if (s >= 40) {
        char e[8] = "";
        const char *b = name ? strrchr(name, '.') : 0;
        if (b && b != name) { int i = 0; for (b++; *b && i < 4; b++) e[i++] = toupper((unsigned char)*b); e[i] = 0; }
        for (int i = 0; i < 3; i++) gfx_fill(c, px + s / 8, y + s / 6 + i * (s / 9), pw - s / 4 - (i == 2) * (pw / 3), 2, RGB(0xA9, 0xA4, 0xC8));
        int bh = s / 4 + 2, by = y + s - bh - s / 12;
        if (e[0]) gfx_round_fill(c, px - s / 12, by, pw + s / 6, bh, 4, a);
        if (e[0]) gfx_text_center(c, &font_ui_bold, px - s / 12, by + (bh - font_ui_bold.height) / 2, pw + s / 6, e, RGB(255, 255, 255));
    } else {
        for (int i = 0; i < 4; i++) gfx_fill(c, px + s / 6, y + s * 2 / 5 + i * (s / 6), pw - s / 3 - (i == 3) * (pw / 3), 1 + s / 24, a);
    }
}
