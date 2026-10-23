/* sheet.c - contact sheet of the baked assets (fonts, icons, wallpapers) for eyeballing, plus checks on them.
 * Build/run via run.sh, which turns the PPM into build/assetsheet.png. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "termfonts.h"
#include "icons.h"
#include "wallpaper.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SW 1440
#define SH 4200
static uint32_t px[SW * SH];
static struct canvas cv;

static void tint(const struct icon *ic, int x, int y, color_t col) { gfx_mask(&cv, x, y, ic->a8, ic->w, ic->h, ic->w, col); }
/* premultiplied ARGB over (gfx.c has no such blit yet; gfx_blit_alpha expects straight alpha) */
static void premul(const struct aicon *ic, int x, int y) {
    for (int j = 0; j < ic->h; j++)
        for (int i = 0; i < ic->w; i++) {
            uint32_t s = ic->argb[j * ic->w + i], a = s >> 24, *d = &px[(y + j) * SW + x + i], o = 0xFF000000u;
            for (int k = 0; k < 24; k += 8) o |= (((s >> k) & 0xFF) + ((*d >> k) & 0xFF) * (255 - a) / 255) << k;
            *d = o;
        }
}

static void ftext(const struct font *f, int x, int y, const char *s, color_t col) {   /* pen in 1/64 px (adv64) */
    int pen = x * 64;
    for (uint32_t cp; *s; pen += font_glyph(f, cp)->adv64) {
        s += utf8_decode(s, 4, &cp);
        gfx_char(&cv, f, (pen + 32) >> 6, y, cp, col);
    }
}

#define DARK RGB(0x1B, 0x1C, 0x1F)
#define LIGHT RGB(0xFF, 0xFF, 0xFF)
#define TXT RGB(0xE6, 0xE7, 0xE9)
#define TXT2 RGB(0xA4, 0xA7, 0xAD)
#define INK RGB(0x16, 0x17, 0x1A)
#define INK2 RGB(0x55, 0x58, 0x5E)

static int heading(int y, const char *s) {
    gfx_fill(&cv, 0, y, SW, 40, RGB(0x14, 0x15, 0x17));
    gfx_text(&cv, &font_heading, 24, y + 10, s, TXT);
    return y + 40;
}

static const struct { const char *name; const struct font *f; int px, lh; } fonts[] = {
    { "font_caption 11/14 400", &font_caption, 11, 14 }, { "font_caption_bold 11/14 600 +0.3", &font_caption_bold, 11, 14 },
    { "font_body 13/18 400", &font_body, 13, 18 }, { "font_body_bold 13/18 600", &font_body_bold, 13, 18 },
    { "font_heading 15/20 600", &font_heading, 15, 20 }, { "font_title 20/26 600", &font_title, 20, 26 },
    { "font_display 28/34 600", &font_display, 28, 34 },
    { "font_mono 13/18 400", &font_mono, 13, 18 }, { "font_mono_bold 13/18 700", &font_mono_bold, 13, 18 },
    { "font_mono_11", &font_mono_11, 11, 15 }, { "font_mono_15", &font_mono_15, 15, 21 },
    { "font_mono_18", &font_mono_18, 18, 25 },
    { "font_ui (Noto 15, legacy)", &font_ui, 15, 19 }, { "font_ui_bold (legacy)", &font_ui_bold, 15, 18 },
    { "font_big (Noto 52, legacy)", &font_big, 52, 64 },
};
#define SYM_NAME(id, f) #id,
static const char *const symnames[] = { ICON_LIST(SYM_NAME) };
#define APP_NAME(id, k, n) k,
static const char *const appnames[] = { APPICON_LIST(APP_NAME) };
#define FILE_NAME(id, k) k,
static const char *const filenames[] = { FILEICON_LIST(FILE_NAME) };

int main(int argc, char **argv) {
    canvas_init(&cv, px, SW, SH, SW);
    gfx_fill(&cv, 0, 0, SW, SH, DARK);
    int y = heading(0, "Fonts: dark (#1B1C1F / #E6E7E9) and light (#FFFFFF / #16171A); the box is the line height");
    for (unsigned i = 0; i < sizeof fonts / sizeof *fonts; i++) {
        const struct font *f = fonts[i].f;
        CHECK(f->height == fonts[i].lh);
        int d0 = font_glyph(f, '0')->adv, tab = 1;
        for (int c = '1'; c <= '9'; c++) tab &= font_glyph(f, c)->adv == d0;
        CHECK(tab);                                      /* tabular digits */
        const char *s1 = f == &font_big ? "0123456789 ×÷−" : "Sphinx of black quartz, judge my vow · Files — Home",
                   *s2 = f == &font_big ? "12:45" : "0123456789  1,234.56  2.1 GB  “quotes” café ✓ ← → … 11:11";
        int h = f->height * 2 + 12;
        gfx_fill(&cv, SW / 2, y, SW / 2, h + 20, LIGHT);
        gfx_text(&cv, &font_caption, 24, y + 4, fonts[i].name, TXT2);
        gfx_text(&cv, &font_caption, SW / 2 + 24, y + 4, fonts[i].name, INK2);
        for (int side = 0; side < 2; side++) {
            int x = side ? SW / 2 + 24 : 24, yy = y + 20;
            color_t col = side ? INK : TXT;
            gfx_rect(&cv, x - 1, yy - 1, gfx_text_width(f, s1) + 2, f->height + 2, side ? RGB(0xE0, 0xC8, 0xA0) : RGB(0x45, 0x3A, 0x2A));
            gfx_text(&cv, f, x, yy, s1, col);
            gfx_text(&cv, f, x, yy + f->height + 6, s2, col);
        }
        y += h + 24;
    }

    y = heading(y, "Whole-pixel advances (today's gfx_text) vs a fractional pen using adv64");
    for (int i = 0; i < 3; i++) {
        const struct font *f = fonts[i * 2 % 4 + i / 2].f;   /* caption, caption_bold, body */
        const char *s = "Sphinx of black quartz, judge my vow · Run commands in a shell · Places";
        gfx_text(&cv, f, 24, y + 8, s, TXT);
        ftext(f, SW / 2 + 24, y + 8, s, TXT);
        y += f->height + 8;
    }
    y = heading(y + 8, "Symbolic icons (Lucide): 16 px and 20 px, idle / active / accent on dark, and on light");
    int cols = 7, cw = SW / cols, x0;
    for (int i = 0; i < ICON_COUNT; i++) {
        int cx = (i % cols) * cw, cy = y + (i / cols) * 40;
        if (i % cols == 0) gfx_fill(&cv, 0, cy, SW, 40, (i / cols) & 1 ? RGB(0x20, 0x21, 0x24) : DARK);
        const struct icon *a = icon_get(i, 16), *b = icon_get(i, 20);
        CHECK(a && a->w == 16 && b && b->w == 20);
        tint(a, cx + 8, cy + 12, TXT2); tint(a, cx + 28, cy + 12, TXT); tint(b, cx + 48, cy + 10, RGB(0xE8, 0xA3, 0x3D));
        gfx_fill(&cv, cx + 72, cy + 6, 28, 28, LIGHT);
        tint(a, cx + 78, cy + 12, INK2);
        gfx_text(&cv, &font_caption, cx + 106, cy + 13, symnames[i], TXT2);
    }
    y += (ICON_COUNT + cols - 1) / cols * 40 + 8;
    CHECK(icon_get(ICON_COUNT, 16) == 0 && icon_get(-1, 16) == 0);
    CHECK(icon_get(0, 18)->w == 16 && icon_get(0, 64)->w == 20 && icon_get(0, 4)->w == 16);

    for (int set = 0; set < 2; set++) {
        int n = set ? FILEICON_COUNT : APPICON_COUNT;
        y = heading(y, set ? "File-type icons: 48 / 32 / 24 px on dark (#141517) and light (#E9EAEC)"
                           : "App icons: 48 / 32 / 24 px on dark (#141517) and light (#E9EAEC)");
        cols = 6; cw = SW / cols;
        for (int i = 0; i < n; i++) {
            int cx = (i % cols) * cw, cy = y + (i / cols) * 76;
            for (int side = 0; side < 2; side++) {
                x0 = cx + side * cw / 2;
                gfx_fill(&cv, x0, cy, cw / 2, 76, side ? RGB(0xE9, 0xEA, 0xEC) : RGB(0x14, 0x15, 0x17));
                int xx = x0 + 8;
                for (int s = 48; s >= 24; s -= s == 48 ? 16 : 8) {
                    const struct aicon *ic = set ? fileicon_get(i, s) : appicon_get(i, s);
                    CHECK(ic && ic->w == s);
                    premul(ic, xx, cy + 8 + (48 - s));
                    xx += s + 4;
                }
                gfx_text(&cv, &font_caption, x0 + 8, cy + 60, set ? filenames[i] : appnames[i], side ? INK2 : TXT2);
            }
        }
        y += (n + cols - 1) / cols * 76 + 8;
    }
    CHECK(appicon_find("Text Editor") == APPICON_EDITOR && appicon_find("editor") == APPICON_EDITOR);
    CHECK(appicon_find("SYSMON") == APPICON_SYSMON && appicon_find("2048") == APPICON_G2048 && appicon_find("nope") == -1);
    CHECK(appicon_get(APPICON_COUNT, 48) == 0 && appicon_get(0, 40)->w == 32 && fileicon_get(0, 16)->w == 24);

    y = heading(y, "Wallpapers (320x200 renders; Dunes is the default)");
    static uint32_t thumb[320 * 200];
    for (int i = 0; i < wallpaper_count(); i++) {
        struct canvas t;
        canvas_init(&t, thumb, 320, 200, 320);
        wallpaper_render(&t, i);
        int tx = 24 + i * 276, ty = y + 12;
        gfx_blit_scaled(&cv, tx, ty, 256, 160, thumb, 320, 200, 320);
        gfx_text(&cv, &font_caption, tx, ty + 166, wallpaper_name(i), TXT2);
    }
    CHECK(!strcmp(wallpaper_name(0), "Dunes"));
    y += 200;

    FILE *f = fopen(argc > 1 ? argv[1] : "assetsheet.ppm", "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", SW, y);
    for (int i = 0; i < SW * y; i++) { uint8_t b[3] = { px[i] >> 16, px[i] >> 8, px[i] }; fwrite(b, 1, 3, f); }
    fclose(f);
    if (argc > 2 && (f = fopen(argv[2], "wb"))) {           /* the default wallpaper at the mockup's size */
        static uint32_t wp[1280 * 800];
        struct canvas t;
        canvas_init(&t, wp, 1280, 800, 1280);
        wallpaper_render(&t, 0);
        fprintf(f, "P6\n1280 800\n255\n");
        for (int i = 0; i < 1280 * 800; i++) { uint8_t b[3] = { wp[i] >> 16, wp[i] >> 8, wp[i] }; fwrite(b, 1, 3, f); }
        fclose(f);
    }
    printf("assetsheet: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
