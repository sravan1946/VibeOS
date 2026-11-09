/* canvas.c - the pixel side of <canvas> 2D: RGBA8 (straight alpha) buffers, an anti-aliased polygon filler (4 sub-scanlines,
 * exact horizontal coverage), paints (solid, linear/radial gradient, canvas pattern), Porter-Duff compositing and bitmap text
 * (the browser's own fonts, so measureText agrees with what is drawn). The geometry (paths, curves, strokes, transforms, state)
 * lives in the JS prelude (dom_js.c, class CanvasRenderingContext2D) and arrives here as flat double arrays; js.c wraps
 * these calls as natives. Canvases are numbered slots; when all are taken the least recently used one is recycled
 * (ponytail: no finalizers, a page that keeps more than CV_MAX canvases alive draws into a recycled one). */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CV_MAX 64
#define CV_PIX (1 << 22)                        /* pixels per canvas: 16 MB of RGBA */
struct cnv { int w, h; uint8_t *px; unsigned use; };
static struct cnv CNV[CV_MAX + 1];
static unsigned cnv_tick;

void cnv_reset(void) { for (int i = 1; i <= CV_MAX; i++) { free(CNV[i].px); CNV[i].px = 0; CNV[i].w = CNV[i].h = 0; } }

static int cnv_alloc(int id, int w, int h) {
    if (w < 1 || h < 1 || (long)w * h > CV_PIX) return 0;
    uint8_t *p = calloc((size_t)w * h, 4);
    if (!p) return 0;
    free(CNV[id].px);
    CNV[id].px = p; CNV[id].w = w; CNV[id].h = h; CNV[id].use = ++cnv_tick;
    return id;
}
int cnv_new(int w, int h) {
    int id = 0;
    unsigned old = ~0u;
    for (int i = 1; i <= CV_MAX; i++) {
        if (!CNV[i].px) { id = i; break; }
        if (CNV[i].use < old) { old = CNV[i].use; id = i; }
    }
    return cnv_alloc(id, w, h);
}
int cnv_resize(int id, int w, int h) { return id > 0 && id <= CV_MAX ? cnv_alloc(id, w, h) : 0; }
static struct cnv *cnv_get(int id) { if (id < 1 || id > CV_MAX || !CNV[id].px) return 0; CNV[id].use = ++cnv_tick; return &CNV[id]; }

/* ---------- coverage ---------- */
struct cover { int x0, y0, w, h; uint8_t *a; };
struct edge { double x0, y0, x1, y1, k; int dir; };

static void addspan(float *acc, double xa, double xb, int bw) {
    if (xa < 0) xa = 0;
    if (xb > bw) xb = bw;
    if (xb <= xa) return;
    int ia = (int)xa, ib = (int)xb;
    if (ia == ib) { acc[ia] += (float)(xb - xa); return; }
    acc[ia] += (float)(ia + 1 - xa);
    for (int i = ia + 1; i < ib; i++) acc[i] += 1;
    if (ib < bw) acc[ib] += (float)(xb - ib);
}

/* v: subpaths as n, x0, y0, ..., x(n-1), y(n-1); each is closed implicitly. 0 when nothing is covered. */
static int cnv_raster(const struct cnv *k, const double *v, int nv, int evenodd, struct cover *o) {
    double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
    int ne = 0;
    for (int i = 0; i < nv;) {
        int n = (int)v[i++];
        if (n < 0 || i + 2 * n > nv) return 0;
        for (int j = 0; j < n; j++) {
            double x = v[i + 2 * j], y = v[i + 2 * j + 1];
            if (x != x || y != y) return 0;
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
        }
        ne += n;
        i += 2 * n;
    }
    if (!ne) return 0;
    int x0 = mnx < 0 ? 0 : (int)floor(mnx), y0 = mny < 0 ? 0 : (int)floor(mny);
    int x1 = mxx > k->w ? k->w : (int)ceil(mxx), y1 = mxy > k->h ? k->h : (int)ceil(mxy);
    if (x1 <= x0 || y1 <= y0) return 0;
    struct edge *E = malloc(sizeof *E * ne);
    double *cx = malloc(sizeof *cx * (ne + 1));
    int *cd = malloc(sizeof *cd * (ne + 1));
    int bw = x1 - x0, bh = y1 - y0;
    float *acc = malloc(sizeof *acc * bw);
    uint8_t *a = malloc((size_t)bw * bh);
    if (!E || !cx || !cd || !acc || !a) { free(E); free(cx); free(cd); free(acc); free(a); return 0; }
    int m = 0;
    for (int i = 0; i < nv;) {
        int n = (int)v[i++];
        for (int j = 0; j < n; j++) {
            const double *p = v + i + 2 * j, *q = v + i + 2 * ((j + 1) % n);
            if (p[1] == q[1]) continue;
            struct edge *e = &E[m++];
            if (p[1] < q[1]) { e->x0 = p[0]; e->y0 = p[1]; e->x1 = q[0]; e->y1 = q[1]; e->dir = 1; }
            else { e->x0 = q[0]; e->y0 = q[1]; e->x1 = p[0]; e->y1 = p[1]; e->dir = -1; }
            e->k = (e->x1 - e->x0) / (e->y1 - e->y0);
        }
        i += 2 * n;
    }
    for (int y = 0; y < bh; y++) {
        memset(acc, 0, sizeof *acc * bw);
        for (int s = 0; s < 4; s++) {
            double sy = y0 + y + (s + 0.5) / 4;
            int nc = 0;
            for (int i = 0; i < m; i++) {
                if (sy < E[i].y0 || sy >= E[i].y1) continue;
                double x = E[i].x0 + (sy - E[i].y0) * E[i].k;
                int j = nc++;
                while (j > 0 && cx[j - 1] > x) { cx[j] = cx[j - 1]; cd[j] = cd[j - 1]; j--; }
                cx[j] = x; cd[j] = E[i].dir;
            }
            int wind = 0;
            double xs = 0;
            for (int i = 0; i < nc; i++) {
                int was = evenodd ? (wind & 1) : wind != 0;
                wind += cd[i];
                int now = evenodd ? (wind & 1) : wind != 0;
                if (!was && now) xs = cx[i];
                else if (was && !now) addspan(acc, xs - x0, cx[i] - x0, bw);
            }
        }
        for (int x = 0; x < bw; x++) { float c = acc[x] * 63.75f + 0.5f; a[(size_t)y * bw + x] = c > 255 ? 255 : (uint8_t)c; }
    }
    free(E); free(cx); free(cd); free(acc);
    o->x0 = x0; o->y0 = y0; o->w = bw; o->h = bh; o->a = a;
    return 1;
}

/* ---------- paint ---------- */
struct paint { int type, ns, src, repx, repy, smooth; double c[4], x0, y0, x1, y1, r0, r1, m[6]; const double *st; };
/* p[]: 0 solid: r g b a(0..1) | 1 linear: x0 y0 x1 y1 n stops | 2 radial: x0 y0 r0 x1 y1 r1 n stops | 3 canvas pattern: src a b c d e f repx repy smooth
 * stops are off r g b a (r..b 0..255, sorted by off) */
static int paint_parse(const double *p, int n, struct paint *o) {
    memset(o, 0, sizeof *o);
    if (n < 1) return 0;
    o->type = (int)p[0];
    if (o->type == 0 && n >= 5) { for (int i = 0; i < 4; i++) o->c[i] = p[1 + i]; return 1; }
    if (o->type == 1 && n >= 6) { o->x0 = p[1]; o->y0 = p[2]; o->x1 = p[3]; o->y1 = p[4]; o->ns = (int)p[5]; o->st = p + 6; return n >= 6 + 5 * o->ns && o->ns > 0; }
    if (o->type == 2 && n >= 8) { o->x0 = p[1]; o->y0 = p[2]; o->r0 = p[3]; o->x1 = p[4]; o->y1 = p[5]; o->r1 = p[6]; o->ns = (int)p[7]; o->st = p + 8; return n >= 8 + 5 * o->ns && o->ns > 0; }
    if (o->type == 3 && n >= 11) { o->src = (int)p[1]; for (int i = 0; i < 6; i++) o->m[i] = p[2 + i]; o->repx = (int)p[8]; o->repy = (int)p[9]; o->smooth = (int)p[10]; return 1; }
    return 0;
}
static void ramp(const struct paint *p, double t, double *rgba) {
    const double *s = p->st;
    int n = p->ns;
    const double *a = s, *b = s + 5 * (n - 1);
    if (t <= s[0]) b = a;
    else if (t >= b[0]) a = b;
    else for (int i = 0; i + 1 < n; i++) if (t >= s[5 * i] && t <= s[5 * i + 5]) { a = s + 5 * i; b = a + 5; break; }
    double f = b[0] > a[0] && a != b ? (t - a[0]) / (b[0] - a[0]) : 0;
    for (int i = 0; i < 4; i++) rgba[i] = a[1 + i] + (b[1 + i] - a[1 + i]) * f;
}
static void texel(const struct cnv *s, int x, int y, int repx, int repy, double *o) {
    if (x < 0 || x >= s->w) { if (!repx) { o[0] = o[1] = o[2] = o[3] = 0; return; } x = ((x % s->w) + s->w) % s->w; }
    if (y < 0 || y >= s->h) { if (!repy) { o[0] = o[1] = o[2] = o[3] = 0; return; } y = ((y % s->h) + s->h) % s->h; }
    const uint8_t *q = s->px + ((size_t)y * s->w + x) * 4;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[3] / 255.0;
}
/* straight colour at device point (x+.5, y+.5): rgb 0..255, a 0..1 */
static void paint_at(const struct paint *p, int px, int py, double *o) {
    double x = px + 0.5, y = py + 0.5;
    if (p->type == 0) { memcpy(o, p->c, sizeof p->c); return; }
    if (p->type == 1) {
        double dx = p->x1 - p->x0, dy = p->y1 - p->y0, l = dx * dx + dy * dy;
        double t = l > 0 ? ((x - p->x0) * dx + (y - p->y0) * dy) / l : 0;
        ramp(p, t < 0 ? 0 : t > 1 ? 1 : t, o);
        return;
    }
    if (p->type == 2) {
        double dx = p->x1 - p->x0, dy = p->y1 - p->y0, dr = p->r1 - p->r0, qx = x - p->x0, qy = y - p->y0;
        double A = dx * dx + dy * dy - dr * dr, B = qx * dx + qy * dy + p->r0 * dr, C = qx * qx + qy * qy - p->r0 * p->r0, s = -1e30;
        if (fabs(A) < 1e-9) { if (fabs(B) > 1e-9) s = C / (2 * B); }
        else {
            double disc = B * B - A * C;
            if (disc >= 0) {
                double sq = sqrt(disc), s1 = (B + sq) / A, s2 = (B - sq) / A, hi = s1 > s2 ? s1 : s2, lo = s1 > s2 ? s2 : s1;
                s = p->r0 + hi * dr >= 0 ? hi : (p->r0 + lo * dr >= 0 ? lo : -1e30);
            }
        }
        if (s < -1e29 || p->r0 + s * dr < 0) { o[0] = o[1] = o[2] = o[3] = 0; return; }
        ramp(p, s < 0 ? 0 : s > 1 ? 1 : s, o);
        return;
    }
    const struct cnv *s = cnv_get(p->src);
    if (!s) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    double sx = p->m[0] * x + p->m[2] * y + p->m[4], sy = p->m[1] * x + p->m[3] * y + p->m[5];
    if (!p->smooth) { texel(s, (int)floor(sx), (int)floor(sy), p->repx, p->repy, o); return; }
    double fx = sx - 0.5, fy = sy - 0.5, t[4][4];
    int ix = (int)floor(fx), iy = (int)floor(fy);
    fx -= ix; fy -= iy;
    texel(s, ix, iy, p->repx, p->repy, t[0]); texel(s, ix + 1, iy, p->repx, p->repy, t[1]);
    texel(s, ix, iy + 1, p->repx, p->repy, t[2]); texel(s, ix + 1, iy + 1, p->repx, p->repy, t[3]);
    double w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy }, aa = 0, c[3] = { 0, 0, 0 };
    for (int i = 0; i < 4; i++) { aa += w[i] * t[i][3]; for (int j = 0; j < 3; j++) c[j] += w[i] * t[i][3] * t[i][j]; }
    for (int j = 0; j < 3; j++) o[j] = aa > 0 ? c[j] / aa : 0;
    o[3] = aa;
}

/* ops: 0 source-over 1 source-in 2 source-out 3 source-atop 4 destination-over 5 destination-in 6 destination-out
 * 7 destination-atop 8 lighter 9 copy 10 xor */
static void composite(struct cnv *k, const struct cover *c, const struct paint *p, double alpha, int op, const uint8_t *mask) {
    for (int y = 0; y < c->h; y++) {
        int dy = c->y0 + y;
        for (int x = 0; x < c->w; x++) {
            int cov = c->a[(size_t)y * c->w + x], dx = c->x0 + x;
            if (!cov) continue;
            if (mask) cov = cov * mask[(size_t)dy * k->w + dx] / 255;
            if (!cov) continue;
            double s[4];
            paint_at(p, dx, dy, s);
            double sa = s[3] * alpha * cov / 255;
            uint8_t *d = k->px + ((size_t)dy * k->w + dx) * 4;
            double ad = d[3] / 255.0, Fa, Fb;
            switch (op) {
            case 1: Fa = ad; Fb = 0; break;
            case 2: Fa = 1 - ad; Fb = 0; break;
            case 3: Fa = ad; Fb = 1 - sa; break;
            case 4: Fa = 1 - ad; Fb = 1; break;
            case 5: Fa = 0; Fb = sa; break;
            case 6: Fa = 0; Fb = 1 - sa; break;
            case 7: Fa = 1 - ad; Fb = sa; break;
            case 8: Fa = 1; Fb = 1; break;
            case 9: Fa = 1; Fb = 0; break;
            case 10: Fa = 1 - ad; Fb = 1 - sa; break;
            default: Fa = 1; Fb = 1 - sa;
            }
            double oa = sa * Fa + ad * Fb, o[3];
            for (int j = 0; j < 3; j++) o[j] = s[j] * sa * Fa + d[j] * ad * Fb;
            if (oa > 1) oa = 1;
            if (oa <= 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            for (int j = 0; j < 3; j++) { double v = o[j] / oa + 0.5; d[j] = v > 255 ? 255 : (uint8_t)v; }
            d[3] = (uint8_t)(oa * 255 + 0.5);
        }
    }
}

int cnv_fill(int id, const double *poly, int np, int evenodd, const double *pn, int npn, double alpha, int op, const uint8_t *mask) {
    struct cnv *k = cnv_get(id);
    struct paint p;
    struct cover c;
    if (!k || !paint_parse(pn, npn, &p) || !cnv_raster(k, poly, np, evenodd, &c)) return 0;
    composite(k, &c, &p, alpha, op, mask);
    free(c.a);
    return 1;
}

/* the clip mask (w*h bytes) of the shape, intersected with prev (NULL = everything) */
uint8_t *cnv_clip(int id, const double *poly, int np, int evenodd, const uint8_t *prev, size_t *len) {
    struct cnv *k = cnv_get(id);
    struct cover c;
    if (!k) return 0;
    uint8_t *m = calloc((size_t)k->w * k->h, 1);
    if (!m) return 0;
    if (cnv_raster(k, poly, np, evenodd, &c)) {
        for (int y = 0; y < c.h; y++)
            for (int x = 0; x < c.w; x++) {
                size_t i = (size_t)(c.y0 + y) * k->w + c.x0 + x;
                m[i] = prev ? c.a[(size_t)y * c.w + x] * prev[i] / 255 : c.a[(size_t)y * c.w + x];
            }
        free(c.a);
    }
    *len = (size_t)k->w * k->h;
    return m;
}

/* ---------- text ---------- */
/* flags: 1 bold, 2 monospace. The browser's bitmap faces stand in for any CSS font at the nearest size. */
static const struct font *pick(double size, int flags) {
    int b = flags & 1;
    if (flags & 2) return b ? &bfont_mono_bold : &bfont_mono;
    if (size <= 11.5) return b ? &font_caption_bold : &font_caption;
    if (size <= 14.5) return b ? &font_body_bold : &font_body;
    if (size <= 17.5) return b ? &bfont_ui_bold : &bfont_ui;
    if (size <= 24) return &font_title;
    return &font_display;
}
void cnv_measure(const char *s, double size, int flags, double *w, double *asc, double *desc) {
    const struct font *f = pick(size, flags);
    *w = gfx_text_width(f, s);
    *asc = f->ascent;
    *desc = f->height - f->ascent;
}
int cnv_text(int id, const char *s, double x, double y, double size, int flags, const double *pn, int npn, double alpha, int op, const uint8_t *mask) {
    struct cnv *k = cnv_get(id);
    struct paint p;
    const struct font *f = pick(size, flags);
    int tw = gfx_text_width(f, s);
    if (!k || !paint_parse(pn, npn, &p) || tw < 1 || tw > 8192) return 0;
    uint32_t *t = malloc(sizeof *t * (tw + 4) * f->height);
    if (!t) return 0;
    for (int i = 0; i < (tw + 4) * f->height; i++) t[i] = 0xFF000000u;
    struct canvas cc;
    canvas_init(&cc, t, tw + 4, f->height, tw + 4);
    gfx_text(&cc, f, 0, 0, s, 0xFFFFFFFFu);
    struct cover c;
    int x0 = (int)floor(x + 0.5), y0 = (int)floor(y + 0.5) - f->ascent;
    int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0, cx1 = x0 + tw + 4 > k->w ? k->w : x0 + tw + 4, cy1 = y0 + f->height > k->h ? k->h : y0 + f->height;
    if (cx1 <= cx0 || cy1 <= cy0) { free(t); return 0; }
    c.x0 = cx0; c.y0 = cy0; c.w = cx1 - cx0; c.h = cy1 - cy0;
    c.a = malloc((size_t)c.w * c.h);
    if (!c.a) { free(t); return 0; }
    for (int j = 0; j < c.h; j++)
        for (int i = 0; i < c.w; i++) c.a[(size_t)j * c.w + i] = (t[(size_t)(cy0 - y0 + j) * (tw + 4) + (cx0 - x0 + i)] >> 8) & 255;
    composite(k, &c, &p, alpha, op, mask);
    free(c.a); free(t);
    return 1;
}

/* ---------- pixels in and out ---------- */
uint8_t *cnv_get_rgba(int id, int x, int y, int w, int h) {
    struct cnv *k = cnv_get(id);
    if (w < 0 || h < 0 || (long)w * h > CV_PIX) return 0;
    uint8_t *o = calloc((size_t)w * h + 1, 4);
    if (!o || !k) return o;
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if (sy < 0 || sy >= k->h) continue;
        for (int i = 0; i < w; i++) {
            int sx = x + i;
            if (sx >= 0 && sx < k->w) memcpy(o + ((size_t)j * w + i) * 4, k->px + ((size_t)sy * k->w + sx) * 4, 4);
        }
    }
    return o;
}
void cnv_put_rgba(int id, const uint8_t *s, int w, int h, int dx, int dy) {
    struct cnv *k = cnv_get(id);
    if (!k) return;
    for (int j = 0; j < h; j++) {
        int y = dy + j;
        if (y < 0 || y >= k->h) continue;
        for (int i = 0; i < w; i++) {
            int x = dx + i;
            if (x >= 0 && x < k->w) memcpy(k->px + ((size_t)y * k->w + x) * 4, s + ((size_t)j * w + i) * 4, 4);
        }
    }
}

/* "data:image/png;base64,..." (malloc'd), or NULL */
char *cnv_png_url(int id) {
    struct cnv *k = cnv_get(id);
    if (!k) return 0;
    uint32_t *px = malloc(sizeof *px * k->w * k->h);
    if (!px) return 0;
    for (long i = 0; i < (long)k->w * k->h; i++) { const uint8_t *q = k->px + i * 4; px[i] = (uint32_t)q[3] << 24 | q[0] << 16 | q[1] << 8 | q[2]; }
    size_t n;
    uint8_t *png = image_png_encode(px, k->w, k->h, &n);
    free(px);
    if (!png) return 0;
    static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *pre = "data:image/png;base64,";
    size_t pl = strlen(pre);
    char *o = malloc(pl + (n + 2) / 3 * 4 + 1);
    if (!o) { free(png); return 0; }
    memcpy(o, pre, pl);
    char *q = o + pl;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = png[i] << 16 | (i + 1 < n ? png[i + 1] << 8 : 0) | (i + 2 < n ? png[i + 2] : 0);
        *q++ = B[v >> 18]; *q++ = B[(v >> 12) & 63]; *q++ = i + 1 < n ? B[(v >> 6) & 63] : '='; *q++ = i + 2 < n ? B[v & 63] : '=';
    }
    *q = 0;
    free(png);
    return o;
}

/* the RGBA pixels of canvas id (valid until the next resize/reset), for display */
const uint8_t *cnv_pixels(int id, int *w, int *h) {
    struct cnv *k = cnv_get(id);
    if (!k) return 0;
    *w = k->w; *h = k->h;
    return k->px;
}
