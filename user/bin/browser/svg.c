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
    char *t = malloc(n + 32);
    if (!t) return -1;
    size_t o = 0;
    const char *sv = svg_mem((const char *)d, n, "<svg", 4), *sg = sv ? memchr(sv, '>', n - (sv - (const char *)d)) : 0;
    size_t hdr = 0;
    if (cur >= 0 && sg) {                           /* inline icons: page CSS (fill:currentColor / color) is not visible to nanosvg, so an svg root without its own fill takes the text colour */
        char *tmp = malloc(sg - sv + 1);
        if (tmp) { memcpy(tmp, sv, sg - sv); tmp[sg - sv] = 0; if (!strstr(tmp, " fill=")) hdr = (sv - (const char *)d) + 4; free(tmp); }
    }
    if (hdr) { memcpy(t, d, hdr); o = hdr; o += snprintf(t + o, 20, " fill=\"#%06X\"", (unsigned)cur); }
    for (size_t i = hdr; i < n; ) {                  /* nanosvg has no currentColor: substitute it (7 chars <= 12, so the copy never grows) */
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

/* ---- <use> resolution ---- */
const char *svg_mem(const char *h, size_t hn, const char *nd, size_t nn) {
    for (; hn >= nn; h++, hn--) if (!memcmp(h, nd, nn)) return h;
    return 0;
}

/* attr value of name="..." inside the tag text [t, t+n); copies into out (cap), returns 1 if found */
static int tag_attr(const char *t, size_t n, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (size_t i = 1; i + nl + 2 < n; i++) {
        if (strncasecmp(t + i, name, nl) || t[i + nl] != '=' || !strchr(" \t\r\n\"'", t[i - 1])) continue;
        char q = t[i + nl + 1];
        if (q != '"' && q != '\'') continue;
        size_t s = i + nl + 2, e = s;
        while (e < n && t[e] != q) e++;
        snprintf(out, cap, "%.*s", (int)(e - s), t + s);
        return 1;
    }
    return 0;
}

/* find the element with id=`id` in doc: sets *s,*e to its whole text; for <symbol> *s,*e is the inner markup and vb gets its viewBox */
static int find_id(const char *doc, size_t dn, const char *id, const char **s, const char **e, char *vb, size_t vbcap) {
    char pat[96];
    size_t pl = snprintf(pat, sizeof pat, "id=\"%s\"", id);
    const char *end = doc + dn;
    for (const char *p = doc; (p = svg_mem(p, end - p, pat, pl)); p += pl) {
        const char *lt = p;
        while (lt > doc && *lt != '<') lt--;
        const char *gt = memchr(p, '>', end - p);
        if (*lt != '<' || !gt || lt[1] == '/' || !strchr(" \t\r\n", p[-1])) continue;
        char nm[16] = "";
        sscanf(lt + 1, "%15[A-Za-z]", nm);
        int self = gt[-1] == '/';
        const char *cl = 0;
        if (!self) {                                    /* matching close tag (same-name nesting not handled: symbol/path/g are flat enough) */
            char close[24];
            snprintf(close, sizeof close, "</%s>", nm);
            cl = svg_mem(gt, end - gt, close, strlen(close));
            if (!cl) continue;
        }
        if (!strcasecmp(nm, "symbol")) {
            if (vb) { vb[0] = 0; tag_attr(lt, gt - lt, "viewBox", vb, vbcap); }
            *s = gt + 1; *e = cl ? cl : gt + 1;
        } else { *s = lt; *e = cl ? cl + strlen(nm) + 3 : gt + 1; }
        return 1;
    }
    return 0;
}

/* Expand <use href="#id"> (or xlink:href) into a <g> with the referenced element/symbol's markup, looked up in svg itself and in the
 * other inline svgs of the page (docs[i], dl[i]: sprite sheets). A symbol's viewBox is given to the outer <svg> when it has none.
 * Returns a malloc'd string, or NULL when there is nothing to expand. */
char *svg_resolve_use(const char *svg, size_t n, const char *const *docs, const size_t *dl, int nd, size_t *on) {
    if (!svg_mem(svg, n, "<use", 4)) return 0;
    size_t cap = n + 4096, o = 0;
    char *out = malloc(cap);
    char vbuf[96] = "";
    int any = 0;
    if (!out) return 0;
    for (size_t i = 0; i < n; ) {
        if (n - i > 5 && !strncmp(svg + i, "<use", 4) && strchr(" \t\r\n/", svg[i + 4])) {
            const char *gt = memchr(svg + i, '>', n - i);
            if (gt) {
                size_t tl = gt + 1 - (svg + i);
                char ref[80] = "";
                if (!tag_attr(svg + i, tl, "href", ref, sizeof ref)) tag_attr(svg + i, tl, "xlink:href", ref, sizeof ref);
                const char *s = 0, *e = 0;
                char vb[96] = "";
                char *hash = strrchr(ref, '#');
                if (hash) {
                    find_id(svg, n, hash + 1, &s, &e, vb, sizeof vb);
                    for (int k = 0; k < nd && !s; k++) find_id(docs[k], dl[k], hash + 1, &s, &e, vb, sizeof vb);
                }
                if (s) {
                    size_t need = o + tl + (e - s) + 16;
                    if (need > cap) { cap = need + 4096; char *nb = realloc(out, cap); if (!nb) { free(out); return 0; } out = nb; }
                    if (!vbuf[0]) snprintf(vbuf, sizeof vbuf, "%s", vb);
                    size_t al = tl - 5 - (svg[i + tl - 2] == '/');      /* the attributes between "<use" and ">" or "/>" */
                    o += snprintf(out + o, cap - o, "<g%.*s>", (int)al, svg + i + 4);
                    memcpy(out + o, s, e - s); o += e - s;
                    memcpy(out + o, "</g>", 4); o += 4;
                    i += tl; any = 1;
                    if (!strncmp(svg + i, "</use>", 6)) i += 6;
                    continue;
                }
            }
        }
        if (o + 2 > cap) { cap += 4096; char *nb = realloc(out, cap); if (!nb) { free(out); return 0; } out = nb; }
        out[o++] = svg[i++];
    }
    out[o] = 0;
    if (!any) { free(out); return 0; }
    if (vbuf[0] && !strstr(out, "viewBox")) {           /* the outer tag has no viewBox: use the symbol's */
        char *gt = strchr(out, '>');
        if (gt && o + 120 < cap) {
            char ins[128];
            int il = snprintf(ins, sizeof ins, " viewBox=\"%s\"", vbuf);
            memmove(gt + il, gt, out + o + 1 - gt);
            memcpy(gt, ins, il);
            o += il;
        }
    }
    *on = o;
    return out;
}
