/* uni.c - fallback glyphs for what the baked faces lack (CJK, Hangul, kana, Hebrew, Thai): font_fallback for gfx.c.
 * The data is root/usr/share/vibeos/uni15.bin.gz (tools/mkcjk.py: format described there), read on first use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "image.h"

static const uint8_t *ud;                   /* inflated file */
static const uint32_t *ucp, *uoff;
static uint32_t un;
static int tried;
static struct glyph fg;

static void uni_load(void) {
    tried = 1;
    FILE *f = fopen("/usr/share/vibeos/uni15.bin.gz", "rb");
    if (!f) return;
    size_t n = 0, cap = 1 << 20, r;
    uint8_t *z = malloc(cap), *raw = NULL;
    while (z && (r = fread(z + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { uint8_t *nz = realloc(z, cap *= 2); if (!nz) { free(z); z = NULL; } else z = nz; }
    }
    size_t rl = 0;
    if (z && gunzip_buf(z, n, &raw, &rl) == 0 && rl >= 8 && !memcmp(raw, "VUNI", 4)) {
        uint32_t c = raw[4] | raw[5] << 8 | raw[6] << 16 | (uint32_t)raw[7] << 24;
        if (c && (uint64_t)c * 8 + 8 <= rl) { un = c; ucp = (const uint32_t *)(raw + 8); uoff = ucp + c; ud = raw + 8 + (size_t)c * 8; }
        else free(raw);
    } else free(raw);
    free(z);
    fclose(f);
}

static const struct glyph *uni_glyph(const struct font *f, uint32_t cp) {
    if (!tried) uni_load();
    if (!un) return NULL;
    uint32_t lo = 0, hi = un;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (ucp[m] < cp) lo = m + 1; else hi = m;
    }
    if (lo >= un || ucp[lo] != cp) return NULL;
    const uint8_t *g = ud + uoff[lo];
    int w = g[0], h = g[1];
    if (w * h > GFX_FB_MAX) return NULL;
    const uint8_t *p = g + 5;
    for (int i = 0; i < w * h; i++) {
        int nb = (i & 1) ? p[i >> 1] & 15 : p[i >> 1] >> 4;
        gfx_fb_alpha[i] = nb * 17;
    }
    fg = (struct glyph){ w, h, (int8_t)g[2], (int8_t)(f->ascent + (int8_t)g[3]), g[4], g[4] << 6, GLYPH_FB };
    return &fg;
}

void uni_init(void) { font_fallback = uni_glyph; }
