/* gfxtest.c - host tests for the gfx.c design primitives (run.sh). Pixels are read back and compared; the last test
 * draws a contact sheet in the graphite theme so the shapes can be eyeballed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gfx.h"
#include "design.h"
#include "theme.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define W 640
#define HT 420
static uint32_t buf[W * HT];
static struct canvas cv;
#define BG 0xFF000000u
#define FG 0xFFFFFFFFu
static void clear(color_t c) { canvas_init(&cv, buf, W, HT, W); for (int i = 0; i < W * HT; i++) buf[i] = c; }
static uint32_t px(int x, int y) { return buf[y * W + x]; }
static int red(int x, int y) { return px(x, y) >> 16 & 255; }
static int untouched_outside(int x, int y, int w, int h) {      /* every pixel outside the rect is still BG */
    for (int j = 0; j < HT; j++)
        for (int i = 0; i < W; i++)
            if ((i < x || i >= x + w || j < y || j >= y + h) && px(i, j) != BG) return 0;
    return 1;
}

static void test_hairlines(void) {
    clear(BG);
    gfx_hline(&cv, 10, 20, 30, FG);
    gfx_vline(&cv, 50, 5, 12, FG);
    CHECK(px(10, 20) == FG && px(39, 20) == FG && px(40, 20) == BG && px(9, 20) == BG && px(20, 19) == BG && px(20, 21) == BG);
    CHECK(px(50, 5) == FG && px(50, 16) == FG && px(50, 17) == BG && px(49, 10) == BG && px(51, 10) == BG);
    canvas_clip(&cv, 0, 0, 25, HT);
    gfx_hline(&cv, 10, 30, 30, FG);
    CHECK(px(24, 30) == FG && px(25, 30) == BG);
}

static void test_corners(void) {
    int x = 40, y = 40, w = 60, h = 40;
    clear(BG);
    gfx_rrect_fill(&cv, x, y, w, h, RADII(0, 8, 0, 8), FG);
    CHECK(px(x, y) == FG && px(x + w - 1, y + h - 1) == FG);                 /* square corners are solid */
    CHECK(px(x + w - 1, y) == BG && px(x, y + h - 1) == BG);                 /* round corners cut */
    CHECK(px(x + w / 2, y + h / 2) == FG && untouched_outside(x, y, w, h));
    CHECK(red(x + w - 1, y + 8) == 255 && red(x + w - 9, y) == 255 && red(x + w - 8, y) < 255);         /* the arc meets the straight edges flush */
    /* all-round: four-way symmetric */
    clear(BG);
    gfx_round_fill(&cv, x, y, w, h, 8, FG);
    int sym = 1;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (px(x + i, y + j) != px(x + w - 1 - i, y + j) || px(x + i, y + j) != px(x + i, y + h - 1 - j)) sym = 0;
    CHECK(sym);
    /* a radius past the cache (slow path) and one clamped to a pill still behave */
    clear(BG);
    gfx_round_fill(&cv, 100, 100, 100, 100, 50, FG);
    CHECK(px(100, 100) == BG && px(150, 150) == FG && px(100, 150) != BG && untouched_outside(100, 100, 100, 100));
    clear(BG);
    gfx_round_fill(&cv, 10, 10, 36, 20, 99, FG);                             /* switch track: R_F */
    CHECK(px(10, 20) != BG && px(10, 10) == BG && px(28, 10) == FG);
}

/* ring(t) + fill(inset t, r - t) == fill(r), per pixel (coverages add: drawn into separate buffers) */
static void test_ring_partition(void) {
    static uint32_t a[W * HT], b[W * HT];
    for (int r = 0; r <= 12; r += 4)
        for (int t = 1; t <= 2; t++) {
            int x = 30, y = 30, w = 50, h = 34, worst = 0, ri = r > t ? r - t : 0;
            clear(BG);
            gfx_rrect_fill(&cv, x, y, w, h, RADII(r, r, r, r), FG);
            memcpy(a, buf, sizeof buf);
            clear(BG);
            gfx_rrect_fill(&cv, x + t, y + t, w - 2 * t, h - 2 * t, RADII(ri, ri, ri, ri), FG);
            memcpy(b, buf, sizeof buf);
            clear(BG);
            gfx_rrect_ring(&cv, x, y, w, h, RADII(r, r, r, r), t, FG);
            for (int i = 0; i < W * HT; i++) {
                int d = (int)(a[i] >> 16 & 255) - (int)(buf[i] >> 16 & 255) - (int)(b[i] >> 16 & 255);
                if (abs(d) > worst) { worst = abs(d); if (getenv("GFXDBG")) printf("  at %d,%d a=%d ring=%d b=%d\n", i % W, i / W, (int)(a[i] >> 16 & 255), (int)(buf[i] >> 16 & 255), (int)(b[i] >> 16 & 255)); }
            }
            CHECK(worst <= 3);
            if (worst > 3) printf("  r=%d t=%d off by %d\n", r, t, worst);
        }
}

static void test_rings(void) {
    int x = 100, y = 80, w = 80, h = 28, mx = x + w / 2;
    clear(BG);
    gfx_round_rect(&cv, x, y, w, h, R_2, FG);                                /* 1px inset */
    CHECK(px(mx, y) == FG && px(mx, y + h - 1) == FG && px(x, y + h / 2) == FG && px(x + w - 1, y + h / 2) == FG);
    CHECK(px(mx, y + 1) == BG && px(mx, y + h / 2) == BG && untouched_outside(x, y, w, h));
    clear(BG);
    gfx_ring_out(&cv, x, y, w, h, R_2, FG);                                  /* 1px outside */
    CHECK(px(mx, y - 1) == FG && px(mx, y) == BG && px(x - 1, y + h / 2) == FG && px(x, y + h / 2) == BG);
    CHECK(px(mx, y + h) == FG && px(mx, y + h - 1) == BG && untouched_outside(x - 1, y - 1, w + 2, h + 2));
    clear(BG);
    gfx_focus_ring(&cv, x, y, w, h, R_2, FG);                                /* gap 1, 2px ring */
    CHECK(px(mx, y) == BG && px(mx, y - 1) == BG && px(mx, y - 2) == FG && px(mx, y - 3) == FG && px(mx, y - 4) == BG);
    CHECK(px(x + w, y + h / 2) == BG && px(x + w + 1, y + h / 2) == FG && px(x + w + 2, y + h / 2) == FG && px(x + w + 3, y + h / 2) == BG);
    CHECK(px(mx, y + h / 2) == BG && untouched_outside(x - 3, y - 3, w + 6, h + 6));
    /* the corner follows the control's radius: the ring's diagonal pixel sits off the box corner */
    CHECK(px(x - 3, y - 3) == BG && red(x - 1, y - 1) > 0);
}

static int shade(int x, int y) { return 255 - red(x, y); }                   /* darkness of a shadow over white */

static void test_shadow(void) {
    const color_t white = 0xFFFFFFFFu;
    int x = 200, y = 120, w = 200, h = 120, r = R_3;
    const struct elevation e = ELEV[E3];
    int reach = 3 * (e.blur / 2), alpha = 102;
    clear(white);
    gfx_elev(&cv, x, y, w, h, r, E3, alpha);
    int my = y + h / 2, mx = x + w / 2;
    CHECK(px(mx, my) == white && px(x + 2, my) == white);                    /* the caster's own pixels are skipped */
    CHECK(px(x - reach - 1, my) == white && px(x + w + reach, my) == white); /* nothing past the blur reach */
    CHECK(px(mx, y + e.dy + h + reach) == white && px(mx, y + e.dy - reach - 1) == white);
    int mono = 1, symm = 1;
    for (int d = 1; d < reach; d++) {
        if (shade(x - d, my) < shade(x - d - 1, my)) mono = 0;
        if (shade(x - d, my) != shade(x + w - 1 + d, my)) symm = 0;
    }
    CHECK(mono && symm);
    CHECK(shade(x - 1, my) > 20 && shade(x - 1, my) <= alpha);               /* close to the edge: about half the peak */
    CHECK(shade(mx, y + h + 4) > shade(mx, y - 4));                          /* offset down: heavier below than above */
    CHECK(shade(x - 6, y - 6) < shade(x - 6, my));                           /* corners are lighter than edges */
    /* every level renders, deeper levels reach further */
    int prev = 0;
    for (int lv = E1; lv <= E4; lv++) {
        clear(white);
        gfx_elev(&cv, x, y, w, h, r, lv, 128);
        int n = 0;
        for (int i = 0; i < W * HT; i++) n += buf[i] != white;
        CHECK(n > prev);
        prev = n;
    }
    /* small casters (thumbs: R_F, E1) and maximised (r 0) still work */
    clear(white);
    gfx_elev(&cv, 50, 50, SWITCH_THUMB, SWITCH_THUMB, R_F(SWITCH_THUMB), E1, 77);
    CHECK(shade(50 + 7, 50 + SWITCH_THUMB + 1) > shade(50 + 7, 49) && shade(50 + 7, 49) >= 0 && px(50 + 7, 50 + SWITCH_THUMB + 4) == white);
    clear(white);
    gfx_elev(&cv, 50, 50, 300, 200, 0, E2, 92);
    CHECK(shade(49, 150) > 0);
    /* per-frame cost: a focused 800x600 window's shadow */
    static uint32_t big[1280 * 800];
    struct canvas bc;
    canvas_init(&bc, big, 1280, 800, 1280);
    clock_t t0 = clock();
    for (int i = 0; i < 200; i++) gfx_elev(&bc, 200, 100, 800, 600, R_3, E3, 102);
    double ms = (double)(clock() - t0) * 1000 / CLOCKS_PER_SEC / 200;
    printf("gfxtest: E3 shadow under 800x600: %.3f ms/frame\n", ms);
    CHECK(ms < 5);
}

static void test_mask(void) {
    static const uint8_t m[3 * 2] = { 0, 128, 255, 255, 0, 64 };
    clear(BG);
    gfx_mask(&cv, 10, 10, m, 3, 2, 3, 0xFFE8A33Du);
    CHECK(px(10, 10) == BG && px(12, 10) == 0xFFE8A33Du && px(10, 11) == 0xFFE8A33Du && px(11, 11) == BG);
    CHECK(red(11, 10) > 0x60 && red(11, 10) < 0x8A && px(13, 10) == BG);
    canvas_clip(&cv, 0, 0, 11, HT);
    gfx_mask(&cv, 10, 20, m + 2, 1, 1, 1, 0x80FFFFFFu);                     /* clip + half-alpha tint */
    CHECK(red(10, 20) >= 126 && red(10, 20) <= 129);
    clear(BG);
    CHECK(gfx_char(&cv, &font_ui, 20, 20, 'H', FG) > 0);
    int n = 0;
    for (int i = 0; i < W * HT; i++) n += buf[i] != BG;
    CHECK(n > 10);
}

/* the contact sheet: E1-E4 over the graphite base, rings, a focused button, a tinted mask */
static void sheet(const char *path) {
    const struct theme *t = theme_get(0);
    clear(t->base);
    for (int lv = E1; lv <= E4; lv++) {
        int x = 24 + (lv - 1) * 150, y = 32;
        gfx_elev(&cv, x, y, 120, 90, R_3, lv, t->elev[lv]);
        gfx_round_fill(&cv, x, y, 120, 90, R_3, t->overlay);
        gfx_round_rect(&cv, x, y, 120, 90, R_3, t->line);
        char s[4] = { 'E', (char)('0' + lv), 0 };
        gfx_text(&cv, &font_ui, x + 12, y + 12, s, t->text);
    }
    const struct theme *l = theme_get(1);
    gfx_fill(&cv, 0, 170, W, HT - 170, l->base);
    for (int lv = E1; lv <= E4; lv++) {
        int x = 24 + (lv - 1) * 150, y = 196;
        gfx_elev(&cv, x, y, 120, 90, R_3, lv, l->elev[lv]);
        gfx_round_fill(&cv, x, y, 120, 90, R_3, l->overlay);
    }
    /* controls: secondary + primary buttons, the primary focused, a switch, per-corner tab */
    int by = 330;
    gfx_round_fill(&cv, 24, by, 96, H_CTL, R_2, l->raised);
    gfx_round_rect(&cv, 24, by, 96, H_CTL, R_2, l->line_strong);
    gfx_round_fill(&cv, 136, by, 96, H_CTL, R_2, l->accent);
    gfx_round_rect(&cv, 136, by, 96, H_CTL, R_2, l->accent_line);
    gfx_focus_ring(&cv, 136, by, 96, H_CTL, R_2, l->accent_text);
    gfx_round_fill(&cv, 256, by + 4, SWITCH_W, SWITCH_H, R_F(SWITCH_H), l->accent);
    gfx_elev(&cv, 256 + 19, by + 7, SWITCH_THUMB, SWITCH_THUMB, R_F(SWITCH_THUMB), E1, l->elev[E1]);
    gfx_round_fill(&cv, 256 + 19, by + 7, SWITCH_THUMB, SWITCH_THUMB, R_F(SWITCH_THUMB), l->thumb);
    gfx_rrect_fill(&cv, 316, by, 120, H_TAB, RADII(R_3, R_3, 0, 0), l->window);
    gfx_fill(&cv, 316 + 12, by + H_TAB - SIGNAL, 96, SIGNAL, l->accent_line);
    static uint8_t ring[32 * 32];
    for (int j = 0; j < 32; j++)
        for (int i = 0; i < 32; i++) { int d = (i - 16) * (i - 16) + (j - 16) * (j - 16); ring[j * 32 + i] = d < 196 && d > 100 ? 255 : 0; }
    gfx_mask(&cv, 460, by - 2, ring, 32, 32, 32, l->accent_text);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); fails++; return; }
    fprintf(f, "P6\n%d %d\n255\n", W, HT);
    for (int i = 0; i < W * HT; i++) { unsigned char p[3] = { buf[i] >> 16, buf[i] >> 8, buf[i] }; fwrite(p, 1, 3, f); }
    fclose(f);
}

int main(int argc, char **argv) {
    test_hairlines();
    test_corners();
    test_ring_partition();
    test_rings();
    test_shadow();
    test_mask();
    sheet(argc > 1 ? argv[1] : "gfxtest.ppm");
    printf("gfxtest: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
