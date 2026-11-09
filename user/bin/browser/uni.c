/* uni.c - fallback glyphs for what the baked faces lack (CJK, Hangul, kana, Hebrew, Thai): font_fallback for gfx.c.
 * The data is root/usr/share/vibeos/uni15.bin.gz (tools/mkcjk.py: format described there), read on first use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "image.h"

static const uint8_t *ud;                   /* inflated file */
static const uint32_t *ucp, *uoff;
static uint32_t un, en;
static const uint32_t *ecp;
static const uint16_t *ed;
static int tried;
static struct glyph fg;

static uint8_t *slurp_gunzip(const char *path, size_t *rl) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = 0, cap = 1 << 20, r;
    uint8_t *z = malloc(cap), *raw = NULL;
    while (z && (r = fread(z + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { uint8_t *nz = realloc(z, cap *= 2); if (!nz) { free(z); z = NULL; } else z = nz; }
    }
    fclose(f);
    if (!z || gunzip_buf(z, n, &raw, rl) < 0) raw = NULL;
    free(z);
    return raw;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void uni_load(void) {
    tried = 1;
    size_t rl = 0;
    uint8_t *raw = slurp_gunzip("/usr/share/vibeos/uni15.bin.gz", &rl);
    if (raw && rl >= 8 && !memcmp(raw, "VUNI", 4) && rd32(raw + 4) && (uint64_t)rd32(raw + 4) * 8 + 8 <= rl) {
        un = rd32(raw + 4); ucp = (const uint32_t *)(raw + 8); uoff = ucp + un; ud = raw + 8 + (size_t)un * 8;
    } else free(raw);
    raw = slurp_gunzip("/usr/share/vibeos/emoji16.bin.gz", &rl);
    if (raw && rl >= 8 && !memcmp(raw, "VEMO", 4) && rd32(raw + 4) && (uint64_t)rd32(raw + 4) * 516 + 8 <= rl) {
        en = rd32(raw + 4); ecp = (const uint32_t *)(raw + 8); ed = (const uint16_t *)(ecp + en);
    } else free(raw);
}

/* colour emoji: 16 px, drawn a little larger than the text's x-height like browsers do */
static const struct glyph *emoji_glyph(const struct font *f, uint32_t cp) {
    uint32_t lo = 0, hi = en;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (ecp[m] < cp) lo = m + 1; else hi = m;
    }
    if (lo >= en || ecp[lo] != cp) return NULL;
    uint32_t *o = (uint32_t *)gfx_fb_alpha;
    for (int i = 0; i < 256; i++) {
        uint16_t v = ed[lo * 256 + i];
        uint32_t a = (v >> 12) * 17, r = (v >> 8 & 15) * 17, g = (v >> 4 & 15) * 17, b = (v & 15) * 17;
        o[i] = a << 24 | r << 16 | g << 8 | b;
    }
    fg = (struct glyph){ 16, 16, 0, (int8_t)(f->ascent - 13), 17, 17 << 6, GLYPH_FB_COLOR };
    return &fg;
}

static const struct glyph *uni_glyph(const struct font *f, uint32_t cp) {
    if (!tried) uni_load();
    if (cp >= 0x231A && en) { const struct glyph *e = emoji_glyph(f, cp); if (e) return e; }
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
