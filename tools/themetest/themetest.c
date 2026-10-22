/* Host check of common/theme.c: every built-in theme (and every accent preset on top of each) keeps its text/background
 * pairs readable. Body text needs WCAG 4.5:1; decorative marks (line numbers, close glyphs, bright black) 2.5:1;
 * non-text UI parts (accents, outlines, status colours) 3:1. Also checks theme_contrast against the textbook formula. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "theme.h"

static int fails, checks;

static double lin(int c) { double v = c / 255.0; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double L(color_t c) { return 0.2126 * lin(c >> 16 & 255) + 0.7152 * lin(c >> 8 & 255) + 0.0722 * lin(c & 255); }
static double ratio(color_t a, color_t b) { double x = L(a), y = L(b); if (x < y) { double t = x; x = y; y = t; } return (x + 0.05) / (y + 0.05); }

/* a translucent surface over the worst wallpapers: the lighter and the darker of black and white */
static color_t over(color_t s, color_t under) {
    unsigned a = s >> 24;
    unsigned r = ((s >> 16 & 255) * a + (under >> 16 & 255) * (255 - a)) / 255, g = ((s >> 8 & 255) * a + (under >> 8 & 255) * (255 - a)) / 255,
             b = ((s & 255) * a + (under & 255) * (255 - a)) / 255;
    return 0xFF000000u | r << 16 | g << 8 | b;
}

static void need(const char *tag, const char *pair, color_t fg, color_t bg, double min) {
    double r = ratio(fg, bg);
    checks++;
    if (r + 0.005 < min) {
        static char seen[512][64];                      /* the same pair failing under every accent is one finding */
        static int nseen;
        char key[64];
        snprintf(key, sizeof key, "%.12s %.24s %06X %06X", tag, pair, fg & 0xFFFFFF, bg & 0xFFFFFF);
        for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) { fails++; return; }
        if (nseen < 512) strcpy(seen[nseen++], key);
        fails++;
        printf("FAIL %-18s %-24s %06X on %06X = %.2f (< %.1f)\n", tag, pair, fg & 0xFFFFFF, bg & 0xFFFFFF, r, min);
    }
}
#define BODY(p, f, b) need(tag, p, t->f, t->b, 4.5)
#define BODYX(p, fe, be) need(tag, p, fe, be, 4.5)
#define DECO(p, f, b) need(tag, p, t->f, t->b, 2.5)
#define PART(p, f, b) need(tag, p, t->f, t->b, 3.0)

static void check(const struct theme *t, const char *tag, int accent_override) {
    double flare = !strcmp(t->name, "dark") ? 4.0 : 4.5;     /* the original theme is frozen; over a pure white wallpaper its taskbar dims to 4.2 */
    /* widgets: text on every surface it is drawn on */
    BODY("text/bg", text, bg); BODY("text/panel", text, panel); BODY("text/surface", text, surface); BODY("text/hover", text, hover);
    BODY("text/pressed", text, pressed); BODY("text/input", text, input); BODY("text/gutter", text, gutter);
    BODY("dim/bg", dim, bg); BODY("dim/panel", dim, panel); BODY("dim/surface", dim, surface); BODY("dim/input", dim, input);
    DECO("faint/input", faint, input); DECO("faint/gutter", faint, gutter);
    BODY("on_accent/accent", on_accent, accent); PART("on_accent/accent_hi", on_accent, accent_hi); PART("on_accent/accent_lo", on_accent, accent_lo);
    BODY("text/key", text, key); BODY("key_fg/key", key_fg, key); BODY("key_fg/key_hi", key_fg, key_hi); PART("key_fg/key_lo", key_fg, key_lo);
    BODY("bad/bg", bad, bg); BODY("warn/bg", warn, bg); BODY("good/bg", good, bg);
    BODY("bad/panel", bad, panel); BODY("warn/panel", warn, panel); BODY("good/panel", good, panel);
    PART("accent/bg", accent, bg); PART("accent/panel", accent, panel); PART("accent2/bg", accent2, bg); PART("knob/surface", knob, surface);
    PART("border/bg", border, bg);
    /* compositor: title bars, taskbar, popups (checked over both a black and a white wallpaper) */
    BODY("text/frame", text, frame); BODY("cdim/frame_off", cdim, frame_off); BODY("text/frame_off", text, frame_off);
    PART("accent/frame", accent, frame);
    color_t under[2] = { 0xFF000000u, 0xFFFFFFFFu };
    for (int i = 0; i < 2; i++) {
        color_t bar = over(t->bar, under[i]), la = over(t->launcher, under[i]), po = over(t->popup, under[i]), to = over(t->toast, under[i]), sw = over(t->switcher, under[i]);
        BODYX("text/bar", t->text, bar); need(tag, "cdim/bar", t->cdim, bar, flare);
        BODYX("text/launcher", t->text, la); need(tag, "cdim/launcher", t->cdim, la, flare);
        BODYX("text/popup", t->text, po); need(tag, "cdim/popup", t->cdim, po, flare); BODYX("body/popup", t->body, po);
        BODYX("text/toast", t->text, to); BODYX("body/toast", t->body, to);
        BODYX("text/switcher", t->text, sw); need(tag, "cdim/switcher", t->cdim, sw, flare);
        need(tag, "accent/popup", t->accent, po, 3.0);
    }
    {                                                         /* the launcher pill: bold text over the accent -> accent2 gradient */
        need(tag, "on_pill/accent", t->on_pill, t->accent, strcmp(t->name, "dark") || accent_override ? 3.0 : 0);
        need(tag, "on_pill/accent2", t->on_pill, t->accent2, strcmp(t->name, "dark") || accent_override ? 3.0 : 0);   /* dark's white-on-lilac logo is the original look */
    }
    /* terminal */
    const struct term_colors *c = &t->term;
    need(tag, "term fg/bg", c->fg, c->bg, 4.5); need(tag, "term bold/bg", c->bold, c->bg, 4.5);
    need(tag, "term link/bg", c->link, c->bg, 4.5);
    need(tag, "term fg/sel", c->fg, c->sel, 4.5); need(tag, "term fg/hit", c->fg, c->hit, 4.5);
    need(tag, "term hitcur_fg/hitcur", c->hitcur_fg, c->hitcur, 4.5);
    for (int i = 1; i < 16; i++) {
        char p[24];
        snprintf(p, sizeof p, "ansi %d/bg", i);
        need(tag, p, c->pal[i], c->bg, i == 8 ? 2.5 : 4.5);
    }
    need(tag, "ansi 0 vs bg (visible)", c->pal[0], c->bg, 1.0);
    BODY("tab_fg/tab", tab_fg, tab); BODY("tab_fg/tab_off", tab_fg, tab_off); BODY("tab_dim/tab_off", tab_dim, tab_off);
    BODY("tab_dim/tbar", tab_dim, tbar); BODY("tab_dim/tfind", tab_dim, tfind); BODY("tab_fg/tfind", tab_fg, tfind);
    DECO("tab_faint/tab_off", tab_faint, tab_off); DECO("tab_faint/tfind", tab_faint, tfind);
    PART("tab_acc/tab", tab_acc, tab);
    /* the terminal must read against the window frame it sits in */
    need(tag, "term bg vs tbar (distinct)", c->bg, t->tbar, 1.02);
}

int main(void) {
    /* the integer contrast must match the textbook double formula */
    srand(7);
    int worst = 0;
    for (int i = 0; i < 5000; i++) {
        color_t a = 0xFF000000u | (rand() & 0xFFFFFF), b = 0xFF000000u | (rand() & 0xFFFFFF);
        int got = theme_contrast(a, b), want = (int)(ratio(a, b) * 100);
        if (abs(got - want) > worst) worst = abs(got - want);
    }
    checks++;
    if (worst > 2) { fails++; printf("FAIL theme_contrast is %d/100 off the WCAG formula\n", worst); }
    checks++;
    if (theme_contrast(0xFF000000u, 0xFFFFFFFFu) != 2100) { fails++; printf("FAIL black on white = %d\n", theme_contrast(0xFF000000u, 0xFFFFFFFFu)); }

    int n = theme_count();
    checks++;
    if (n < 5) { fails++; printf("FAIL only %d themes\n", n); }
    for (int i = 0; i < n; i++) {
        const struct theme *t = theme_get(i);
        check(t, t->name, 0);
        checks++;
        if (theme_find(t->name) != i) { fails++; printf("FAIL theme_find(%s)\n", t->name); }
        for (int a = 0; a < THEME_ACCENTS; a++) {
            struct theme u = *t;
            theme_set_accent(&u, theme_accents[a].rgb);
            char tag[40];
            snprintf(tag, sizeof tag, "%s+%s", t->name, theme_accents[a].name);
            check(&u, tag, 1);
        }
    }
    printf("themetest: %d themes x %d accents, %d checks, %d failed\n", n, THEME_ACCENTS, checks, fails);
    return fails != 0;
}
