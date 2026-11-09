/* html.c - the display list's queries and drawing: hit testing, text selection, find, doc_draw. The layout that fills
 * struct doc is layout.c (boxes) over dom.c (tree). Items are in paint order; y is not monotonic (table cells, floats,
 * positioned boxes), see first_item. */
#include "browser.h"
#include "layout.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

const struct font *const FT[] = { &font_ui, &font_ui_bold, &font_title, &font_mono, &font_mono_bold };

void doc_free(struct doc *d) {
    for (int i = 0; i < d->nlink; i++) free(d->link[i]);
    free(d->pmax); free(d->smin); free(d->it); free(d->dc); free(d->txt); free(d->link); free(d->an); free(d->ct); free(d->op); free(d->fm); free(d->cl);
    memset(d, 0, sizeof *d);
}

static int in_clip(const struct doc *d, const struct item *it, int x, int y) {
    if (!it->clip) return 1;
    const struct cliprect *k = &d->cl[it->clip];
    return x >= k->x && x < k->x + k->w && y >= k->y && y < k->y + k->h;
}

/* ---------- drawing and hit testing ---------- */
/* y is not monotonic (table cells in one row are laid out one after the other): the first item whose bottom edge reaches
 * ytop is found on the prefix maximum, and a scan can stop once every later item starts below its limit (suffix minimum) */
static int first_item(const struct doc *d, int ytop) {
    int lo = 0, hi = d->nit;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->pmax[mid] < ytop) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* ---------- text helpers: item/char positions, find, copy ---------- */
static int is_text(const struct item *it) { return !(it->flags & (FL_IMG | FL_CTL)) && it->len; }

/* x offset of byte `ch` inside a text run */
static int run_x(const struct doc *d, const struct item *it, int ch) { return gfx_text_width_n(FT[it->font], d->txt + it->off, ch); }

static int run_char_at(const struct doc *d, const struct item *it, int x) {      /* byte offset nearest to x (relative to the run) */
    const char *t = d->txt + it->off;
    const struct font *f = FT[it->font];
    int k = 0, acc = 0;
    while (k < it->len) {
        uint32_t cp;
        int l = utf8_decode(t + k, it->len - k, &cp), cw = gfx_text_width_n(f, t + k, l);
        if (x < acc + cw / 2) break;
        acc += cw; k += l;
    }
    return k;
}

int doc_pos_at(const struct doc *d, int x, int y, int *item, int *ch) {
    int best = -1, bd = 1 << 30;
    for (int i = first_item(d, y - 40); i < d->nit && d->smin[i] <= y + 40; i++) {
        const struct item *it = &d->it[i];
        if (!is_text(it) || it->y > y + 40) continue;
        int dy = y < it->y ? it->y - y : y >= it->y + it->h ? y - (it->y + it->h) + 1 : 0;
        int dx = x < it->x ? it->x - x : x >= it->x + it->w ? x - (it->x + it->w) + 1 : 0;
        int sc = dy * 4096 + dx;
        if (sc < bd) { bd = sc; best = i; }
    }
    if (best < 0) return 0;
    const struct item *it = &d->it[best];
    *item = best;
    *ch = x <= it->x ? 0 : x >= it->x + it->w ? it->len : run_char_at(d, it, x - it->x);
    return 1;
}

int doc_ctl_at(const struct doc *d, int x, int y) {
    for (int i = first_item(d, y); i < d->nit && d->smin[i] <= y; i++) {
        const struct item *it = &d->it[i];
        if ((it->flags & FL_CTL) && in_clip(d, it, x, y) && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->ref;
    }
    return -1;
}

char *doc_text(const struct doc *d, int a_it, int a_ch, int b_it, int b_ch) {
    size_t cap = 256, n = 0;
    char *o = malloc(cap);
    if (!o) return 0;
    for (int i = a_it; i <= b_it && i < d->nit; i++) {
        const struct item *it = &d->it[i];
        if (!is_text(it)) continue;
        int s = i == a_it ? a_ch : 0, e = i == b_it ? b_ch : it->len;
        if (e <= s) continue;
        if (n + (size_t)(e - s) + 3 > cap) {
            while (n + (size_t)(e - s) + 3 > cap) cap *= 2;
            char *no = realloc(o, cap);
            if (!no) { free(o); return 0; }
            o = no;
        }
        if (n) {                                          /* newline when the run starts a new line */
            const struct item *pv = 0;
            for (int j = i - 1; j >= a_it && !pv; j--) if (is_text(&d->it[j])) pv = &d->it[j];
            if (pv && it->x <= pv->x) { o[n++] = '\n'; if (it->y - (pv->y + pv->h) > 8) o[n++] = '\n'; }
            else if (pv && it->x > pv->x + pv->w) o[n++] = ' ';
        }
        memcpy(o + n, d->txt + it->off + s, e - s);
        n += e - s;
    }
    o[n] = 0;
    return o;
}

static int ci_at(const char *h, int hl, int pos, const char *nd, int nl) {
    if (pos + nl > hl) return 0;
    for (int k = 0; k < nl; k++) if (tolower((unsigned char)h[pos + k]) != tolower((unsigned char)nd[k])) return 0;
    return 1;
}

/* next match at/after (from_it, from_off) going `dir` (+1/-1), wrapping once */
int doc_find(const struct doc *d, const char *nd, int from_it, int from_off, int dir, int *oit, int *ooff) {
    int nl = strlen(nd);
    if (!nl || !d->nit) return 0;
    for (int step = 0; step <= d->nit; step++) {
        int i = ((from_it + dir * step) % d->nit + d->nit) % d->nit;
        const struct item *it = &d->it[i];
        if (!is_text(it)) continue;
        const char *h = d->txt + it->off;
        int start = step == 0 ? from_off : dir > 0 ? 0 : it->len - nl;
        if (dir > 0) { for (int k = start; k + nl <= it->len; k++) if (ci_at(h, it->len, k, nd, nl)) { *oit = i; *ooff = k; return 1; } }
        else { for (int k = start > it->len - nl ? it->len - nl : start; k >= 0; k--) if (ci_at(h, it->len, k, nd, nl)) { *oit = i; *ooff = k; return 1; } }
    }
    return 0;
}

int doc_count(const struct doc *d, const char *nd) {
    int nl = strlen(nd), c = 0;
    if (!nl) return 0;
    for (int i = 0; i < d->nit; i++) {
        const struct item *it = &d->it[i];
        if (!is_text(it)) continue;
        for (int k = 0; k + nl <= it->len; k++) if (ci_at(d->txt + it->off, it->len, k, nd, nl)) { c++; k += nl - 1; }
    }
    return c;
}

/* ---------- drawing ---------- */
static void draw_box(struct canvas *cc, const struct doc *d, const struct item *it, int x, int y) {
    int nw, nh;
    if (it->fit && img_info(it->ref, &nw, &nh) == IMG_READY && nw > 0 && nh > 0) {   /* object-fit: contain, cover, none, scale-down (fill = the plain path) */
        long W = it->w, H = it->h, tw = nw, th = nh;
        int wide = W * nh > H * nw;                      /* the box is wider than the picture */
        int contain = it->fit == 1 || (it->fit == 4 && (nw > W || nh > H));
        if (contain || it->fit == 2) {
            int by_h = it->fit == 2 ? !wide : wide;
            tw = by_h ? H * nw / nh : W; th = by_h ? H : W * nh / nw;
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        const uint32_t *p2 = img_pixels(it->ref, tw, th, d->bg);
        if (p2) {
            struct canvas c2 = *cc;
            canvas_clip(&c2, x, y, it->w, it->h);
            gfx_blit(&c2, x + (it->w - tw) / 2, y + (it->h - th) / 2, p2, tw, th, tw);
            return;
        }
    }
    const uint32_t *px = img_pixels(it->ref, it->w, it->h, d->bg);
    if (px) { gfx_blit(cc, x, y, px, it->w, it->h, it->w); return; }
    int w, h, st = img_info(it->ref, &w, &h);           /* not ready: an outline with the alt text */
    color_t dim = mix(d->fg, d->bg, 130);
    gfx_fill(cc, x, y, it->w, it->h, mix(d->bg, d->fg, 14));
    gfx_rect(cc, x, y, it->w, it->h, st == IMG_FAILED ? RGB(0xC0, 0x50, 0x50) : mix(d->bg, d->fg, 80));
    if (it->w > 30 && it->h > 18) {
        char t[210];
        snprintf(t, sizeof t, "%s%.*s", st == IMG_FAILED ? "\xC3\x97 " : "", it->len, d->txt + it->off);
        if (st != IMG_FAILED && !it->len) strcpy(t, "...");
        struct canvas c2 = *cc;
        canvas_clip(&c2, x + 3, y, it->w - 6, it->h);
        gfx_text(&c2, &font_ui, x + 5, y + 4, t, dim);
    }
}

static color_t mix_a(color_t a, color_t b, int t) {   /* a -> b, t 0..255, alpha included (a transparent end takes the other's colour) */
    if (!(a >> 24)) a = b & 0xFFFFFF; else if (!(b >> 24)) b = a & 0xFFFFFF;
    uint32_t r = 0;
    for (int s = 0; s <= 24; s += 8) r |= (uint32_t)((((a >> s) & 255) * (255 - t) + ((b >> s) & 255) * t + 127) / 255) << s;
    return r;
}

static int stick_shift(const struct stick *g, int scroll) {
    int s = scroll + g->top - g->y0, mx = g->lim - (g->y0 + g->h);
    if (s <= 0) return 0;
    return s < mx ? s : mx > 0 ? mx : 0;
}

/* is item (decor = 0) or decor (decor = 1) index n inside a sticky group that is currently shifted? */
static int stick_in(const struct doc *d, const int *shs, int n, int decor) {
    for (int g = 0; g < d->nsk; g++) if (shs[g] && (decor ? n >= d->sk[g].d0 && n < d->sk[g].d1 : n >= d->sk[g].i0 && n < d->sk[g].i1)) return 1;
    return 0;
}

/* sh: the sticky shift of the group the thing belongs to (clip rects created inside the group move with it: [c0, c1)) */
static void draw_decor(struct canvas *cc, const struct doc *d, const struct decor *r, int vx, int vy, int vh, int scroll, int sh, int c0, int c1) {
    int y = vy + r->y - scroll + sh;
    if (y >= vy + vh || y + r->h <= vy) return;
    struct canvas c2 = *cc, *c = cc;
    if (r->clip) { const struct cliprect *k = &d->cl[r->clip]; c2 = *cc; canvas_clip(&c2, vx + k->x, vy + k->y - scroll + (r->clip >= c0 && r->clip < c1 ? sh : 0), k->w, k->h); c = &c2; }
    if (r->img) {                                        /* background-image: sized, positioned, tiled, clipped to the box */
        int iw, ih;
        struct canvas c3 = *c;
        canvas_clip(&c3, vx + r->x, y, r->w, r->h);
        if (img_info(r->img - 1, &iw, &ih) != IMG_READY || iw <= 0 || ih <= 0) { img_pixels(r->img - 1, 1, 1, d->bg); return; }   /* (the call marks it visible so it loads) */
        long W = r->w, H = r->h, tw = iw, th = ih;
        if (r->kw == K_COVER || r->kw == K_CONTAIN) {
            int wide = W * ih > H * iw;                  /* box is wider than the picture's aspect */
            int by_w = r->kw == K_COVER ? wide : !wide;
            tw = by_w ? W : H * iw / ih; th = by_w ? W * ih / iw : H;
        } else {
            if (r->kw == K_PX) tw = r->sw; else if (r->kw == K_PCT) tw = W * r->sw / 10000;
            if (r->kh == K_PX) th = r->sh; else if (r->kh == K_PCT) th = H * r->sh / 10000;
            if (r->kw != K_PX && r->kw != K_PCT && (r->kh == K_PX || r->kh == K_PCT)) tw = th * iw / ih;
            if (r->kh != K_PX && r->kh != K_PCT && (r->kw == K_PX || r->kw == K_PCT)) th = tw * ih / iw;
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        if (tw > 4096 || th > 4096) return;
        long ox = r->kx == K_PCT ? (W - tw) * r->px / 10000 : r->px, oy = r->ky == K_PCT ? (H - th) * r->py / 10000 : r->py;
        const uint32_t *px = img_pixels(r->img - 1, (int)tw, (int)th, (r->col >> 24) == 0xFF ? r->col : d->bg);
        if (!px) return;
        int rx = r->rep == REP_REPEAT || r->rep == REP_X, ry = r->rep == REP_REPEAT || r->rep == REP_Y;
        long x0 = rx ? ox - ((ox + tw - 1) / tw) * tw : ox, y0 = ry ? oy - ((oy + th - 1) / th) * th : oy, n = 0;
        if (x0 > 0 && rx) x0 -= ((x0 + tw - 1) / tw) * tw;
        if (y0 > 0 && ry) y0 -= ((y0 + th - 1) / th) * th;
        for (long yy = y0; yy < H && n < 1500; yy += th) {
            for (long xx = x0; xx < W && n < 1500; xx += tw, n++) {
                gfx_blit_alpha(&c3, vx + r->x + (int)xx, y + (int)yy, px, (int)tw, (int)th, (int)tw);
                if (!rx) break;
            }
            if (!ry) break;
        }
        return;
    }
    if (r->gdir) {                                       /* linear gradient: one fill per row (or column) */
        int vert = r->gdir == 1, len = vert ? r->h : r->w, a0 = 0, a1 = len;
        if (vert) { if (y < vy) a0 = vy - y; if (y + r->h > vy + vh) a1 = vy + vh - y; }
        for (int i = a0; i < a1;) {
            int j = i + 1;
            color_t cc2 = mix_a(r->col, r->g1, (2 * i + 1) * 255 / (2 * len));
            while (j < a1 && mix_a(r->col, r->g1, (2 * j + 1) * 255 / (2 * len)) == cc2) j++;
            if (vert) gfx_fill(c, vx + r->x, y + i, r->w, j - i, cc2); else gfx_fill(c, vx + r->x + i, y, j - i, r->h, cc2);
            i = j;
        }
        return;
    }
    if (r->ring) gfx_rrect_ring(c, vx + r->x, y, r->w, r->h, RADII(r->rad, r->rad, r->rad, r->rad), r->ring, r->col);
    else if (r->rad) gfx_round_fill(c, vx + r->x, y, r->w, r->h, r->rad, r->col);
    else gfx_fill(c, vx + r->x, y, r->w, r->h, r->col);
}

/* one text/image/control item; sh/c0/c1 as in draw_decor */
static void draw_item(struct canvas *cc, const struct doc *d, int i, int vx, int vy, int vh, int scroll, int sh, int c0, int c1,
                      int hover, const struct docmark *mk, int focus_ord, int caret, int nl) {
    const struct item *it = &d->it[i];
    int y = vy + it->y - scroll + sh;
    if (y >= vy + vh || y + it->h < vy) return;
    struct canvas c2 = *cc, *cv = cc;
    if (it->clip) { const struct cliprect *k = &d->cl[it->clip]; canvas_clip(&c2, vx + k->x, vy + k->y - scroll + (it->clip >= c0 && it->clip < c1 ? sh : 0), k->w, k->h); cv = &c2; }
    if (it->flags & FL_IMG) { draw_box(cv, d, it, vx + it->x, y); return; }
    if (it->flags & FL_CTL) {
        const struct ctl *t = &d->ct[it->ref];
        ctl_draw(cv, d, t, vx + it->x, y, it->w, it->h, t->ord == focus_ord, caret);
        return;
    }
    const struct font *f = FT[it->font];
    const char *txt = d->txt + it->off;
    if (mk && mk->from_it >= 0 && i >= mk->from_it && i <= mk->to_it) {      /* selection */
        int s = i == mk->from_it ? mk->from_ch : 0, e = i == mk->to_it ? mk->to_ch : it->len;
        if (e > s) { int x0 = run_x(d, it, s), x1 = run_x(d, it, e); gfx_fill(cv, vx + it->x + x0, y, x1 - x0 + (e == it->len ? 3 : 0), it->h, RGBA(0x4C, 0x8D, 0xFF, 120)); }
    }
    for (int k = 0; nl && k + nl <= it->len; k++) {                          /* find matches */
        if (!ci_at(txt, it->len, k, mk->find, nl)) continue;
        int x0 = run_x(d, it, k), x1 = run_x(d, it, k + nl), act = i == mk->find_it && k == mk->find_off;
        gfx_fill(cv, vx + it->x + x0, y, x1 - x0, it->h, act ? RGBA(0xFF, 0x96, 0x1E, 200) : RGBA(0xFF, 0xE0, 0x3C, 150));
        k += nl - 1;
    }
    color_t col = it->link >= 0 && it->link == hover ? d->hoverc : it->col;
    gfx_text_n(cv, f, vx + it->x, y, txt, it->len, col);
    if (it->flags & FL_UL) gfx_fill(cv, vx + it->x, y + f->ascent + 2, it->w, 1, col);
    if (it->flags & FL_STRIKE) gfx_fill(cv, vx + it->x, y + f->height * 55 / 100, it->w, 1, col);
}

void doc_draw(struct ui *ui, const struct doc *d, int vx, int vy, int vw, int vh, int scroll, int hover,
              const struct docmark *mk, int focus_ord, int caret) {
    struct canvas cc = *ui->c;
    canvas_clip(&cc, vx, vy, vw, vh);
    gfx_fill(&cc, vx, vy, vw, vh, d->bg);
    int nl = mk && mk->find ? strlen(mk->find) : 0, di = 0, start = first_item(d, scroll), shifted = 0, shs[MAXSTK];
    for (int g = 0; g < d->nsk; g++) { shs[g] = stick_shift(&d->sk[g], scroll); shifted |= shs[g] != 0; }
    for (; di < d->ndc && d->dc[di].it <= start; di++) if (!shifted || !stick_in(d, shs, di, 1)) draw_decor(&cc, d, &d->dc[di], vx, vy, vh, scroll, 0, 0, 0);   /* backgrounds under the first visible item */
    for (int i = start; i < d->nit; i++) {
        const struct item *it = &d->it[i];
        int y = vy + it->y - scroll;
        if (vy + d->smin[i] - scroll >= vy + vh) break;
        if (y >= vy + vh || y + it->h < vy) continue;
        for (; di < d->ndc && d->dc[di].it <= i; di++) if (!shifted || !stick_in(d, shs, di, 1)) draw_decor(&cc, d, &d->dc[di], vx, vy, vh, scroll, 0, 0, 0);
        if (shifted && stick_in(d, shs, i, 0)) continue;                         /* drawn below, moved */
        draw_item(&cc, d, i, vx, vy, vh, scroll, 0, 0, 0, hover, mk, focus_ord, caret, nl);
    }
    for (; di < d->ndc; di++) if (!shifted || !stick_in(d, shs, di, 1)) draw_decor(&cc, d, &d->dc[di], vx, vy, vh, scroll, 0, 0, 0);
    for (int g = 0; g < d->nsk; g++) {                                           /* sticky boxes last: on top, at their stuck position */
        const struct stick *k = &d->sk[g];
        if (!shs[g]) continue;
        int di2 = k->d0;
        for (int i = k->i0; i < k->i1; i++) {
            for (; di2 < k->d1 && d->dc[di2].it <= i; di2++) draw_decor(&cc, d, &d->dc[di2], vx, vy, vh, scroll, shs[g], k->c0, k->c1);
            draw_item(&cc, d, i, vx, vy, vh, scroll, shs[g], k->c0, k->c1, hover, mk, focus_ord, caret, nl);
        }
        for (; di2 < k->d1; di2++) draw_decor(&cc, d, &d->dc[di2], vx, vy, vh, scroll, shs[g], k->c0, k->c1);
    }
}

int doc_unstick(const struct doc *d, int y, int scroll) {
    for (int g = 0; g < d->nsk; g++) {
        int s = stick_shift(&d->sk[g], scroll);
        if (s && y >= d->sk[g].y0 + s && y < d->sk[g].y0 + s + d->sk[g].h) return y - s;
    }
    return y;
}

int doc_hit(const struct doc *d, int x, int y) {
    for (int i = first_item(d, y); i < d->nit && d->smin[i] <= y; i++) {
        const struct item *it = &d->it[i];
        if (it->link >= 0 && in_clip(d, it, x, y) && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->link;
    }
    return -1;
}

int doc_node_at(const struct doc *d, int x, int y) {
    int node = 0;
    for (int i = first_item(d, y); i < d->nit && d->smin[i] <= y; i++) {
        const struct item *it = &d->it[i];
        if (it->node && in_clip(d, it, x, y) && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) node = it->node;
    }
    return node;
}

int doc_anchor(const struct doc *d, const char *name) {
    for (int i = 0; i < d->nan; i++) if (!strcmp(d->an[i].name, name)) return d->an[i].y;
    return -1;
}
