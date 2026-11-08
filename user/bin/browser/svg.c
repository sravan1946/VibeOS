/* svg.c - SVG pictures for the browser: nanosvg (third_party/nanosvg, the one the icon pipeline uses at build time) parses the
 * text and rasterises it. An SVG has no pixel size of its own, so it is drawn at about SVG_MIN px on its long side (or at its natural
 * size when bigger) and img.c scales that down for display: small icons stay crisp at any size the page asks for. */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#define acosf(x) ((float)acos(x))
#define tanf(x) ((float)tan(x))
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#define SVG_MIN 256
#define SVG_MAX 1200

int svg_is(const uint8_t *d, size_t n) {          /* "<svg" within the first 1 KiB (after any XML prolog, comments, doctype) */
    for (size_t i = 0; i + 4 <= n && i < 1024; i++)
        if (d[i] == '<' && !strncasecmp((const char *)d + i, "<svg", 4)) return 1;
    return 0;
}

/* im: the pixels (0xAARRGGBB, not premultiplied); *nw, *nh: the natural size in CSS px. currentColor becomes `cur` (0xRRGGBB, or -1 = black). */
int svg_decode(const uint8_t *d, size_t n, struct image *im, int *nw, int *nh, long cur) {
    char *t = malloc(n + 16);
    if (!t) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; ) {                    /* nanosvg has no currentColor: substitute it (7 chars <= 12, so the copy never grows) */
        if (n - i >= 12 && !strncasecmp((const char *)d + i, "currentColor", 12)) {
            o += snprintf(t + o, 8, "#%06X", (unsigned)(cur < 0 ? 0 : cur));
            i += 12;
        } else t[o++] = d[i++];
    }
    t[o] = 0;
    NSVGimage *g = nsvgParse(t, "px", 96);
    free(t);
    if (!g) return -1;
    float w = g->width, h = g->height;
    if (!(w >= 1)) w = 300;
    if (!(h >= 1)) h = 150;
    float s = 1, big = w > h ? w : h;
    if (big < SVG_MIN) s = SVG_MIN / big;
    if (big * s > SVG_MAX) s = SVG_MAX / big;
    int iw = (int)ceilf(w * s), ih = (int)ceilf(h * s);
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    unsigned char *rgba = calloc((size_t)iw * ih, 4);
    uint32_t *px = malloc((size_t)iw * ih * 4);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if (!rgba || !px || !r) { free(rgba); free(px); if (r) nsvgDeleteRasterizer(r); nsvgDelete(g); return -1; }
    nsvgRasterize(r, g, 0, 0, s, rgba, iw, ih, iw * 4);
    for (long i = 0, m = (long)iw * ih; i < m; i++) {
        const unsigned char *c = rgba + i * 4;
        px[i] = (uint32_t)c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
    }
    free(rgba);
    nsvgDeleteRasterizer(r);
    nsvgDelete(g);
    im->w = iw; im->h = ih; im->px = px;
    *nw = (int)ceilf(w); *nh = (int)ceilf(h);
    return 0;
}
