/* gfx.c - see gfx.h.  Freestanding: no libc, no floats except gfx_line_aa. */
#include "gfx.h"

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

/* coverage (0..255) of a corner pixel whose near edge is (dx,dy) pixels from
 * the corner circle's centre; 4x4 supersampled */
static int corner_cov(int dx, int dy, int r) {
    int cov = 0, rr = r * r * 64;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int px = dx * 8 + sx * 2 + 1, py = dy * 8 + sy * 2 + 1;
            if (px * px + py * py <= rr) cov++;
        }
    return cov * 255 / 16;
}

void gfx_round_fill(struct canvas *c, int x, int y, int w, int h, int r, color_t col) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_fill(c, x, y, w, h, col); return; }
    gfx_fill(c, x, y + r, w, h - 2 * r, col);
    for (int i = 0; i < r; i++) {                  /* rows of the top/bottom bands */
        int dy = r - 1 - i;                        /* distance from centre row, pixel units */
        for (int j = 0; j < r; j++) {
            int dx = r - 1 - j;
            int cov = corner_cov(dx, dy, r);
            if (!cov) continue;
            color_t cc = with_alpha(col, cov);
            gfx_pixel(c, x + j, y + i, cc);
            gfx_pixel(c, x + w - 1 - j, y + i, cc);
            gfx_pixel(c, x + j, y + h - 1 - i, cc);
            gfx_pixel(c, x + w - 1 - j, y + h - 1 - i, cc);
        }
        gfx_fill(c, x + r, y + i, w - 2 * r, 1, col);
        gfx_fill(c, x + r, y + h - 1 - i, w - 2 * r, 1, col);
    }
}

void gfx_round_rect(struct canvas *c, int x, int y, int w, int h, int r, color_t col) {
    if (r <= 0) { gfx_rect(c, x, y, w, h, col); return; }
    gfx_fill(c, x + r, y, w - 2 * r, 1, col);
    gfx_fill(c, x + r, y + h - 1, w - 2 * r, 1, col);
    gfx_fill(c, x, y + r, 1, h - 2 * r, col);
    gfx_fill(c, x + w - 1, y + r, 1, h - 2 * r, col);
    for (int i = 0; i < r; i++)
        for (int j = 0; j < r; j++) {
            int dx = r - 1 - j, dy = r - 1 - i;
            int outer = corner_cov(dx, dy, r), inner = r > 1 ? corner_cov(dx, dy, r - 1) : 0;
            int cov = outer - inner;
            if (cov <= 0) continue;
            color_t cc = with_alpha(col, cov);
            gfx_pixel(c, x + j, y + i, cc);
            gfx_pixel(c, x + w - 1 - j, y + i, cc);
            gfx_pixel(c, x + j, y + h - 1 - i, cc);
            gfx_pixel(c, x + w - 1 - j, y + h - 1 - i, cc);
        }
}

void gfx_vgradient(struct canvas *c, int x, int y, int w, int h, color_t top, color_t bot) {
    for (int i = 0; i < h; i++)
        gfx_fill(c, x, y + i, w, 1, mix(top, bot, h > 1 ? i * 256 / (h - 1) : 0));
}

/* soft drop shadow: stacked translucent rounded rects */
void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int r, int spread, int strength) {
    for (int i = spread; i > 0; i--) {
        int a = strength * (spread - i + 1) / (spread * spread / 2 + 1);
        if (a <= 0) continue;
        gfx_round_fill(c, x - i, y - i + spread / 3, w + 2 * i, h + 2 * i, r + i, RGBA(0, 0, 0, a));
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
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            float ex = px - t * vx, ey = py - t * vy;
            float d = sqrtf_(ex * ex + ey * ey) - hw;   /* <0 inside */
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            gfx_pixel(c, x, y, with_alpha(col, (uint32_t)(cov * 255)));
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
    const uint8_t *a = f->alpha + g->off;
    int gx = x + g->xoff, gy = y + g->yoff;
    int ca = col >> 24;
    for (int j = 0; j < g->h; j++) {
        int yy = gy + j;
        if (yy < c->cy0 || yy >= c->cy1) continue;
        uint32_t *row = &c->px[yy * c->stride];
        for (int i = 0; i < g->w; i++) {
            int xx = gx + i;
            if (xx < c->cx0 || xx >= c->cx1) continue;
            uint32_t v = a[j * g->w + i];
            if (!v) continue;
            v = v * ca / 255;
            row[xx] = blend(row[xx], (v << 24) | (col & 0xFFFFFF));
        }
    }
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
