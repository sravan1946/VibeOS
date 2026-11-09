/* ttf.c - vector text for the browser: stb_truetype rasterises glyph outlines at the size a page asks for, so a 96 px heading is drawn from the
 * real outline, not an enlarged 20 px bitmap. A rendered (face, px) becomes a runtime `struct font` like the baked ones (ASCII in g[], filled when
 * the font is made; everything else comes lazily through gfx.c's font_fallback hook into a per-font glyph cache), so layout and drawing treat
 * it like any other FT[] entry. Faces: the built-in static instances (tools/mkttf.py -> /usr/share/vibeos/fonts: Inter 400..800, JetBrains Mono
 * 400/700, a symbol face for stars/dingbats) and web fonts a page loads with @font-face (ttf_add_face). The baked faces stay for the chrome and
 * the small body sizes; they also get the symbol face as a last resort, so a missing glyph is a star, not a box. */
#include "browser.h"
#include "layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"

#define NFACE 24
#define NFONT 40
#define MAXPX 118                                  /* int8 glyph offsets */
struct tface { char fam[48]; short wght; uint8_t mono, ital, loaded, bad, own; char path[64]; uint8_t *data; stbtt_fontinfo fi; };
static struct tface F[NFACE];
static int nf;

struct gent { uint32_t cp; struct glyph g; uint8_t miss; };
struct tfont { struct font f; int face, px; float sc; struct gent *ex; int nex, excap; uint8_t *al; uint32_t aln, alcap; };
static struct tfont TF[NFONT];
static int ntf, tf_ready;
static int symface = -1;

static const char *fontdir(void) { const char *e = getenv("VIBE_FONTDIR"); return e && *e ? e : "/usr/share/vibeos/fonts"; }

static int add_face(const char *fam, int wght, int mono, int ital, const char *path, uint8_t *data, int own) {
    if (nf >= NFACE) return -1;
    struct tface *t = &F[nf];
    memset(t, 0, sizeof *t);
    strlcpy(t->fam, fam, sizeof t->fam);
    t->wght = wght; t->mono = mono; t->ital = ital; t->own = own;
    if (path) strlcpy(t->path, path, sizeof t->path);
    t->data = data;
    return nf++;
}

static void init_faces(void) {
    if (tf_ready) return;
    tf_ready = 1;
    static const short iw[] = { 400, 500, 600, 700, 800 };
    for (int i = 0; i < 5; i++) { char p[64]; snprintf(p, sizeof p, "inter-%d.ttf", iw[i]); add_face("inter", iw[i], 0, 0, p, 0, 0); }
    add_face("jbmono", 400, 1, 0, "jbmono-400.ttf", 0, 0);
    add_face("jbmono", 700, 1, 0, "jbmono-700.ttf", 0, 0);
    symface = add_face("symbols", 400, 0, 0, "symbols.ttf", 0, 0);
}

static int face_load(int i) {                       /* read + parse on first use */
    struct tface *t = &F[i];
    if (t->loaded) return 1;
    if (t->bad) return 0;
    if (!t->data) {
        char p[200];
        snprintf(p, sizeof p, "%s/%s", fontdir(), t->path);
        FILE *fp = fopen(p, "rb");
        if (!fp) { t->bad = 1; return 0; }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        t->data = n > 0 ? malloc(n) : 0;
        if (!t->data || fread(t->data, 1, n, fp) != (size_t)n) { free(t->data); t->data = 0; fclose(fp); t->bad = 1; return 0; }
        fclose(fp);
        t->own = 1;
    }
    if (!stbtt_InitFont(&t->fi, t->data, stbtt_GetFontOffsetForIndex(t->data, 0))) { t->bad = 1; return 0; }
    t->loaded = 1;
    return 1;
}

/* a web font's bytes (TTF/OTF, already unwrapped); the face takes ownership of data */
int ttf_add_face(const char *fam, int wght, int ital, uint8_t *data) {
    init_faces();
    int i = add_face(fam, wght, 0, ital, 0, data, 1);
    if (i >= 0 && !face_load(i)) { F[i].fam[0] = 0; return -1; }
    return i;
}

/* best face for a family name (lower case) and weight: -1 = none */
int ttf_find(const char *fam, int wght, int ital) {
    init_faces();
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < nf; i++) {
        if (F[i].bad || strcmp(F[i].fam, fam)) continue;
        int d = abs(F[i].wght - wght) * 4 + (F[i].ital != ital);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static struct tfont *tfont_of(const struct font *f) {
    if ((const struct tfont *)f < TF || (const struct tfont *)f >= TF + NFONT) return 0;
    return (struct tfont *)f;
}

static uint8_t *al_room(struct tfont *t, uint32_t n) {   /* n more bytes of glyph bitmap; the arena may move, glyph offsets don't */
    if (t->aln + n > t->alcap) {
        uint32_t nc = t->alcap ? t->alcap * 2 : 16384;
        while (nc < t->aln + n) nc *= 2;
        uint8_t *a = realloc(t->al, nc);
        if (!a) return 0;
        t->al = a; t->alcap = nc;
        t->f.alpha = a;
    }
    return t->al + t->aln;
}

/* render cp of face fi at scale sc into t's arena; ascent: the baseline of the owning font. Returns 0 on failure/absent. */
static int render(struct tfont *t, int fi, float sc, uint32_t cp, struct glyph *g) {
    stbtt_fontinfo *s = &F[fi].fi;
    int gi = stbtt_FindGlyphIndex(s, cp);
    if (!gi) return 0;
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(s, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(s, gi, sc, sc, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w < 0 || h < 0 || w > 255 || h > 255) return 0;
    float a64 = adv * sc * 64;
    *g = (struct glyph){ (uint8_t)w, (uint8_t)h, (int8_t)x0, (int8_t)(t->f.ascent + y0 > 127 ? 127 : t->f.ascent + y0), (uint8_t)(adv * sc + .5f > 255 ? 255 : adv * sc + .5f), (uint16_t)(a64 > 65535 ? 65535 : a64), t->aln };
    if (w && h) {
        uint8_t *o = al_room(t, w * h);
        if (!o) return 0;
        memset(o, 0, w * h);
        stbtt_MakeGlyphBitmap(s, o, w, h, w, sc, sc, gi);
        t->aln += w * h;
    }
    return 1;
}

static const struct glyph *tf_glyph(struct tfont *t, uint32_t cp) {
    int lo = 0, hi = t->nex - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (t->ex[m].cp == cp) return t->ex[m].miss ? NULL : &t->ex[m].g;
        if (t->ex[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    struct gent e = { .cp = cp };
    int ok = render(t, t->face, t->sc, cp, &e.g);
    if (!ok && symface >= 0 && symface != t->face && face_load(symface)) {      /* the symbol face, sized to match */
        float sc = t->sc * 0.95f;
        ok = render(t, symface, sc, cp, &e.g);
    }
    e.miss = !ok;
    if (t->nex == t->excap) {
        int nc = t->excap ? t->excap * 2 : 32;
        struct gent *n = realloc(t->ex, nc * sizeof *n);
        if (!n) return NULL;
        t->ex = n; t->excap = nc;
    }
    memmove(&t->ex[lo + 1], &t->ex[lo], (t->nex - lo) * sizeof *t->ex);
    t->ex[lo] = e; t->nex++;
    return ok ? &t->ex[lo].g : NULL;
}

/* font_fallback: our own fonts (any non-ASCII glyph), then the CJK/emoji file, then a symbol for the baked faces */
static const struct glyph *(*uni_hook)(const struct font *f, uint32_t cp);
static const struct glyph *ttf_fallback(const struct font *f, uint32_t cp) {
    struct tfont *t = tfont_of(f);
    if (t) { const struct glyph *g = tf_glyph(t, cp); if (g) return g; }
    const struct glyph *g = uni_hook ? uni_hook(f, cp) : NULL;
    if (g || t || cp < 0x2190) return g;
    init_faces();                                  /* baked face: stars and dingbats from the symbol face (at most the 32x32 the shared buffer holds) */
    if (symface < 0 || !face_load(symface)) return NULL;
    static struct glyph sg;
    stbtt_fontinfo *s = &F[symface].fi;
    int gi = stbtt_FindGlyphIndex(s, cp);
    if (!gi) return NULL;
    float sc = stbtt_ScaleForMappingEmToPixels(s, f->height * 0.72f);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(s, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(s, gi, sc, sc, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w < 0 || h < 0 || w * h > GFX_FB_MAX) return NULL;
    memset(gfx_fb_alpha, 0, w * h);
    if (w && h) stbtt_MakeGlyphBitmap(s, gfx_fb_alpha, w, h, w, sc, sc, gi);
    sg = (struct glyph){ (uint8_t)w, (uint8_t)h, (int8_t)x0, (int8_t)(f->ascent + y0), (uint8_t)(adv * sc + .5f), (uint16_t)(adv * sc * 64), GLYPH_FB };
    return &sg;
}
void ttf_init(void) { uni_hook = font_fallback; font_fallback = ttf_fallback; }      /* after uni_init() */

/* FT[] index of face fi at px (made on first use); -1 when it can't be made */
int ttf_font(int fi, int px, int *idx_out) {
    if (fi < 0 || !face_load(fi)) return -1;
    if (px < 4) px = 4;
    if (px > MAXPX) px = MAXPX;
    for (int i = 0; i < ntf; i++) if (TF[i].face == fi && TF[i].px == px) { *idx_out = i; return 0; }
    if (ntf >= NFONT) return -1;
    struct tfont *t = &TF[ntf];
    memset(t, 0, sizeof *t);
    stbtt_fontinfo *s = &F[fi].fi;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(s, &asc, &desc, &gap);
    t->face = fi; t->px = px;
    t->sc = stbtt_ScaleForMappingEmToPixels(s, (float)px);
    t->f.ascent = (int)(asc * t->sc + .5f);
    t->f.height = (int)((asc - desc + gap) * t->sc + .5f);
    for (int c = 32; c < 127; c++) {                 /* ASCII is direct-indexed by gfx.c, so it is all made now */
        struct glyph *g = &t->f.g[c - 32];
        if (!render(t, fi, t->sc, c, g)) *g = (struct glyph){ .off = 0, .adv = (uint8_t)(px / 2), .adv64 = (uint16_t)(px * 32) };
    }
    t->f.alpha = t->al;
    *idx_out = ntf++;
    return 0;
}
const struct font *ttf_font_ptr(int i) { return &TF[i].f; }
