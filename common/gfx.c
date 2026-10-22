/* gfx.c - see gfx.h.  Freestanding: no libc, no floats except gfx_line_aa. */
#include "gfx.h"
#include "design.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

void canvas_init(struct canvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    canvas_noclip(c);
}

void canvas_noclip(struct canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }

void canvas_clip(struct canvas *c, int x, int y, int w, int h) {
    c->cx0 = imax(c->cx0, x); c->cy0 = imax(c->cy0, y);
    c->cx1 = imin(c->cx1, x + w); c->cy1 = imin(c->cy1, y + h);
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

color_t blend(color_t d, color_t s) {
    uint32_t a = s >> 24;
    if (a == 255) return s;
    if (a == 0) return d;
    a += a >> 7;                                   /* 0..256 */
    uint32_t rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8;
    uint32_t g  = ((s & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8;
    return 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
}

color_t mix(color_t x, color_t y, int t) {
    uint32_t rb = ((y & 0xFF00FF) * t + (x & 0xFF00FF) * (256 - t)) >> 8;
    uint32_t g  = ((y & 0x00FF00) * t + (x & 0x00FF00) * (256 - t)) >> 8;
    uint32_t a  = (((y >> 24) * t + (x >> 24) * (256 - t)) >> 8) << 24;
    return a | (rb & 0xFF00FF) | (g & 0x00FF00);
}

static inline color_t with_alpha(color_t col, uint32_t a) {   /* scale col alpha by a/255 */
    uint32_t ca = ((col >> 24) * a + 127) / 255;
    return (ca << 24) | (col & 0xFFFFFF);
}

void gfx_pixel(struct canvas *c, int x, int y, color_t col) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    uint32_t *p = &c->px[y * c->stride + x];
    *p = blend(*p, col);
}

void gfx_fill(struct canvas *c, int x, int y, int w, int h, color_t col) {
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0);
    int x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1);
    if (x0 >= x1 || y0 >= y1) return;
    if (ALPHA(col) == 255) {
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *p = &c->px[yy * c->stride + x0];
            int n = x1 - x0;
            __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(col) : "memory");
        }
    } else if (ALPHA(col)) {
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *p = &c->px[yy * c->stride];
            for (int xx = x0; xx < x1; xx++) p[xx] = blend(p[xx], col);
        }
    }
}

void gfx_rect(struct canvas *c, int x, int y, int w, int h, color_t col) {
    gfx_fill(c, x, y, w, 1, col);
    gfx_fill(c, x, y + h - 1, w, 1, col);
    gfx_fill(c, x, y + 1, 1, h - 2, col);
    gfx_fill(c, x + w - 1, y + 1, 1, h - 2, col);
}

/* ---- rounded rects: per-corner radii, fills and rings, from cached corner coverage masks ---- */
/* coverage (0..255) of pixel (j,i) in the r x r box at a top-left corner whose circle centre is (r,r); g x g samples */
static int cov_px(int r, int i, int j, int g) {
    int n = 0, R = r * 2 * g, py, px;
    for (int sy = 0; sy < g; sy++)
        for (int sx = 0; sx < g; sx++) {
            py = R - (i * 2 * g + sy * 2 + 1); px = R - (j * 2 * g + sx * 2 + 1);
            n += px * px + py * py <= R * R;
        }
    return n * 255 / (g * g);
}

#define MAXR 32                                         /* radii up to this are baked once at 16x16 samples */
static uint8_t cmask[MAXR * (MAXR + 1) * (2 * MAXR + 1) / 6];
static uint8_t cready[MAXR + 1];

static inline int cc(int r, int i, int j) {             /* corner coverage; 255 past the arc */
    if (i >= r || j >= r) return 255;
    if (r > MAXR) return cov_px(r, i, j, 4);
    uint8_t *m = cmask + (r - 1) * r * (2 * r - 1) / 6;
    if (!cready[r]) {
        for (int k = 0; k < r * r; k++) m[k] = cov_px(r, k / r, k % r, 16);
        cready[r] = 1;
    }
    return m[i * r + j];
}

/* coverage of pixel (j,i) of a w x h rounded rect, minus the same rect inset by t (t = 0: no hole) */
static int rr_cov(int i, int j, int w, int h, const int *r, int t) {
    int top = i < h / 2, left = j < w / 2;
    int ii = top ? i : h - 1 - i, jj = left ? j : w - 1 - j;
    int rad = r[top ? (left ? 0 : 1) : (left ? 3 : 2)];
    int v = cc(rad, ii, jj);
    if (t) v -= ii < t || jj < t ? 0 : cc(rad > t ? rad - t : 0, ii - t, jj - t);
    return v;
}

/* t = 0 fills, else draws a t px ring inside the edge; radii packs tl,tr,br,bl (RADII) */
static void rr_draw(struct canvas *c, int x, int y, int w, int h, uint32_t radii, int t, color_t col) {
    if (w <= 0 || h <= 0 || !ALPHA(col)) return;
    int r[4], m = imin(w, h) / 2;
    for (int k = 0; k < 4; k++) r[k] = imin(radii >> 8 * k & 255, m);
    if (t < 0 || 2 * t >= w || 2 * t >= h) t = 0;
    int bt = imax(imax(r[0], r[1]), t), bb = imax(imax(r[2], r[3]), t);   /* rows needing per-row work */
    if (t) { gfx_fill(c, x, y + bt, t, h - bt - bb, col); gfx_fill(c, x + w - t, y + bt, t, h - bt - bb, col); }
    else gfx_fill(c, x, y + bt, w, h - bt - bb, col);
    for (int i = 0; i < h; i++) {
        if (i == bt && (i = h - bb) >= h) break;
        if (y + i < c->cy0 || y + i >= c->cy1) continue;
        int top = i < h / 2, ii = top ? i : h - 1 - i;
        int rl = r[top ? 0 : 3], rr = r[top ? 1 : 2];
        int ls = imax(ii < rl ? rl : 0, t), rs = imax(ii < rr ? rr : 0, t);
        for (int j = 0; j < ls; j++) {
            int v = rr_cov(i, j, w, h, r, t);
            if (v > 0) gfx_pixel(c, x + j, y + i, with_alpha(col, v));
        }
        for (int j = w - rs; j < w; j++) {
            int v = rr_cov(i, j, w, h, r, t);
            if (v > 0) gfx_pixel(c, x + j, y + i, with_alpha(col, v));
        }
        if (!t || ii < t) gfx_fill(c, x + ls, y + i, w - ls - rs, 1, col);
    }
}

void gfx_rrect_fill(struct canvas *c, int x, int y, int w, int h, uint32_t radii, color_t col) { rr_draw(c, x, y, w, h, radii, 0, col); }
void gfx_rrect_ring(struct canvas *c, int x, int y, int w, int h, uint32_t radii, int t, color_t col) { rr_draw(c, x, y, w, h, radii, t > 0 ? t : 1, col); }
void gfx_round_fill(struct canvas *c, int x, int y, int w, int h, int r, color_t col) { rr_draw(c, x, y, w, h, RADII(r, r, r, r), 0, col); }
void gfx_round_rect(struct canvas *c, int x, int y, int w, int h, int r, color_t col) { rr_draw(c, x, y, w, h, RADII(r, r, r, r), 1, col); }
void gfx_ring_out(struct canvas *c, int x, int y, int w, int h, int r, color_t col) {
    rr_draw(c, x - 1, y - 1, w + 2, h + 2, RADII(r + 1, r + 1, r + 1, r + 1), 1, col);
}
void gfx_focus_ring(struct canvas *c, int x, int y, int w, int h, int r, color_t col) {
    int o = FOCUS_GAP + FOCUS_W, ro = r + o;
    rr_draw(c, x - o, y - o, w + 2 * o, h + 2 * o, RADII(ro, ro, ro, ro), FOCUS_W, col);
}

void gfx_vgradient(struct canvas *c, int x, int y, int w, int h, color_t top, color_t bot) {
    for (int i = 0; i < h; i++)
        gfx_fill(c, x, y + i, w, 1, mix(top, bot, h > 1 ? i * 256 / (h - 1) : 0));
}

/* legacy soft shadow (stacked translucent rounded rects); new code uses gfx_elev */
/* soft drop shadow for a (rounded) rect the caller then draws on top: only the four bands
 * around the edge are filled, since the inside is hidden by the content anyway */
void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int r, int spread, int strength) {
    int in = r > 0 ? r : 0;
    struct { int x, y, w, h; } band[4] = {
        { x - 2 * spread, y - 2 * spread, w + 4 * spread, 2 * spread + in },            /* top */
        { x - 2 * spread, y + h - in, w + 4 * spread, 3 * spread + in },                 /* bottom */
        { x - 2 * spread, y + in, 2 * spread + in, h - 2 * in },                         /* left */
        { x + w - in, y + in, 2 * spread + in, h - 2 * in },                             /* right */
    };
    for (int b = 0; b < 4; b++) {
        if (band[b].w <= 0 || band[b].h <= 0) continue;
        struct canvas cc = *c;
        canvas_clip(&cc, band[b].x, band[b].y, band[b].w, band[b].h);
        for (int i = spread; i > 0; i--) {
            int a = strength * (spread - i + 1) / (spread * spread / 2 + 1);
            if (a <= 0) continue;
            gfx_round_fill(&cc, x - i, y - i + spread / 3, w + 2 * i, h + 2 * i, r + i, RGBA(0, 0, 0, a));
        }
    }
}

/* ---- baked 9-slice shadows ----
 * One corner tile (c x c) and one edge profile (c) per (radius, blur): the blurred coverage of a rounded rect corner,
 * three box passes of radius blur/2 (close to a Gaussian with sigma = blur/2), reaching e = 3 * (blur/2) px out.
 * All four corners and edges are mirrors, so a shadow of any size is blits from these. */
struct shtile { int r, blur, e, c; uint8_t *px, *edge; };
static uint8_t shpool[32768];                           /* ponytail: one bump pool, wiped when full; fits E1-E4 at the 4/8 radii */
static struct shtile sht[12];
static int shused, nsh;

static void box3(uint8_t *p, int n, int step, int rb) { /* 3 box passes along n samples: zero before 0, clamped after */
    static uint8_t tmp[512];
    int d = 2 * rb + 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < n; i++) tmp[i] = p[i * step];
#define AT(k) ((k) < 0 ? 0 : tmp[(k) < n ? (k) : n - 1])
        int sum = 0;
        for (int k = -rb; k <= rb; k++) sum += AT(k);
        for (int i = 0; i < n; i++) { p[i * step] = (sum + d / 2) / d; sum += AT(i + rb + 1) - AT(i - rb); }
#undef AT
    }
}

static const struct shtile *shtile(int r, int blur) {
    for (int k = 0; k < nsh; k++) if (sht[k].r == r && sht[k].blur == blur) return &sht[k];
    int rb = blur / 2 > 0 ? blur / 2 : 1, e = 3 * rb, cs = r + 2 * e, need = cs * cs + cs;
    if (cs > 512 || need > (int)sizeof shpool) return 0;
    if (nsh == (int)(sizeof sht / sizeof sht[0]) || shused + need > (int)sizeof shpool) nsh = shused = 0;
    struct shtile *t = &sht[nsh];
    *t = (struct shtile){ r, blur, e, cs, shpool + shused, shpool + shused + cs * cs };
    for (int y = 0; y < cs; y++)
        for (int x = 0; x < cs; x++) t->px[y * cs + x] = x < e || y < e ? 0 : cc(r, y - e, x - e);
    for (int y = 0; y < cs; y++) box3(t->px + y * cs, cs, 1, rb);
    for (int x = 0; x < cs; x++) box3(t->px + x, cs, cs, rb);
    for (int y = 0; y < cs; y++) t->edge[y] = y < e ? 0 : 255;
    box3(t->edge, cs, 1, rb);
    shused += need; nsh++;
    return t;
}

void gfx_shadow9(struct canvas *c, int x, int y, int w, int h, int r, int dy, int blur, int alpha) {
    if (blur <= 0 || alpha <= 0 || w <= 0 || h <= 0) return;
    r = imin(r, imin(w, h) / 2);
    const struct shtile *t = shtile(r, blur);
    if (!t) return;
    int e = t->e, cs = t->c, W = w + 2 * e, H = h + 2 * e, sx0 = x - e, sy0 = y + dy - e;
    int hy0 = y + r, hy1 = y + h - r;                   /* the caster's own rows (minus its corners) are left alone */
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) lut[v] = (v * imin(alpha, 255) + 127) / 255;
    int x0 = imax(sx0, c->cx0), x1 = imin(sx0 + W, c->cx1);
    for (int yy = imax(sy0, c->cy0); yy < imin(sy0 + H, c->cy1); yy++) {
        int ly = yy - sy0, ry = H - 1 - ly, vy = ly < cs && ly <= ry ? ly : ry < cs ? ry : -1;
        uint32_t *row = &c->px[yy * c->stride];
        int hole = yy >= hy0 && yy < hy1, ha = imax(x0, x), hb = imin(x1, x + w);
        for (int xx = x0; xx < x1; xx++) {
            if (hole && xx == ha && ha < hb) { xx = hb - 1; continue; }
            int lx = xx - sx0, rx = W - 1 - lx, vx = lx < cs && lx <= rx ? lx : rx < cs ? rx : -1;
            int v = vy < 0 ? (vx < 0 ? 255 : t->edge[vx]) : vx < 0 ? t->edge[vy] : t->px[vy * cs + vx];
            if (lut[v]) row[xx] = blend(row[xx], (uint32_t)lut[v] << 24);
        }
    }
}

void gfx_elev(struct canvas *c, int x, int y, int w, int h, int r, int level, int alpha) {
    if (level > E0 && level <= E4) gfx_shadow9(c, x, y, w, h, r, ELEV[level].dy, ELEV[level].blur, alpha);
}

/* ---- A8 masks (glyphs, icons) tinted with col; col's alpha scales the mask ---- */
void gfx_mask(struct canvas *c, int x, int y, const uint8_t *a, int w, int h, int stride, color_t col) {
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0), x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1);
    uint32_t ca = col >> 24, rgb = col & 0xFFFFFF;
    if (!ca) return;
    for (int yy = y0; yy < y1; yy++) {
        const uint8_t *m = a + (yy - y) * stride - x;
        uint32_t *row = &c->px[yy * c->stride];
        for (int xx = x0; xx < x1; xx++) {
            uint32_t v = m[xx];
            if (!v) continue;
            if (ca != 255) v = v * ca / 255;
            row[xx] = blend(row[xx], v << 24 | rgb);
        }
    }
}

void gfx_circle_fill(struct canvas *c, int cx, int cy, int r, color_t col) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int cov = 0;
            for (int s = 0; s < 4; s++) {
                int sx = dx * 4 + (s & 1) * 2 - 1, sy = dy * 4 + (s >> 1) * 2 - 1;
                if (sx * sx + sy * sy <= r * r * 16) cov++;
            }
            if (cov) gfx_pixel(c, cx + dx, cy + dy, with_alpha(col, cov * 255 / 4));
        }
}

void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, color_t col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static float sqrtf_(float v) { float r; __asm__("fsqrt" : "=t"(r) : "0"(v)); return r; }

/* thick anti-aliased line: distance-to-segment coverage over the bounding box */
void gfx_line_aa(struct canvas *c, float x0, float y0, float x1, float y1, float width, color_t col) {
    float hw = width / 2;
    int bx0 = (int)((x0 < x1 ? x0 : x1) - hw - 1), bx1 = (int)((x0 > x1 ? x0 : x1) + hw + 2);
    int by0 = (int)((y0 < y1 ? y0 : y1) - hw - 1), by1 = (int)((y0 > y1 ? y0 : y1) + hw + 2);
    bx0 = imax(bx0, c->cx0); bx1 = imin(bx1, c->cx1); by0 = imax(by0, c->cy0); by1 = imin(by1, c->cy1);
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    float avy = vy < 0 ? -vy : vy, slope = avy > 0.01f ? vx / vy : 0;
    float reach = avy > 0.01f ? (hw + 1.5f) * sqrtf_(len2) / avy : 1e9f;   /* strip half-width per row */
    float in2 = hw > 0.5f ? (hw - 0.5f) * (hw - 0.5f) : 0, out2 = (hw + 0.5f) * (hw + 0.5f);
    for (int y = by0; y < by1; y++) {
        int xa = bx0, xb = bx1;
        if (reach < 1e8f) {                                   /* only the pixels near the line on this row */
            float xc = x0 + slope * (y + 0.5f - y0);
            xa = imax(xa, (int)(xc - reach)); xb = imin(xb, (int)(xc + reach) + 1);
        }
        for (int x = xa; x < xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float ex = px - t * vx, ey = py - t * vy, d2 = ex * ex + ey * ey;
            if (d2 >= out2) continue;
            float cov = d2 <= in2 ? 1 : 0.5f - (sqrtf_(d2) - hw);
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            gfx_pixel(c, x, y, with_alpha(col, (uint32_t)(cov * 255)));
        }
    }
}

void gfx_blit(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int stride) {
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0);
    int x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1);
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *d = &c->px[yy * c->stride + x0];
        const uint32_t *s = &src[(yy - y) * stride + (x0 - x)];
        int n = x1 - x0;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    }
}

void gfx_blit_alpha(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int stride) {
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0);
    int x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *d = &c->px[yy * c->stride];
        const uint32_t *s = &src[(yy - y) * stride - x];
        for (int xx = x0; xx < x1; xx++) d[xx] = blend(d[xx], s[xx]);
    }
}

void gfx_blit_scaled(struct canvas *c, int x, int y, int dw, int dh, const uint32_t *src, int sw, int sh, int stride) {
    if (dw <= 0 || dh <= 0) return;
    for (int yy = imax(y, c->cy0); yy < imin(y + dh, c->cy1); yy++) {
        const uint32_t *row = &src[((yy - y) * sh / dh) * stride];
        uint32_t *d = &c->px[yy * c->stride];
        for (int xx = imax(x, c->cx0); xx < imin(x + dw, c->cx1); xx++)
            d[xx] = blend(d[xx], row[(xx - x) * sw / dw]);
    }
}

const struct glyph *font_glyph(const struct font *f, uint32_t cp) {
    if (cp >= 32 && cp < 127) return &f->g[cp - 32];
    int lo = 0, hi = f->nx - 1;                               /* extras are sorted */
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f->xcp[m] == cp) return &f->xg[m];
        if (f->xcp[m] < cp) lo = m + 1; else hi = m - 1;
    }
    return &f->g['?' - 32];
}

/* decode one UTF-8 sequence from s (at most n bytes), returns bytes used (>= 1) */
int utf8_decode(const char *s, int n, uint32_t *cp) {
    const uint8_t *u = (const uint8_t *)s;
    int len = u[0] < 0x80 ? 1 : u[0] >= 0xF0 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC2 ? 2 : 0;
    if (!len || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = len == 1 ? u[0] : u[0] & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return i; }
        v = v << 6 | (u[i] & 0x3F);
    }
    *cp = v;
    return len;
}

int gfx_char(struct canvas *c, const struct font *f, int x, int y, uint32_t cp, color_t col) {
    const struct glyph *g = font_glyph(f, cp);
    gfx_mask(c, x + g->xoff, y + g->yoff, f->alpha + g->off, g->w, g->h, g->w, col);
    return g->adv;
}

int gfx_text_n(struct canvas *c, const struct font *f, int x, int y, const char *s, int n, color_t col) {
    int x0 = x;
    uint32_t cp;
    for (int i = 0; i < n && s[i]; ) {
        i += utf8_decode(s + i, n - i, &cp);
        x += gfx_char(c, f, x, y, cp, col);
    }
    return x - x0;
}

int gfx_text(struct canvas *c, const struct font *f, int x, int y, const char *s, color_t col) {
    return gfx_text_n(c, f, x, y, s, 1 << 30, col);
}

int gfx_text_width_n(const struct font *f, const char *s, int n) {
    int w = 0;
    uint32_t cp;
    for (int i = 0; i < n && s[i]; ) {
        i += utf8_decode(s + i, n - i, &cp);
        w += font_glyph(f, cp)->adv;
    }
    return w;
}

int gfx_text_width(const struct font *f, const char *s) { return gfx_text_width_n(f, s, 1 << 30); }

void gfx_text_center(struct canvas *c, const struct font *f, int x, int y, int w, const char *s, color_t col) {
    gfx_text(c, f, x + (w - gfx_text_width(f, s)) / 2, y, s, col);
}
