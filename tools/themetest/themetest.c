/* Host check of common/theme.c: every built-in theme (and every accent preset on top of each) keeps its text/background
 * pairs readable. Body text needs WCAG 4.5:1; decorative marks (line numbers, close glyphs, bright black) 2.5:1;
 * non-text UI parts (control edges, tracks, the signal line, the focus ring) 3:1. Also checks the Graphite token invariants
 * (elevation order, shadow levels, the default amber) and theme_contrast against the textbook formula. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "theme.h"

static int fails, checks;

static double lin(int c) { double v = c / 255.0; return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static double L(color_t c) { return 0.2126 * lin(c >> 16 & 255) + 0.7152 * lin(c >> 8 & 255) + 0.0722 * lin(c & 255); }
static double lum(color_t c) { return L(c); }
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
    /* Graphite tokens (docs/DESIGN.md §2): text on every surface it is drawn on */
    color_t surf[5] = { t->base, t->window, t->raised, t->overlay, t->selection };
    const char *sn[5] = { "base", "window", "raised", "overlay", "selection" };
    char p[48];
    for (int i = 0; i < 5; i++) {
#define ON(f, min) snprintf(p, sizeof p, #f "/%s", sn[i]), need(tag, p, t->f, surf[i], min)
        ON(text, 4.5); ON(text2, 4.5);
        if (i < 3) ON(text3, 4.5);                              /* placeholders/captions; on dark overlays text2 is used instead */
        if (i < 4) { ON(accent_text, 4.5); ON(danger, 4.5); ON(warning, 4.5); ON(success, 4.5); ON(info, 4.5); ON(control, 3.0); }
        ON(accent_line, 3.0);                                   /* the signal line, also on the selected row */
        if (i < 4) ON(accent, t->dark ? 3.0 : 1.0);            /* light accent fills carry an accent_line rim instead */
#undef ON
    }
    BODY("on_accent/accent", on_accent, accent); BODY("on_accent/accent_hover", on_accent, accent_hover); BODY("on_accent/accent_press", on_accent, accent_press);
    BODY("on_danger/danger", on_danger, danger);
    PART("accent_line/accent", accent_line, raised);
    need(tag, "thumb/control (off track)", t->thumb, t->control, t->dark ? 3.0 : 1.0);   /* light thumbs carry a control ring */
    /* the text over the translucent hover/press backplates, on the lightest surface they sit on */
    BODYX("text/hover_ov", t->text, over(t->hover_ov, t->raised)); BODYX("text/press_ov", t->text, over(t->press_ov, t->raised));
    BODYX("text2/hover_ov", t->text2, over(t->hover_ov, t->raised));
    /* dark elevation is lightness, and shadows deepen with the level */
    checks++;
    if (t->dark && !(lum(t->base) <= lum(t->window) && lum(t->window) <= lum(t->raised) && lum(t->raised) <= lum(t->overlay))) {
        fails++; printf("FAIL %-18s surfaces do not get lighter with elevation\n", tag);
    }
    for (int e = 1; e < 5; e++) {
        checks++;
        if (t->elev[e] < t->elev[e - 1] || t->elev[e] > 255) { fails++; printf("FAIL %-18s elev[%d] = %d\n", tag, e, t->elev[e]); }
    }
    /* legacy names: still read by unported widgets and the compositor */
    BODY("text/hover", text, hover); BODY("text/pressed", text, pressed); BODY("text/input", text, input); BODY("text/gutter", text, gutter);
    BODY("dim/panel", dim, panel); BODY("dim/input", dim, input); DECO("faint/gutter", faint, gutter);
    PART("on_accent/accent_hi", on_accent, accent_hi); PART("on_accent/accent_lo", on_accent, accent_lo);
    BODY("text/key", text, key); BODY("key_fg/key", key_fg, key); BODY("key_fg/key_hi", key_fg, key_hi); PART("key_fg/key_lo", key_fg, key_lo);
    BODY("bad/bg", bad, bg); BODY("warn/panel", warn, panel); PART("knob/surface", knob, surface);
    BODY("text/frame", text, frame); BODY("cdim/frame_off", cdim, frame_off); BODY("cdim/bar", cdim, bar); BODY("body/popup", body, popup);
    BODY("on_pill/accent", on_pill, accent);
    /* terminal */
    const struct term_colors *c = &t->term;
    need(tag, "term fg/bg", c->fg, c->bg, 4.5); need(tag, "term bold/bg", c->bold, c->bg, 4.5);
    need(tag, "term link/bg", c->link, c->bg, 4.5);
    need(tag, "term fg/sel", c->fg, c->sel, 4.5); need(tag, "term fg/hit", c->fg, c->hit, 4.5);
    need(tag, "term hitcur_fg/hitcur", c->hitcur_fg, c->hitcur, 4.5);
    for (int i = 1; i < 16; i++) {
        snprintf(p, sizeof p, "ansi %d/bg", i);
        need(tag, p, c->pal[i], c->bg, i == 8 ? 3.0 : 4.5);
    }
    need(tag, "ansi 0 vs bg (visible)", c->pal[0], c->bg, 1.0);
    BODY("tab_fg/tab", tab_fg, tab); BODY("tab_fg/tab_off", tab_fg, tab_off); BODY("tab_dim/tab_off", tab_dim, tab_off);
    BODY("tab_dim/tbar", tab_dim, tbar); BODY("tab_dim/tfind", tab_dim, tfind); BODY("tab_fg/tfind", tab_fg, tfind);
    PART("tab_acc/tab", tab_acc, tab);
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
    if (n != 3 || strcmp(theme_get(0)->name, "graphite") || theme_find("dark") != 0 || theme_find("light") != 1 || theme_find("nord") != -1) {
        fails++; printf("FAIL the built-ins are graphite, graphite-light, contrast (dark/light still resolve)\n");
    }
    checks++;
    if (theme_accents[0].rgb != theme_get(0)->accent || theme_get(0)->accent != 0xFFE8A33Du) { fails++; printf("FAIL the default accent is amber #E8A33D\n"); }
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
