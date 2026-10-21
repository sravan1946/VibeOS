/* wallpaper.c - procedural "aurora" backdrop, rendered once at boot (integer math only) */
#include "kernel.h"
#include "gfx.h"

struct blob { int x, y, r; int cr, cg, cb; int k; };   /* k: intensity /256 */

/* each theme: background gradient top/bottom, then the five blob colours + intensities */
static const struct theme {
    const char *name;
    uint32_t top, bot, col[5];
    int k[5];
} themes[] = {
    { "Aurora",   0x0B0A1A, 0x160E2C, { 0x7C4DFF, 0xFF4FA3, 0xFF8A3D, 0x2EC5FF, 0x9D5CFF }, { 150, 120, 110, 70, 40 } },
    { "Ocean",    0x050B1A, 0x08182C, { 0x3D5AFE, 0x00C2FF, 0x2EE6D6, 0x7C4DFF, 0x1E88E5 }, { 150, 120, 100, 60, 50 } },
    { "Sunset",   0x150812, 0x24101A, { 0xFF3D7F, 0xFFC24B, 0xFF6B3D, 0x8E2DE2, 0xFF8A65 }, { 140, 110, 130, 80, 40 } },
    { "Forest",   0x06110D, 0x0C1E16, { 0x16A085, 0xA3E635, 0x2ECC71, 0x0EA5E9, 0x22C55E }, { 140, 90, 110, 70, 40 } },
    { "Midnight", 0x07070E, 0x0E0E1C, { 0x4C3A8A, 0x6B4FA0, 0x2B2B5F, 0x1F3B70, 0x3D2E6B }, { 120, 100, 90, 70, 40 } },
};
int wallpaper_count(void) { return ARRAY_LEN(themes); }
const char *wallpaper_name(int i) { return themes[i % ARRAY_LEN(themes)].name; }

void wallpaper_render(struct canvas *c, int theme) {
    const struct theme *th = &themes[theme % ARRAY_LEN(themes)];
    int W = c->w, H = c->h;
    static const int pos[5][3] = { { 18, 22, 55 }, { 88, 12, 42 }, { 72, 100, 55 }, { 5, 95, 38 }, { 50, 55, 30 } };
    struct blob b[5];
    for (int i = 0; i < 5; i++)
        b[i] = (struct blob){ W * pos[i][0] / 100, H * pos[i][1] / 100, W * pos[i][2] / 100,
                              th->col[i] >> 16, th->col[i] >> 8 & 0xFF, th->col[i] & 0xFF, th->k[i] };
    int tr = th->top >> 16, tg = th->top >> 8 & 0xFF, tb = th->top & 0xFF;
    int dr = (int)(th->bot >> 16) - tr, dg = (int)(th->bot >> 8 & 0xFF) - tg, db = (int)(th->bot & 0xFF) - tb;
    uint32_t seed = 0x9E3779B9;
    for (int y = 0; y < H; y++) {
        int br = tr + dr * y / H, bg = tg + dg * y / H, bb = tb + db * y / H;
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
