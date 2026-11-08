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

static void draw_decor(struct canvas *cc, const struct doc *d, const struct decor *r, int vx, int vy, int vh, int scroll) {
    int y = vy + r->y - scroll;
    if (y >= vy + vh || y + r->h <= vy) return;
    struct canvas c2 = *cc, *c = cc;
    if (r->clip) { const struct cliprect *k = &d->cl[r->clip]; c2 = *cc; canvas_clip(&c2, vx + k->x, vy + k->y - scroll, k->w, k->h); c = &c2; }
    if (r->ring) gfx_rrect_ring(c, vx + r->x, y, r->w, r->h, RADII(r->rad, r->rad, r->rad, r->rad), r->ring, r->col);
    else if (r->rad) gfx_round_fill(c, vx + r->x, y, r->w, r->h, r->rad, r->col);
    else gfx_fill(c, vx + r->x, y, r->w, r->h, r->col);
}

void doc_draw(struct ui *ui, const struct doc *d, int vx, int vy, int vw, int vh, int scroll, int hover,
              const struct docmark *mk, int focus_ord, int caret) {
    struct canvas cc = *ui->c;
    canvas_clip(&cc, vx, vy, vw, vh);
    gfx_fill(&cc, vx, vy, vw, vh, d->bg);
    int nl = mk && mk->find ? strlen(mk->find) : 0, di = 0, start = first_item(d, scroll);
    for (; di < d->ndc && d->dc[di].it <= start; di++) draw_decor(&cc, d, &d->dc[di], vx, vy, vh, scroll);   /* backgrounds under the first visible item */
    for (int i = start; i < d->nit; i++) {
        const struct item *it = &d->it[i];
        int y = vy + it->y - scroll;
        if (vy + d->smin[i] - scroll >= vy + vh) break;
        if (y >= vy + vh || y + it->h < vy) continue;
        for (; di < d->ndc && d->dc[di].it <= i; di++) draw_decor(&cc, d, &d->dc[di], vx, vy, vh, scroll);
        struct canvas c2 = cc, *cv = &cc;
        if (it->clip) { const struct cliprect *k = &d->cl[it->clip]; canvas_clip(&c2, vx + k->x, vy + k->y - scroll, k->w, k->h); cv = &c2; }
        if (it->flags & FL_IMG) { draw_box(cv, d, it, vx + it->x, y); continue; }
        if (it->flags & FL_CTL) {
            const struct ctl *t = &d->ct[it->ref];
            ctl_draw(cv, d, t, vx + it->x, y, it->w, it->h, t->ord == focus_ord, caret);
            continue;
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
    for (; di < d->ndc; di++) draw_decor(&cc, d, &d->dc[di], vx, vy, vh, scroll);
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
