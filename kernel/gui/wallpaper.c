/* wallpaper.c - procedural "aurora" backdrop, rendered once at boot (integer math only) */
#include "kernel.h"
#include "gfx.h"

struct blob { int x, y, r; int cr, cg, cb; int k; };   /* k: intensity /256 */

void wallpaper_render(struct canvas *c) {
    int W = c->w, H = c->h;
    struct blob b[] = {
        { W * 18 / 100, H * 22 / 100, W * 55 / 100, 0x7C, 0x4D, 0xFF, 150 },
        { W * 88 / 100, H * 12 / 100, W * 42 / 100, 0xFF, 0x4F, 0xA3, 120 },
        { W * 72 / 100, H * 100 / 100, W * 55 / 100, 0xFF, 0x8A, 0x3D, 110 },
        { W * 5 / 100,  H * 95 / 100, W * 38 / 100, 0x2E, 0xC5, 0xFF, 70 },
        { W * 50 / 100, H * 55 / 100, W * 30 / 100, 0x9D, 0x5C, 0xFF, 40 },
    };
    uint32_t seed = 0x9E3779B9;
    for (int y = 0; y < H; y++) {
        int br = 0x0B + (0x16 - 0x0B) * y / H, bg = 0x0A + (0x0E - 0x0A) * y / H, bb = 0x1A + (0x2C - 0x1A) * y / H;
        uint32_t *row = &c->px[y * c->stride];
        for (int x = 0; x < W; x++) {
            int r = br << 8, g = bg << 8, bl = bb << 8;            /* 8.8 fixed point */
            for (unsigned i = 0; i < ARRAY_LEN(b); i++) {
                int dx = x - b[i].x, dy = (y - b[i].y) * 5 / 4;     /* slightly squashed ellipses */
                int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy, r2 = (int64_t)b[i].r * b[i].r;
                if (d2 >= r2) continue;
                int t = (int)(256 - d2 * 256 / r2);                 /* 0..256 */
                int f = t * t / 256 * b[i].k / 256;                 /* smooth falloff */
                r += b[i].cr * f; g += b[i].cg * f; bl += b[i].cb * f;
            }
            seed = seed * 1664525 + 1013904223;                     /* film grain kills banding */
            int n = (int)(seed >> 24) - 128;
            r = (r + n) >> 8; g = (g + n) >> 8; bl = (bl + n) >> 8;
            row[x] = RGB(MIN(255, MAX(0, r)), MIN(255, MAX(0, g)), MIN(255, MAX(0, bl)));
        }
    }
    /* soft watermark */
    const char *mark = "VibeOS";
    int tw = gfx_text_width(&font_big, mark);
    gfx_text(c, &font_big, W - tw - 48, H - 44 - font_big.height - 26, mark, RGBA(255, 255, 255, 26));
    gfx_text(c, &font_ui, W - gfx_text_width(&font_ui, "a vibe-coded operating system") - 50, H - 44 - 30,
             "a vibe-coded operating system", RGBA(255, 255, 255, 40));
}
