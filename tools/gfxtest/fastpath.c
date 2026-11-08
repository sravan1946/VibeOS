/* fastpath.c - gfx.c's fast paths against the original slow implementation (gfx_ref.c, symbols renamed ref_*):
 * random canvases, clips, odd widths and alignments must come out pixel-identical. `fastpath bench` also prints cycles
 * per call for both (what the perf pass measured). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "design.h"

void ref_gfx_fill(struct canvas *, int, int, int, int, color_t);
void ref_gfx_mask(struct canvas *, int, int, const uint8_t *, int, int, int, color_t);
void ref_gfx_blit_alpha(struct canvas *, int, int, const uint32_t *, int, int, int);
void ref_gfx_blit_premul(struct canvas *, int, int, const uint32_t *, int, int, int);
void ref_gfx_blit_scaled(struct canvas *, int, int, int, int, const uint32_t *, int, int, int);
void ref_gfx_shadow9(struct canvas *, int, int, int, int, int, int, int, int);
int ref_gfx_text_n(struct canvas *, const struct font *, int, int, const char *, int, color_t);

static int fails, checks;
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904243u; return rs >> 8; }
static inline unsigned long long tsc(void) { unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (unsigned long long)hi << 32 | lo; }

#define CW 211                                  /* odd on purpose */
#define CH 97
static uint32_t a[CW * CH], b[CW * CH], src[CW * CH];
static uint8_t msk[CW * CH];

static void noise(void) {                       /* opaque backdrop, random-alpha source with runs of 0 and 255 */
    for (int i = 0; i < CW * CH; i++) {
        a[i] = 0xFF000000u | (rnd() & 0xFFFFFF);
        uint32_t k = rnd() & 7, al = k < 2 ? 0 : k < 4 ? 255 : rnd() & 255;
        src[i] = al << 24 | (rnd() & 0xFFFFFF);
        msk[i] = k < 3 ? 0 : k < 5 ? 255 : rnd() & 255;
    }
    memcpy(b, a, sizeof a);
}
static void same(const char *what, int it) {
    checks++;
    if (memcmp(a, b, sizeof a)) {
        int i = 0; while (a[i] == b[i]) i++;
        fails++; printf("FAIL %s: pixel %d,%d differs (%08x vs ref %08x) iteration %d\n", what, i % CW, i / CW, a[i], b[i], it);
    }
}
static void cvs(struct canvas *c, uint32_t *p) {                  /* canvas = a random sub-window of the buffer, random clip */
    int w = 20 + rnd() % (CW - 20), h = 10 + rnd() % (CH - 10);
    canvas_init(c, p, w, h, CW);
    if (rnd() & 1) canvas_clip(c, rnd() % (w / 2) - 3, rnd() % (h / 2) - 3, 1 + rnd() % w, 1 + rnd() % h);
}
/* run the same random call on buffer a (new code) and b (reference) */
#define BOTH(call_new, call_ref) do { struct canvas c1, c2; uint32_t s0 = rs; cvs(&c1, a); rs = s0; cvs(&c2, b); \
    x = (int)(rnd() % (CW + 40)) - 20; y = (int)(rnd() % (CH + 20)) - 10; w = 1 + rnd() % 160; h = 1 + rnd() % 70; \
    { struct canvas *c = &c1; call_new; } { struct canvas *c = &c2; call_ref; } } while (0)

static void test_all(void) {
    int x, y, w, h;
    for (int it = 0; it < 3000; it++) {
        noise();
        color_t col = it % 3 == 0 ? 0xFF000000u | rnd() : it % 3 == 1 ? (rnd() & 255) << 24 | (rnd() & 0xFFFFFF) : (uint32_t)rnd() << 8 | 0x80;
        BOTH(gfx_fill(c, x, y, w, h, col), ref_gfx_fill(c, x, y, w, h, col)); same("fill", it);
        int mw = 1 + w % 60, mh = 1 + h % 30;
        BOTH(gfx_mask(c, x, y, msk, mw, mh, mw, col), ref_gfx_mask(c, x, y, msk, mw, mh, mw, col)); same("mask", it);
        BOTH(gfx_blit_alpha(c, x, y, src, w, h, CW), ref_gfx_blit_alpha(c, x, y, src, w, h, CW)); same("blit_alpha", it);
        BOTH(gfx_blit_premul(c, x, y, src, w, h, CW), ref_gfx_blit_premul(c, x, y, src, w, h, CW)); same("blit_premul", it);
        int sw = 1 + rnd() % 100, sh = 1 + rnd() % 60;
        BOTH(gfx_blit_scaled(c, x, y, w, h, src, sw, sh, CW), ref_gfx_blit_scaled(c, x, y, w, h, src, sw, sh, CW)); same("blit_scaled", it);
        int r = rnd() % 14, dy = (int)(rnd() % 9) - 2, blur = rnd() % 3 ? 4 + 4 * (rnd() % 8) : rnd() % 5, al = rnd() % 4 ? 20 + rnd() % 200 : rnd() % 300;
        BOTH(gfx_shadow9(c, x, y, w, h, r, dy, blur, al), ref_gfx_shadow9(c, x, y, w, h, r, dy, blur, al)); same("shadow9", it);
        BOTH(gfx_text_n(c, &font_body, x, y, "The quick brown fox, jumps over 0123 (lazy) dogs", 1 + w % 48, col),
             ref_gfx_text_n(c, &font_body, x, y, "The quick brown fox, jumps over 0123 (lazy) dogs", 1 + w % 48, col)); same("text", it);
    }
}

/* ---- bench: representative frames, new vs ref, best of 5 ---- */
#define BW 1280
#define BH 800
static uint32_t big[BW * BH], bigsrc[BW * BH];
static unsigned long long best(void (*fn)(struct canvas *, int), int ref) {
    unsigned long long m = ~0ull;
    struct canvas c;
    canvas_init(&c, big, BW, BH, BW);
    for (int k = 0; k < 5; k++) { for (int i = 0; i < BW * BH; i++) big[i] = 0xFF303030u + i * 7; unsigned long long t = tsc(); fn(&c, ref); t = tsc() - t; if (t < m) m = t; }
    return m;
}
static void b_fill(struct canvas *c, int r) { color_t x = 0xFF123456u; for (int i = 0; i < 20; i++) (r ? ref_gfx_fill : gfx_fill)(c, 100, 100, 800, 520, x); }
static void b_fillA(struct canvas *c, int r) { color_t x = 0x80123456u; for (int i = 0; i < 4; i++) (r ? ref_gfx_fill : gfx_fill)(c, 100, 100, 800, 520, x); }
static void b_text(struct canvas *c, int r) {       /* ~30 lines of body text, like a launcher / settings page */
    static const char s[] = "The quick brown fox jumps over the lazy dog 0123456789 and more text";
    for (int l = 0; l < 30; l++) (r ? ref_gfx_text_n : gfx_text_n)(c, &font_body, 20, 10 + l * 20, s, 1 << 30, 0xFFE0E0E0u);
}
static void b_mono(struct canvas *c, int r) {       /* a terminal: 100 x 30 cells, bg fill + glyph each */
    for (int row = 0; row < 30; row++) for (int col = 0; col < 100; col++) {
        (r ? ref_gfx_fill : gfx_fill)(c, 12 + col * 8, 20 + row * 18, 8, 18, 0xFF101214u);
        (r ? ref_gfx_text_n : gfx_text_n)(c, &font_mono, 12 + col * 8, 20 + row * 18, "e", 1, 0xFFD0D0D0u);
    }
}
static void b_shadow(struct canvas *c, int r) { for (int i = 0; i < 4; i++) (r ? ref_gfx_shadow9 : gfx_shadow9)(c, 200, 120, 800, 520, 8, 8, 24, 90); }
static void b_blita(struct canvas *c, int r) { for (int i = 0; i < 2; i++) (r ? ref_gfx_blit_alpha : gfx_blit_alpha)(c, 0, 0, bigsrc, 800, 520, BW); }
static void b_scaled(struct canvas *c, int r) { (r ? ref_gfx_blit_scaled : gfx_blit_scaled)(c, 0, 0, 800, 520, bigsrc, 640, 400, BW); }
static void bench(void) {
    for (int i = 0; i < BW * BH; i++) bigsrc[i] = (i % 5 ? 0xFF000000u : (uint32_t)(i * 13 & 255) << 24) | (uint32_t)(i * 2654435761u) >> 8;
    static const struct { const char *n; void (*f)(struct canvas *, int); int div; } t[] = {
        { "fill opaque 800x520", b_fill, 20 }, { "fill alpha 800x520", b_fillA, 4 }, { "text 30 body lines", b_text, 1 },
        { "terminal 3000 cells", b_mono, 1 }, { "shadow9 800x520 E3", b_shadow, 4 }, { "blit_alpha 800x520", b_blita, 2 }, { "blit_scaled 800x520", b_scaled, 1 } };
    printf("%-24s %12s %12s %8s\n", "kcycles per call", "ref", "new", "speedup");
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        unsigned long long o = best(t[i].f, 1) / t[i].div, n = best(t[i].f, 0) / t[i].div;
        printf("%-24s %12llu %12llu %7.2fx\n", t[i].n, o / 1000, n / 1000, (double)o / n);
    }
}

int main(int argc, char **argv) {
    test_all();
    printf("fastpath: %d checks, %d failed\n", checks, fails);
    if (argc > 1 && !strcmp(argv[1], "bench")) bench();
    return fails != 0;
}
