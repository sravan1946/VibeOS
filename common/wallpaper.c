/* wallpaper.c - procedural desktop backgrounds (integer math only); shared by the compositor and Settings, which
 * renders thumbnails. "Dunes" is the redesign's default (vibeos-design mockups/src/wallpapers.py, direction A, drawn
 * at 1280x800 and cover-scaled); the others are soft blobs in calm neutrals. An ordered dither hides gradient banding. */
#include "wallpaper.h"

#define WP_LEN(a) (int)(sizeof(a) / sizeof((a)[0]))
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

struct blob { int x, y, r; int cr, cg, cb; int k; };   /* k: intensity /256 */

/* each blob theme: background gradient top/bottom, then the five blob colours + intensities */
static const struct wp {
    const char *name;
    uint32_t top, bot, col[5];
    int k[5];
} themes[] = {
    { "Dunes" },
    { "Graphite", 0x121315, 0x1A1B1E, { 0x45484E, 0x34363B, 0x2C2D31, 0x3A3C41, 0x232427 }, { 70, 60, 60, 40, 30 } },
    { "Slate",    0x111518, 0x191E23, { 0x5B6B7A, 0x3E4A56, 0x4A5663, 0x2F3A44, 0x34404B }, { 60, 60, 50, 40, 30 } },
    { "Sand",     0x15130F, 0x1E1A15, { 0x8A6644, 0x5A4330, 0x4E3C2D, 0x3B2E22, 0x2E251E }, { 45, 60, 50, 40, 30 } },
    { "Sage",     0x111412, 0x171C19, { 0x557A60, 0x3A4A3F, 0x46574B, 0x2F3A34, 0x26302A }, { 50, 60, 50, 40, 30 } },
};
int wallpaper_count(void) { return WP_LEN(themes); }
const char *wallpaper_name(int i) { return themes[i % WP_LEN(themes)].name; }

/* ---- Dunes: coordinates in 1/16 px of the 1280x800 design ---- */
struct grad { int y0, y1, n; int t[4]; uint32_t c[4]; };   /* stops t in 0..256 over [y0,y1] (design px) */
static const struct grad g_sky = { 0, 800, 4, { 0, 77, 133, 154 }, { 0x141517, 0x1A1B1C, 0x2B2622, 0x4A3726 } };
static const struct grad g_far = { 465, 800, 2, { 0, 256 }, { 0x4E3C2D, 0x2E251E } };
static const struct grad g_lit = { 387, 800, 3, { 0, 90, 256 }, { 0x8A6644, 0x5A4330, 0x2A2019 } };
static const struct grad g_lee = { 387, 700, 2, { 0, 256 }, { 0x2A211B, 0x110E0C } };
static const struct grad g_lit2 = { 604, 800, 2, { 0, 256 }, { 0x5C4532, 0x1C1612 } };
static const struct grad g_lee2 = { 604, 800, 2, { 0, 256 }, { 0x1A1512, 0x0C0A09 } };

static void gcol(const struct grad *g, int y16, int *rgb) {    /* 8.8 fixed channels */
    int t = (y16 - g->y0 * 16) * 16 / (g->y1 - g->y0);
    int i = 0;
    while (i < g->n - 1 && t > g->t[i + 1]) i++;
    uint32_t a = g->c[i], b = g->c[i + 1 < g->n ? i + 1 : i];
    int span = i + 1 < g->n ? g->t[i + 1] - g->t[i] : 1, f = t < g->t[i] ? 0 : t - g->t[i];
    if (f > span) f = span;
    for (int k = 0; k < 3; k++) {
        int ca = a >> (16 - 8 * k) & 0xFF, cb = b >> (16 - 8 * k) & 0xFF;
        rgb[k] = (ca << 8) + (cb - ca) * 256 * f / span;
    }
}

/* y (1/16 px) of a cubic bezier with x monotonic in t, at x16; p = x0,y0 .. x3,y3 in px */
static int bez(const short *p, int x16) {
    int lo = 0, hi = 4096, up = p[6] > p[0];
    int64_t y = 0;
    for (int it = 0; it < 13; it++) {
        int64_t t = (lo + hi) / 2, u = 4096 - t;
        int64_t w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        int64_t x = (w0 * p[0] + w1 * p[2] + w2 * p[4] + w3 * p[6]) >> 32;   /* /4096^3 * 16 */
        y = (w0 * p[1] + w1 * p[3] + w2 * p[5] + w3 * p[7]) >> 32;
        if ((x < x16) == up) lo = (int)t; else hi = (int)t;
    }
    return (int)y;
}

static int isin(int a) {                                 /* a in 1/4096 turn -> sin * 4096 (Bhaskara) */
    a &= 4095;
    int s = a >= 2048 ? -1 : 1;
    int64_t h = a & 2047, q = h * (2048 - h);
    return s * (int)(4096 * 4 * q / (5 * 2048 * 2048 / 4 - q));
}

static const short crest1[8] = { -20, 560, 240, 500, 520, 420, 800, 392 }, crest2[8] = { 800, 392, 960, 376, 1120, 404, 1300, 452 };
static const short leeb[8] = { 1300, 700, 1150, 600, 980, 470, 800, 392 };
static const short near1[8] = { -20, 690, 200, 640, 420, 600, 620, 604 }, near2[8] = { 620, 604, 820, 608, 1060, 660, 1300, 640 };
static const short nlee[8] = { 620, 604, 480, 650, 260, 720, -20, 800 };

static void over(int *c, const int *s, int cov) {      /* cov 0..256 */
    for (int k = 0; k < 3; k++) c[k] += (s[k] - c[k]) * cov / 256;
}

static void dunes(struct canvas *c) {
    int W = c->w, H = c->h;
    int sc = W * 256 / 1280 > H * 256 / 800 ? W * 256 / 1280 : H * 256 / 800;   /* cover scale, 8.8 */
    int ox = (W * 256 - 1280 * sc) / 2, oy = (H * 256 - 800 * sc) / 2;        /* 8.8 screen px */
    for (int x = 0; x < W; x++) {
        int X16 = (int)(((int64_t)x * 256 + 128 - ox) * 16 / sc);
        int b[6];                                        /* boundaries in screen 1/16 px: far, crest, lee bottom, near, near lee */
        int yfar = 470 * 16 + 160 * isin(X16 * 1695 / 16000) / 4096 + 80 * isin(X16 * 4563 / 16000 + 652) / 4096;
        int ycr = X16 < 800 * 16 ? bez(crest1, X16) : bez(crest2, X16);
        int ylee = X16 < 800 * 16 ? ycr : bez(leeb, X16);
        int ynr = X16 < 620 * 16 ? bez(near1, X16) : bez(near2, X16);
        int ynl = X16 < 620 * 16 ? bez(nlee, X16) : ynr;
        int d[5] = { yfar, ycr, ylee, ynr, ynl };
        for (int i = 0; i < 5; i++) b[i] = (int)(((int64_t)d[i] * sc + oy * 16) / 256);
        for (int y = 0; y < H; y++) {
            int Y16 = (int)(((int64_t)y * 256 + 128 - oy) * 16 / sc), y16 = y * 16;
            int cov[5];                                  /* how much of this pixel row lies below each boundary */
            for (int i = 0; i < 5; i++) { int v = y16 + 16 - b[i]; cov[i] = v <= 0 ? 0 : v >= 16 ? 256 : v * 16; }
            int col[3], s[3];
            gcol(&g_sky, Y16, col);
            if (cov[0]) { gcol(&g_far, Y16, s); over(col, s, cov[0]); }
            if (cov[1]) { gcol(&g_lit, Y16, s); over(col, s, cov[1]); }
            if (cov[1] > cov[2]) { gcol(&g_lee, Y16, s); over(col, s, cov[1] - cov[2]); }
            int dy = y16 + 8 - b[1], hw = 10 * sc / 256 + 8;   /* the lit crest line, ~1.2 px */
            if (dy < 0) dy = -dy;
            if (dy < hw) { int cr[3] = { 0xB0 << 8, 0x84 << 8, 0x58 << 8 }; over(col, cr, 140 * (hw - dy) / hw); }
            if (cov[3]) { gcol(&g_lit2, Y16, s); over(col, s, cov[3]); }
            if (cov[3] > cov[4]) { gcol(&g_lee2, Y16, s); over(col, s, cov[3] - cov[4]); }
            int n = bayer[y & 3][x & 3] * 16 + 8;
            for (int k = 0; k < 3; k++) col[k] += ((128 << 8) - col[k]) * 12 / 256;   /* the mockup's grain layer, averaged */
            c->px[y * c->stride + x] = RGB(clamp255((col[0] + n) >> 8), clamp255((col[1] + n) >> 8), clamp255((col[2] + n) >> 8));
        }
    }
}

void wallpaper_render(struct canvas *c, int theme) {
    const struct wp *th = &themes[theme % WP_LEN(themes)];
    if (!th->top) { dunes(c); return; }
    int W = c->w, H = c->h;
    static const int pos[5][3] = { { 18, 22, 55 }, { 88, 12, 42 }, { 72, 100, 55 }, { 5, 95, 38 }, { 50, 55, 30 } };
    struct blob b[5];
    for (int i = 0; i < 5; i++)
        b[i] = (struct blob){ W * pos[i][0] / 100, H * pos[i][1] / 100, W * pos[i][2] / 100,
                              th->col[i] >> 16, th->col[i] >> 8 & 0xFF, th->col[i] & 0xFF, th->k[i] };
    int tr = th->top >> 16, tg = th->top >> 8 & 0xFF, tb = th->top & 0xFF;
    int dr = (int)(th->bot >> 16) - tr, dg = (int)(th->bot >> 8 & 0xFF) - tg, db = (int)(th->bot & 0xFF) - tb;
    for (int y = 0; y < H; y++) {
        int br = (tr << 8) + (dr << 8) * y / H, bg = (tg << 8) + (dg << 8) * y / H, bb = (tb << 8) + (db << 8) * y / H;
        uint32_t *row = &c->px[y * c->stride];
        for (int x = 0; x < W; x++) {
            int r = br, g = bg, bl = bb;                         /* 8.8 fixed point */
            for (int i = 0; i < WP_LEN(b); i++) {
                int dx = x - b[i].x, dy = (y - b[i].y) * 5 / 4;     /* slightly squashed ellipses */
                int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy, r2 = (int64_t)b[i].r * b[i].r;
                if (d2 >= r2) continue;
                int t = (int)(256 - d2 * 256 / r2);                 /* 0..256 */
                int f = t * t / 256 * b[i].k / 256;                 /* smooth falloff */
                r += b[i].cr * f; g += b[i].cg * f; bl += b[i].cb * f;
            }
            int n = bayer[y & 3][x & 3] * 16 + 8;
            row[x] = RGB(clamp255((r + n) >> 8), clamp255((g + n) >> 8), clamp255((bl + n) >> 8));
        }
    }
}
