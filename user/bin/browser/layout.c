/* layout.c - the page engine: style -> box tree -> layout -> display list.
 *   build   (bottom of file): the HD (dom.c) + css.c's computed style become a tree of `struct bx` (anonymous blocks and
 *           table parts are added so every container holds either blocks or inline content);
 *   layout  : block and inline formatting contexts, floats, abs/fixed positioning, flexbox, tables, simple grid. All
 *           geometry is in document coordinates, so a laid-out subtree moves with shift();
 *   paint   : walks the tree in stacking order and appends decors/items (struct doc), with clip rects for overflow.
 * min/max-content widths (intr) are computed lazily and cached on the box; tables, floats, inline-blocks, flex items and
 * abs boxes shrink-to-fit with them.
 * ponytail: no bidi/justify/columns/grid-areas, baseline alignment in flex = start, percentage heights only against a
 * definite parent height, inline boxes don't split around block children (they become blocks). */
#include "browser.h"
#include "layout.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#ifdef LT_DEBUG
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#endif
#define LEAD 3                    /* extra line gap under every line (the bitmap fonts have none) */
#define MAXFL 64
#define BIGW 100000
#define VIEWH 600                 /* assumed viewport height for position:fixed / bottom:0 */

enum { OUT_INLINE, OUT_BLOCK };
enum { IN_FLOW, IN_INLINE, IN_FLEX, IN_GRID, IN_TABLE, IN_TGROUP, IN_TROW, IN_TCELL, IN_CAP, IN_TEXT, IN_IMG, IN_CTL, IN_BR };
#define POS_REL POS_RELATIVE
#define POS_ABS POS_ABSOLUTE
enum { L_MT, L_MR, L_MB, L_ML, L_PT, L_PR, L_PB, L_PL, L_TOP, L_RIGHT, L_BOTTOM, L_LEFT, L_W, L_H, L_MINW, L_MAXW, L_MINH, L_MAXH, L_BASIS, NL };
enum { F_TEXTRUN, F_BG, F_BOX };

struct frag {                     /* one painted piece of a line: a text run, an inline box's background, an atomic box */
    int x, y, w, h;
    uint8_t t, font, flags, edges;/* edges: bit0 left border, bit1 right border (inline bg) */
    color_t col;
    int link, node;
    uint32_t off; int len;        /* text: doc arena */
    int box;                      /* F_BOX: box index; F_BG: the inline box */
};

struct bx {
    int par, first, last, next;
    uint8_t outer, inner, pos, flt, clr, ovf, vis, bsz, ws, align, valign, font, flags, nul, li, anon;
    uint8_t fdir, fwrap, just, ali, als, hasneg, zset, hdr;
    int order, grow, shrink, gapr, gapc, zi, gcols, grows, gareas, garea, g[4];
    uint8_t gk[4];
    color_t col, bg, bc[4];
    int bw[4], rad, lh, opacity;
    int link, node, dn;           /* dn: DOM node (debug dumps) */
    uint32_t aname;               /* anchor name: doc arena offset + 1 */
    int v[NL]; uint8_t k[NL];
    uint32_t toff; int tlen;      /* IN_TEXT: bytes in the HD pool */
    int ref, iw, ih, bel, cs, rs, mark;   /* img cache index / control index; intrinsic size; baseline below the box; colspan, rowspan; list marker (0 bullet, n number, -1 none) */
    uint8_t halign, valn, lt;
    int txp, typ;
    int anm, adur, adel, ait, adr;   /* CSS animation: keyframes index + 1, duration/delay ms, iterations (0 = infinite), direction */
    int aspect, txv, tyv, sh[4], outw, outo; color_t shc, outc, markc; uint8_t tt;   /* box-shadow x/y/blur/spread, outline */         /* aspect-ratio (w/h x1000), transform: translate() */         /* table cell: horizontal / vertical alignment */
    int grd;                       /* background-image: linear-gradient(...): css_str index + 1 */
    int bgi; uint8_t msk, brep, bkx, bky, bkw, bkh, objfit; int bpx, bpy, bsw, bsh;   /* background-image (img index + 1) and its repeat/position/size; object-fit for images */
    int x, y, w, h, base;         /* border box, document coordinates; base: baseline offset from the top (-1 = bottom edge) */
    int m[4], p[4];               /* used margins and paddings (t r b l) */
    int imin, imax; uint8_t iv;
    int fw;                       /* forced content width for the next layout (-1 none) */
    struct frag *fr; int nfr;
    int cb, sx, sy, absdone;
    struct tab *tab;
};
static struct bx *B;
static int nb, bcap;
static struct doc *D;

struct fl { int x, y, w, h; uint8_t side; };
struct bfc { struct fl f[MAXFL]; int n, ytop; };

static int bx_new(void) {
    if (nb == bcap) {
        int nc = bcap ? bcap * 2 : 2048;
        struct bx *p = realloc(B, nc * sizeof *p);
        if (!p) return -1;
        B = p; bcap = nc;
    }
    memset(&B[nb], 0, sizeof B[0]);
    B[nb].par = B[nb].first = B[nb].last = B[nb].next = -1;
    B[nb].fw = -1; B[nb].opacity = 255; B[nb].cb = -1; B[nb].base = -1; B[nb].mark = -1; B[nb].link = -1;
    return nb++;
}
static void bx_append(int p, int c) {
    B[c].par = p;
    if (B[p].last >= 0) B[B[p].last].next = c; else B[p].first = c;
    B[p].last = c;
}
static void bx_unlink(int c) {
    int p = B[c].par;
    if (p < 0) return;
    int q = B[p].first, prev = -1;
    while (q >= 0 && q != c) { prev = q; q = B[q].next; }
    if (q < 0) return;
    if (prev >= 0) B[prev].next = B[c].next; else B[p].first = B[c].next;
    if (B[p].last == c) B[p].last = prev;
    B[c].par = B[c].next = -1;
}

/* ---------- small helpers ---------- */
static int imax_(int a, int b) { return a > b ? a : b; }
static int imin_(int a, int b) { return a < b ? a : b; }
static int pctof(int v, int base) { return (int)((long long)v * base / 100); }
static int rl(const struct bx *b, int i, int base) { return b->k[i] == K_PCT ? pctof(b->v[i], base) : b->k[i] == K_AUTO ? 0 : b->v[i]; }
static int collapse(int a, int b) { return a >= 0 && b >= 0 ? imax_(a, b) : a < 0 && b < 0 ? imin_(a, b) : a + b; }
static int is_inline_atom(const struct bx *b) { return b->outer == OUT_INLINE && b->inner != IN_INLINE && b->inner != IN_TEXT && b->inner != IN_BR; }
static int oof(const struct bx *b) { return b->pos == POS_ABS || b->pos == POS_FIXED; }          /* out of flow: abs/fixed */
static int in_flow_block(const struct bx *b) { return b->outer == OUT_BLOCK && !b->flt && !oof(b); }
static int is_bfc(const struct bx *b) { return b->flt || oof(b) || b->ovf || b->outer == OUT_INLINE || b->inner == IN_FLEX || b->inner == IN_GRID || b->inner == IN_TABLE || b->inner == IN_TCELL || b->inner == IN_CAP || b->par < 0; }

static void resolve(struct bx *b, int cw) {
    for (int i = 0; i < 4; i++) {
        b->m[i] = rl(b, L_MT + i, cw);
        b->p[i] = imax_(0, rl(b, L_PT + i, cw));
    }
}
static int hedge(const struct bx *b) { return b->bw[1] + b->bw[3] + b->p[1] + b->p[3]; }
static int vedge(const struct bx *b) { return b->bw[0] + b->bw[2] + b->p[0] + b->p[2]; }

/* content-box size from a width/height-like length (L_W...), or -1 for auto */
static int csize(const struct bx *b, int i, int base, int horiz) {
    if (b->k[i] == K_AUTO) return -1;
    int v = rl(b, i, base);
    if (b->bsz) v -= horiz ? hedge(b) : vedge(b);
    return imax_(v, 0);
}
static int clampw(const struct bx *b, int w, int cw) {
    int mx = csize(b, L_MAXW, cw, 1), mn = csize(b, L_MINW, cw, 1);
    if (mx >= 0 && w > mx) w = mx;
    if (mn >= 0 && w < mn) w = mn;
    return w;
}
static int clamph(const struct bx *b, int h, int ch) {
    int mx = csize(b, L_MAXH, ch, 0), mn = csize(b, L_MINH, ch, 0);
    if (mx >= 0 && h > mx) h = mx;
    if (mn >= 0 && h < mn) h = mn;
    return h;
}

/* move a laid-out subtree */
static void shift(struct bx *b, int dx, int dy) {
    if (!dx && !dy) return;
    b->x += dx; b->y += dy; b->sx += dx; b->sy += dy;
    for (int i = 0; i < b->nfr; i++) { b->fr[i].x += dx; b->fr[i].y += dy; }
    for (int c = b->first; c >= 0; c = B[c].next) shift(&B[c], dx, dy);
}
static void shift_kids(struct bx *b, int dx, int dy) {
    for (int c = b->first; c >= 0; c = B[c].next) shift(&B[c], dx, dy);
    for (int i = 0; i < b->nfr; i++) { b->fr[i].x += dx; b->fr[i].y += dy; }
}

/* ---------- floats ---------- */
static void fl_edges(const struct bfc *f, int y0, int y1, int cx, int cw, int *lx, int *rx) {
    *lx = cx; *rx = cx + cw;
    if (!f) return;
    for (int i = 0; i < f->n; i++) {
        const struct fl *e = &f->f[i];
        if (e->y < y1 && e->y + imax_(e->h, 1) > y0) {
            if (e->side == 1) *lx = imax_(*lx, e->x + e->w); else *rx = imin_(*rx, e->x);
        }
    }
}
static int fl_next(const struct bfc *f, int y0, int y1) {      /* smallest bottom edge above y0 among floats overlapping [y0,y1) */
    int best = 1 << 30;
    for (int i = 0; f && i < f->n; i++) {
        const struct fl *e = &f->f[i];
        if (e->y < y1 && e->y + imax_(e->h, 1) > y0 && e->y + e->h < best) best = e->y + e->h;
    }
    return best == 1 << 30 ? y0 : best;
}
static int fl_clear(const struct bfc *f, int clr, int y) {    /* clr: 1 left, 2 right, 3 both */
    for (int i = 0; f && i < f->n; i++)
        if (f->f[i].side & clr) y = imax_(y, f->f[i].y + f->f[i].h);
    return y;
}

/* ---------- text ---------- */
static int tw(const struct bx *tb, const char *s, int n) { return gfx_text_width_n(FT[tb->font], s, n); }
static int spw(const struct bx *tb) { return gfx_text_width(FT[tb->font], " "); }

/* inline walker: feeds words, spaces, breaks and boxes of a block container's inline content to callbacks */
struct iw {
    void *u;
    void (*word)(void *u, struct bx *tb, const char *s, int n);
    void (*space)(void *u, struct bx *tb);
    void (*brk)(void *u, struct bx *b);
    void (*open)(void *u, struct bx *b);
    void (*close)(void *u, struct bx *b);
    void (*atom)(void *u, struct bx *b);
    void (*flt)(void *u, struct bx *b);
    void (*absb)(void *u, struct bx *b);
    int sp;                       /* last thing was a (collapsed) space or a line start */
    int col;                      /* pre: column, for tabs */
};

static void xform(const struct bx *tb, char *s, int n) {        /* text-transform on ASCII (the bitmap fonts' case mapping stops there) */
    if (!tb->tt) return;
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (tb->tt == 1 && c >= 'a' && c <= 'z') s[i] = c - 32;
        else if (tb->tt == 2 && c >= 'A' && c <= 'Z') s[i] = c + 32;
        else if (tb->tt == 3 && i == 0 && c >= 'a' && c <= 'z') s[i] = c - 32;
    }
}

static void inl_text(struct iw *w, struct bx *tb) {
    const char *s = hd_str(tb->toff);
    int n = tb->tlen, mode = tb->ws;
    char buf[512];
    int bn = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (mode == 2 || mode == 3) {
            if (c == '\n') { if (bn) { (xform(tb, buf, bn), w->word(w->u, tb, buf, bn)); bn = 0; } w->brk(w->u, tb); w->col = 0; continue; }
            if (c == '\t') { int k = 8 - w->col % 8; while (k-- > 0 && bn < 500) { buf[bn++] = ' '; w->col++; } continue; }
            if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) { c = ' '; i++; }
            if (bn >= 480 && (c & 0xC0) != 0x80) { (xform(tb, buf, bn), w->word(w->u, tb, buf, bn)); bn = 0; }
            buf[bn++] = c;
            if ((c & 0xC0) != 0x80) w->col++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
            if (c == '\n' && mode == 4) { if (bn) { (xform(tb, buf, bn), w->word(w->u, tb, buf, bn)); bn = 0; } w->brk(w->u, tb); w->sp = 1; continue; }
            if (bn) { (xform(tb, buf, bn), w->word(w->u, tb, buf, bn)); bn = 0; }
            if (!w->sp) { w->space(w->u, tb); w->sp = 1; }
            continue;
        }
        if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) { c = ' '; i++; }
        else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0x8B || (unsigned char)s[i + 2] == 0x8C || (unsigned char)s[i + 2] == 0x8D)) { i += 2; continue; }   /* zero-width space/joiners */
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xAD) { i++; continue; }   /* soft hyphen */
        else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0x8E || (unsigned char)s[i + 2] == 0x8F || ((unsigned char)s[i + 2] >= 0xAA && (unsigned char)s[i + 2] <= 0xAE))) { i += 2; continue; }
        else if (c == 0xEF && i + 2 < n && (unsigned char)s[i + 1] == 0xBB && (unsigned char)s[i + 2] == 0xBF) { i += 2; continue; }
        if (bn >= 480 && (c & 0xC0) != 0x80) { (xform(tb, buf, bn), w->word(w->u, tb, buf, bn)); bn = 0; }
        buf[bn++] = c;
        w->sp = 0;
    }
    if (bn) (xform(tb, buf, bn), w->word(w->u, tb, buf, bn));
}

static void inl_walk(struct iw *w, int p) {
    for (int c = B[p].first; c >= 0; c = B[c].next) {
        struct bx *b = &B[c];
        if (oof(b)) { w->absb(w->u, b); continue; }
        if (b->flt) { w->flt(w->u, b); continue; }
        switch (b->inner) {
        case IN_TEXT: inl_text(w, b); break;
        case IN_BR: w->brk(w->u, b); w->sp = 1; break;
        case IN_INLINE: w->open(w->u, b); inl_walk(w, c); w->close(w->u, b); break;
        default: w->atom(w->u, b); w->sp = 0;
        }
    }
}

/* ---------- intrinsic widths ---------- */
static void intr(struct bx *b, int *mn, int *mx);
static void grid_intr(struct bx *b, int *mn, int *mx);
static void tab_intr(struct bx *b, int *mn, int *mx);

static void outer_intr(struct bx *c, int *mn, int *mx) {     /* margin box of a child: min/max-content */
    resolve(c, 0);
    int e = c->m[1] + c->m[3] + hedge(c), a, z;
    int w = c->k[L_W] == K_PX ? csize(c, L_W, 0, 1) : -1;
    if (w >= 0) a = z = w;
    else intr(c, &a, &z);
    int xw = c->k[L_MAXW] == K_PX ? csize(c, L_MAXW, 0, 1) : -1, nw = c->k[L_MINW] == K_PX ? csize(c, L_MINW, 0, 1) : -1;
    if (xw >= 0) { if (a > xw) a = xw; if (z > xw) z = xw; }
    if (nw >= 0) { if (a < nw) a = nw; if (z < nw) z = nw; }
    *mn = a + e; *mx = z + e;
}

struct ii { int cur, chunk, mx, mn, pend, spw; };
static void ii_add(struct ii *s, int w, int wmin) {
    if (s->pend && s->cur > 0) { s->cur += s->spw; }
    s->pend = 0;
    s->cur += w; s->chunk += wmin;
    if (s->chunk > s->mn) s->mn = s->chunk;
}
static void ii_word(void *u, struct bx *tb, const char *s, int n) { int w = tw(tb, s, n); ii_add(u, w, w); }
static void ii_space(void *u, struct bx *tb) {
    struct ii *s = u;
    s->spw = spw(tb);
    if (tb->ws == 1 || tb->ws == 2 || tb->ws == 3) { s->chunk += s->spw; s->cur += s->cur ? s->spw : 0; return; }
    s->chunk = 0; s->pend = 1;
}
static void ii_brk(void *u, struct bx *b) { struct ii *s = u; if (s->cur > s->mx) s->mx = s->cur; s->cur = s->chunk = s->pend = 0; }
static void ii_open(void *u, struct bx *b) { resolve(b, 0); int e = b->m[3] + b->bw[3] + b->p[3]; ii_add(u, e, e); }
static void ii_close(void *u, struct bx *b) { int e = b->m[1] + b->bw[1] + b->p[1]; struct ii *s = u; s->cur += e; s->chunk += e; if (s->chunk > s->mn) s->mn = s->chunk; }
static void ii_atom(void *u, struct bx *b) { int a, z; outer_intr(b, &a, &z); ii_add(u, z, a); }
static void ii_flt(void *u, struct bx *b) { struct ii *s = u; int a, z; outer_intr(b, &a, &z); s->cur += z; if (a > s->mn) s->mn = a; }
static void ii_abs(void *u, struct bx *b) { }

static void intr_inline(struct bx *b, int *mn, int *mx) {
    struct ii s = { 0 };
    struct iw w = { .u = &s, .word = ii_word, .space = ii_space, .brk = ii_brk, .open = ii_open, .close = ii_close, .atom = ii_atom, .flt = ii_flt, .absb = ii_abs, .sp = 1 };
    inl_walk(&w, b - B);
    *mn = s.mn; *mx = imax_(s.mx, s.cur);
}

static int has_inline_content(const struct bx *b) {
    for (int c = b->first; c >= 0; c = B[c].next) if (!in_flow_block(&B[c]) && !B[c].flt && !oof(&B[c])) return 1;
    return 0;
}

static void intr(struct bx *b, int *mn, int *mx) {
    if (b->iv) { *mn = b->imin; *mx = b->imax; return; }
    int a = 0, z = 0;
    switch (b->inner) {
    case IN_IMG: case IN_CTL: a = z = b->iw; break;
    case IN_TABLE: tab_intr(b, &a, &z); break;
    case IN_GRID: grid_intr(b, &a, &z); break;
    case IN_FLEX: {
        int row = !(b->fdir & 2), n = 0;
        for (int c = b->first; c >= 0; c = B[c].next) {
            if (oof(&B[c])) continue;
            int x, y;
            outer_intr(&B[c], &x, &y);
            n++;
            if (row) { z += y; a = b->fwrap ? imax_(a, x) : a + x; } else { z = imax_(z, y); a = imax_(a, x); }
        }
        if (row && n > 1) { z += b->gapc * (n - 1); if (!b->fwrap) a += b->gapc * (n - 1); }
        break;
    }
    default:
        if (has_inline_content(b)) intr_inline(b, &a, &z);
        for (int c = b->first; c >= 0; c = B[c].next) {
            if (oof(&B[c]) || (!in_flow_block(&B[c]) && !B[c].flt)) continue;
            if (B[c].flt && has_inline_content(b)) continue;     /* counted with the line */
            int x, y;
            outer_intr(&B[c], &x, &y);
            a = imax_(a, x); z = imax_(z, y);
        }
    }
    b->imin = a; b->imax = z; b->iv = 1;
    *mn = a; *mx = z;
}

/* ---------- document storage ---------- */
static uint32_t arena(const char *s, int n) {
    struct doc *d = D;
    if (d->ntxt + n + 1 > d->txtcap) {
        size_t nc = d->txtcap ? d->txtcap * 2 : 16384;
        while (nc < d->ntxt + n + 1) nc *= 2;
        char *nt = realloc(d->txt, nc);
        if (!nt) return 0;
        d->txt = nt; d->txtcap = nc;
    }
    uint32_t off = d->ntxt;
    memcpy(d->txt + off, s, n);
    d->txt[off + n] = 0;
    d->ntxt += n;
    if (d->ntxt > (8u << 20)) d->truncated = 1;
    return off;
}
static uint32_t astr(const char *s) { return arena(s, strlen(s) + 1); }
static void anchor_add(uint32_t aname, int y) {
    struct doc *d = D;
    if (!aname || d->nan >= 2048) return;
    if (!(d->nan & 63)) {
        struct anchor *na = realloc(d->an, (d->nan + 64) * sizeof *na);
        if (!na) return;
        d->an = na;
    }
    strlcpy(d->an[d->nan].name, d->txt + aname - 1, sizeof d->an[0].name);
    d->an[d->nan++].y = y;
}

/* ---------- inline layout ---------- */
static void lay_block(struct bx *b, int cx, int cw, int y, struct bfc *pf, int *carry, int flags);
enum { LB_ABSY = 1, LB_TOPDONE = 2, LB_ROOT = 4 };
static void place_float(struct bx *c, int cx, int cw, int y, struct bfc *f);

#define LI_MAX 300
enum { LT_TEXT, LT_SPACE, LT_BOX, LT_OPEN, LT_CLOSE };
struct lit { uint8_t t; int b, w, x, toff, tlen; };
struct ls {
    int blk, cx, cw, y, lx, rx, any, used;
    struct bfc *f;
    struct lit li[LI_MAX]; int nli;
    struct lit ch[LI_MAX]; int nch, chw, chnw;
    char *tb; int ntb, tbcap;
    int open[48], nopen, lo[48], nlo;
    int pend, pendspw, pendbox;
    struct frag *fr; int nfr, frcap;
    int sasc, sdesc, lastbase, nlines;
};

static void ls_edges(struct ls *s) { fl_edges(s->f, s->y, s->y + s->sasc + s->sdesc, s->cx, s->cw, &s->lx, &s->rx); }

static int tb_put(struct ls *s, const char *t, int n) {
    if (s->ntb + n + 1 > s->tbcap) {
        int nc = s->tbcap ? s->tbcap * 2 : 4096;
        while (nc < s->ntb + n + 1) nc *= 2;
        char *p = realloc(s->tb, nc);
        if (!p) return -1;
        s->tb = p; s->tbcap = nc;
    }
    memcpy(s->tb + s->ntb, t, n);
    int o = s->ntb;
    s->ntb += n;
    return o;
}

static void fr_add(struct ls *s, const struct frag *f) {
    if (s->nfr == s->frcap) {
        int nc = s->frcap ? s->frcap * 2 : 32;
        struct frag *p = realloc(s->fr, nc * sizeof *p);
        if (!p) return;
        s->fr = p; s->frcap = nc;
    }
    s->fr[s->nfr++] = *f;
}

static int has_paint(const struct bx *b) { return b->bg || b->bw[0] || b->bw[1] || b->bw[2] || b->bw[3]; }
static int below(const struct bx *b) { return b->inner == IN_IMG || b->inner == IN_CTL ? b->bel : b->base >= 0 ? b->h - b->base : 0; }

struct bgr { int b, x0, x1, depth; uint8_t e; };
struct cst { int b, x0; uint8_t e; };

static void finish_line(struct ls *s, int forced) {
    struct bx *blk = &B[s->blk];
    if (!s->nli && !forced) return;
    if (!s->any && !forced) {                                    /* only empty inline boxes: no line */
        s->nli = 0; s->used = 0;
        s->nlo = s->nopen; memcpy(s->lo, s->open, s->nopen * sizeof s->open[0]);
        return;
    }
    int avail = s->rx - s->lx, total = s->used;
    int off = 0;
    if (blk->align == 1) off = (avail - total) / 2; else if (blk->align == 2) off = avail - total;
    if (off < 0) off = 0;
    int asc = s->sasc, desc = s->sdesc, toph = 0, both = 0;
    for (int i = 0; i < s->nli; i++) {
        struct lit *it = &s->li[i];
        if (it->t == LT_TEXT || it->t == LT_SPACE) {
            struct bx *tb = &B[it->b];
            const struct font *f = FT[tb->font];
            int a = f->ascent, d = f->height - f->ascent;
            if (tb->lh > 0) { int lead = tb->lh - f->height; a += lead / 2; d += lead - lead / 2; } else d += LEAD;
            if (tb->valign == VA_SUPER) a += f->ascent / 3; else if (tb->valign == VA_SUB) d += f->ascent / 3;
            asc = imax_(asc, a); desc = imax_(desc, d);
        } else if (it->t == LT_BOX) {
            struct bx *b = &B[it->b];
            int bel = below(b);
            if (b->valign == VA_TOP || b->valign == VA_BOTTOM) {
                int h = b->m[0] + b->h + b->m[2];
                if (b->valign == VA_TOP) toph = imax_(toph, h); else both = imax_(both, h);
                continue;
            }
            asc = imax_(asc, b->m[0] + b->h - bel); desc = imax_(desc, bel + b->m[2]);
        }
    }
    if (toph > asc + desc) desc = toph - asc;
    if (both > asc + desc) desc = both - asc;
    int lineh = asc + desc, base = s->y + asc;
    int x = s->lx + off;
    for (int i = 0; i < s->nli; i++) { s->li[i].x = x; x += s->li[i].w; }
    int lend = x;
    /* backgrounds and borders of inline boxes first */
    struct bgr bg[96];
    int nbg = 0;
    struct cst cs[48];
    int ncs = 0;
    for (int i = 0; i < s->nlo && ncs < 48; i++) { cs[ncs].b = s->lo[i]; cs[ncs].x0 = s->lx + off; cs[ncs].e = 0; ncs++; }
    for (int i = 0; i < s->nli; i++) {
        struct lit *it = &s->li[i];
        if (it->t == LT_OPEN && ncs < 48) { cs[ncs].b = it->b; cs[ncs].x0 = it->x + B[it->b].m[3]; cs[ncs].e = 1; ncs++; }
        else if (it->t == LT_CLOSE) {
            for (int q = ncs - 1; q >= 0; q--) if (cs[q].b == it->b) {
                if (has_paint(&B[it->b]) && nbg < 96) bg[nbg++] = (struct bgr){ it->b, cs[q].x0, it->x + it->w - B[it->b].m[1], q, (uint8_t)(cs[q].e | 2) };
                for (int j = q; j < ncs - 1; j++) cs[j] = cs[j + 1];
                ncs--;
                break;
            }
        }
    }
    for (int q = 0; q < ncs; q++) if (has_paint(&B[cs[q].b]) && nbg < 96) bg[nbg++] = (struct bgr){ cs[q].b, cs[q].x0, lend, q, cs[q].e };
    for (int i = 1; i < nbg; i++) { struct bgr t = bg[i]; int j = i - 1; while (j >= 0 && bg[j].depth > t.depth) { bg[j + 1] = bg[j]; j--; } bg[j + 1] = t; }
    for (int i = 0; i < nbg; i++) {
        struct bx *ob = &B[bg[i].b];
        const struct font *f = FT[ob->font];
        int top = base - f->ascent - ob->p[0] - ob->bw[0];
        struct frag fg = { .x = bg[i].x0, .y = top, .w = bg[i].x1 - bg[i].x0, .h = f->height + ob->p[0] + ob->p[2] + ob->bw[0] + ob->bw[2], .t = F_BG, .box = bg[i].b, .edges = bg[i].e };
        if (fg.w > 0) fr_add(s, &fg);
    }
    /* text runs and boxes */
    for (int i = 0; i < s->nli; i++) {
        struct lit *it = &s->li[i];
        if (it->t == LT_BOX) {
            struct bx *b = &B[it->b];
            int bel = below(b);
            int top = b->valign == VA_TOP ? s->y + b->m[0] : b->valign == VA_BOTTOM ? s->y + lineh - b->m[2] - b->h :
                      b->valign == VA_MIDDLE ? base - s->sasc / 3 - b->h / 2 : base - (b->h - bel);
            shift(b, it->x + b->m[3] - b->x, top - b->y);
            struct frag fg = { .t = F_BOX, .box = it->b, .x = b->x, .y = b->y, .w = b->w, .h = b->h };
            fr_add(s, &fg);
            continue;
        }
        if (it->t != LT_TEXT && it->t != LT_SPACE) continue;
        struct bx *tb = &B[it->b];
        const struct font *f = FT[tb->font];
        int dy = tb->valign == VA_SUPER ? -f->ascent / 3 : tb->valign == VA_SUB ? f->ascent / 3 : 0;
        uint8_t flags = tb->flags | (tb->link >= 0 && !tb->nul ? FL_UL : 0);
        struct frag *last = s->nfr ? &s->fr[s->nfr - 1] : 0;
        const char *t = it->t == LT_SPACE ? " " : s->tb + it->toff;
        int n = it->t == LT_SPACE ? 1 : it->tlen;
        if (last && last->t == F_TEXTRUN && last->font == tb->font && last->flags == flags && last->col == tb->col && last->link == tb->link &&
            last->node == tb->node && last->x + last->w == it->x && last->y == base - f->ascent + dy && last->off + last->len == D->ntxt && last->len + n < 1500) {
            arena(t, n);
            last->len += n; last->w += it->w;
            continue;
        }
        if (it->t == LT_SPACE && (!last || last->t != F_TEXTRUN)) continue;      /* a space never starts a run */
        struct frag fg = { .t = F_TEXTRUN, .x = it->x, .y = base - f->ascent + dy, .w = it->w, .h = f->height, .font = tb->font, .flags = flags,
                           .col = tb->col, .link = tb->link, .node = tb->node, .len = n, .box = tb->vis };
        fg.off = arena(t, n);
        fr_add(s, &fg);
    }
    s->lastbase = base;
    s->y += lineh;
    s->nlines++;
    s->nli = 0; s->used = 0; s->any = 0;
    s->nlo = s->nopen; memcpy(s->lo, s->open, s->nopen * sizeof s->open[0]);
    ls_edges(s);
}

static void commit(struct ls *s, const struct lit *it) {
    if (s->nli >= LI_MAX - 1) finish_line(s, 0);
    if (it->t == LT_OPEN) {
        if (s->nopen < 48) s->open[s->nopen++] = it->b;
        if (B[it->b].aname) anchor_add(B[it->b].aname, s->y);
        if (it->w) s->any = 1;
    } else if (it->t == LT_CLOSE) {
        for (int q = s->nopen - 1; q >= 0; q--) if (s->open[q] == it->b) { for (int j = q; j < s->nopen - 1; j++) s->open[j] = s->open[j + 1]; s->nopen--; break; }
        if (it->w) s->any = 1;
    } else if (it->t != LT_SPACE) s->any = 1;
    s->li[s->nli++] = *it;
    s->used += it->w;
}

static int split_fit(const struct bx *tb, const char *t, int n, int fit) {   /* bytes of t that fit in `fit` px (at least one character) */
    int k = 0, acc = 0;
    while (k < n) {
        uint32_t cp;
        int l = utf8_decode(t + k, n - k, &cp), cwid = gfx_text_width_n(FT[tb->font], t + k, l);
        if (acc + cwid > fit && k) break;
        k += l; acc += cwid;
    }
    return k;
}

static void chunk_flush(struct ls *s) {
    if (!s->nch) return;
    int sp = s->pend && s->any ? s->pendspw : 0;
    if (s->any && s->used + sp + s->chw > s->rx - s->lx) { finish_line(s, 0); sp = 0; }   /* nowrap chunks still move down as a whole */
    if (!s->any) {
        sp = 0;
        while (s->chw > s->rx - s->lx && s->f) {                 /* narrower than the chunk because of floats: drop below them */
            int ny = fl_next(s->f, s->y, s->y + s->sasc + s->sdesc);
            if (ny <= s->y) break;
            s->y = ny; ls_edges(s);
        }
    }
    if (sp) commit(s, &(struct lit){ .t = LT_SPACE, .b = s->pendbox, .w = sp });
    for (int i = 0; i < s->nch; i++) {
        struct lit it = s->ch[i];
        if (it.t == LT_TEXT && !s->chnw) {
            struct bx *tb = &B[it.b];
            for (;;) {                                            /* a run wider than what is left of the line: break inside it */
                int fit = s->rx - s->lx - s->used;
                if (it.w <= fit || it.tlen <= 1) break;
                int k = split_fit(tb, s->tb + it.toff, it.tlen, fit);
                if (k >= it.tlen) break;
                int kw = tw(tb, s->tb + it.toff, k);
                commit(s, &(struct lit){ .t = LT_TEXT, .b = it.b, .w = kw, .toff = it.toff, .tlen = k });
                finish_line(s, 0);
                it.toff += k; it.tlen -= k; it.w = tw(tb, s->tb + it.toff, it.tlen);
            }
        }
        commit(s, &it);
    }
    s->nch = 0; s->chw = 0; s->chnw = 0; s->pend = 0;
}

static void ls_chunk_add(struct ls *s, const struct lit *it, int nowrap) {
    if (s->nch >= LI_MAX - 1) chunk_flush(s);
    s->ch[s->nch++] = *it;
    s->chw += it->w;
    if (nowrap) s->chnw = 1;
}

static void ls_word(void *u, struct bx *tb, const char *t, int n) {
    struct ls *s = u;
    int o = tb_put(s, t, n);
    if (o < 0) return;
    ls_chunk_add(s, &(struct lit){ .t = LT_TEXT, .b = tb - B, .w = tw(tb, t, n), .toff = o, .tlen = n }, tb->ws == 1);
}
static void ls_space(void *u, struct bx *tb) {
    struct ls *s = u;
    if (tb->ws == 1) {                                            /* nowrap: the space is part of the chunk */
        int o = tb_put(s, " ", 1);
        if (o >= 0 && s->nch) ls_chunk_add(s, &(struct lit){ .t = LT_TEXT, .b = tb - B, .w = spw(tb), .toff = o, .tlen = 1 }, 1);
        return;
    }
    chunk_flush(s);
    s->pend = 1; s->pendspw = spw(tb); s->pendbox = tb - B;
}
static void ls_brk(void *u, struct bx *b) {
    struct ls *s = u;
    chunk_flush(s);
    s->pend = 0;
    finish_line(s, 1);
}
static void ls_open(void *u, struct bx *b) {
    struct ls *s = u;
    resolve(b, s->cw);
    ls_chunk_add(s, &(struct lit){ .t = LT_OPEN, .b = b - B, .w = b->m[3] + b->bw[3] + b->p[3] }, b->ws == 1);
}
static void ls_close(void *u, struct bx *b) {
    struct ls *s = u;
    ls_chunk_add(s, &(struct lit){ .t = LT_CLOSE, .b = b - B, .w = b->m[1] + b->bw[1] + b->p[1] }, 0);
}

/* height: N% of an image: the containing block's height when that is known before layout (a px height, or an aspect-ratio box of width cw) */
static int repl_pct_h(const struct bx *b, int cw) {
    int p = b->par;
    if (oof(b)) while (p >= 0 && B[p].pos == POS_STATIC && B[p].par >= 0) p = B[p].par;
    if (p < 0) return -1;
    const struct bx *q = &B[p];
    int e = oof(b) ? 0 : hedge(q), ph = q->k[L_H] == K_PX ? q->v[L_H] : q->aspect > 0 ? (int)((long long)(cw - e) * 1000 / q->aspect) : -1;
    return ph < 0 ? -1 : pctof(b->v[L_H], ph);
}

static void repl_size(struct bx *b, int cw, int *W, int *H) {
    int nw = b->iw, nh = b->ih;
    int w = b->k[L_W] == K_AUTO ? -1 : rl(b, L_W, cw), h = b->k[L_H] == K_PX ? b->v[L_H] : b->k[L_H] == K_PCT ? repl_pct_h(b, cw) : -1;
    if (b->inner == IN_CTL) { w = w >= 0 ? w : nw; h = h >= 0 ? h : nh; }
    else if (w < 0 && h < 0) { w = nw; h = nh; }
    else if (w < 0) w = nh ? (int)((long long)nw * h / nh) : nw;
    else if (h < 0) h = nw ? (int)((long long)nh * w / nw) : nh;
    int mx = b->k[L_MAXW] == K_AUTO ? -1 : rl(b, L_MAXW, cw);
    if (mx >= 0 && w > mx) { h = w ? (int)((long long)h * mx / w) : h; w = mx; }
    if (b->k[L_MAXH] == K_PX && h > b->v[L_MAXH]) { w = h ? (int)((long long)w * b->v[L_MAXH] / h) : w; h = b->v[L_MAXH]; }
    if (b->k[L_MINW] != K_AUTO && w < rl(b, L_MINW, cw)) { int m = rl(b, L_MINW, cw); h = w ? (int)((long long)h * m / w) : h; w = m; }
    if (b->k[L_MINH] == K_PX && h < b->v[L_MINH]) h = b->v[L_MINH];
    if (cw > 8 && w > cw) { if (b->inner == IN_IMG) h = (int)((long long)h * cw / w); w = cw; }
    *W = w < 1 ? 1 : w > 8192 ? 8192 : w;
    *H = h < 1 ? 1 : h > 8192 ? 8192 : h;
}

static void ls_atom(void *u, struct bx *b) {
    struct ls *s = u;
    resolve(b, s->cw);
    if (b->inner == IN_IMG || b->inner == IN_CTL) {
        int W, H;
        repl_size(b, s->cw - b->m[1] - b->m[3], &W, &H);
        b->x = b->y = 0; b->w = W + hedge(b); b->h = H + vedge(b);
    } else {
        int mn, mx, e = hedge(b) + b->m[1] + b->m[3];
        intr(b, &mn, &mx);
        int sw = csize(b, L_W, s->cw, 1);
        int fit = sw >= 0 ? sw : imin_(imax_(mn, s->cw - e), mx);
        b->fw = clampw(b, fit, s->cw);
        int c0 = 0;
        lay_block(b, s->cx, s->cw, s->y, 0, &c0, LB_ABSY | LB_ROOT);
    }
    int ow = b->w + b->m[1] + b->m[3];
    if (b->ws != 1) chunk_flush(s);                              /* an atomic inline is a line break opportunity on both sides, space or not (label<input width:100%> wraps) */
    ls_chunk_add(s, &(struct lit){ .t = LT_BOX, .b = b - B, .w = ow }, b->ws == 1);
    if (b->ws != 1) chunk_flush(s);
}
static void ls_flt(void *u, struct bx *b) {
    struct ls *s = u;
    chunk_flush(s);
    place_float(b, s->cx, s->cw, s->y, s->f);
    ls_edges(s);
}
static void ls_abs(void *u, struct bx *b) {
    struct ls *s = u;
    b->sx = s->lx + s->used; b->sy = s->y;
}

/* lay the inline content of block b out in [cx, cx+cw) from y0; returns its height. *lastbase: baseline of the last line */
static int lay_inlines(struct bx *b, int cx, int cw, int y0, struct bfc *f, int *lastbase) {
    struct ls *s = calloc(1, sizeof *s);
    if (!s) return 0;
    free(b->fr); b->fr = 0; b->nfr = 0;
    const struct font *bf = FT[b->font];
    s->blk = b - B; s->cx = cx; s->cw = cw; s->y = y0; s->f = f;
    s->sasc = bf->ascent; s->sdesc = bf->height - bf->ascent;
    if (b->lh > 0) { int lead = b->lh - bf->height; s->sasc += lead / 2; s->sdesc += lead - lead / 2; } else s->sdesc += LEAD;
    ls_edges(s);
    struct iw w = { .u = s, .word = ls_word, .space = ls_space, .brk = ls_brk, .open = ls_open, .close = ls_close, .atom = ls_atom, .flt = ls_flt, .absb = ls_abs, .sp = 1 };
    inl_walk(&w, b - B);
    chunk_flush(s);
    finish_line(s, 0);
    b->fr = s->fr; b->nfr = s->nfr;
    *lastbase = s->nlines ? s->lastbase : -1;
    int h = s->y - y0;
    free(s->tb); free(s);
    return h;
}

/* ---------- blocks ---------- */
static int *ABS, nabs, abscap;
static int rootw;
static int tab_plan(struct bx *b, int avail, int target);
static void tab_place(struct bx *b, int cx, int cy, int *ch);

static int abs_reg(int bi) {
    if (nabs == abscap) {
        int nc = abscap ? abscap * 2 : 64;
        int *p = realloc(ABS, nc * sizeof *p);
        if (!p) return 0;
        ABS = p; abscap = nc;
    }
    ABS[nabs++] = bi;
    return 1;
}

static struct bx *first_block(const struct bx *b) {
    for (int c = b->first; c >= 0; c = B[c].next) {
        if (oof(&B[c]) || B[c].flt) continue;
        return in_flow_block(&B[c]) ? &B[c] : 0;
    }
    return 0;
}
static struct bx *last_block(const struct bx *b) {
    struct bx *r = 0;
    for (int c = b->first; c >= 0; c = B[c].next) if (!oof(&B[c]) && !B[c].flt) r = in_flow_block(&B[c]) ? &B[c] : 0;
    return r;
}
static int can_ctop(const struct bx *b) { return b->inner == IN_FLOW && !is_bfc(b) && !b->bw[0] && !b->p[0] && first_block(b) != 0; }
static int can_cbot(const struct bx *b) {
    return b->inner == IN_FLOW && !is_bfc(b) && !b->bw[2] && !b->p[2] && b->k[L_H] == K_AUTO && b->k[L_MINH] == K_AUTO && last_block(b) != 0;
}

static int top_chain(struct bx *b, int cw) {
    int m = b->m[0];
    if (can_ctop(b)) {
        struct bx *c = first_block(b);
        if (!c->clr) { resolve(c, cw); m = collapse(m, top_chain(c, cw)); }
    }
    return m;
}

/* the block children of b, stacked from cy. Returns the bottom y; *carry: bottom margin still pending */
static int lay_blocks(struct bx *b, int cx, int cw, int cy, struct bfc *f, int *carry, int topdone) {
    int y = cy;
    *carry = 0;
    for (int ci = b->first; ci >= 0; ci = B[ci].next) {
        struct bx *c = &B[ci];
        if (oof(c)) { c->sx = cx; c->sy = y + imax_(*carry, 0); continue; }
        if (c->flt) { place_float(c, cx, cw, y + imax_(*carry, 0), f); continue; }
        int fl = 0;
        if (c->clr) {
            int ny = fl_clear(f, c->clr, y + *carry);
            if (ny > y + *carry) { y = ny; *carry = 0; topdone = 0; }
        }
        if (topdone) fl = LB_TOPDONE;
        topdone = 0;
        lay_block(c, cx, cw, y, f, carry, fl);
        y = c->y + c->h;
    }
    return y;
}

static void lay_abs(struct bx *c, int cbi) {
    int px, py, pw, ph;
    if (cbi < 0 || c->pos == POS_FIXED) { px = py = 0; pw = rootw; ph = VIEWH; }
    else {
        struct bx *p = &B[cbi];
        px = p->x + p->bw[3]; py = p->y + p->bw[0];
        pw = p->w - p->bw[1] - p->bw[3]; ph = p->h - p->bw[0] - p->bw[2];
    }
    resolve(c, pw);
    int l = c->k[L_LEFT] != K_AUTO, r = c->k[L_RIGHT] != K_AUTO, t = c->k[L_TOP] != K_AUTO, bt = c->k[L_BOTTOM] != K_AUTO;
    int L = rl(c, L_LEFT, pw), R = rl(c, L_RIGHT, pw), T = rl(c, L_TOP, ph), Bm = rl(c, L_BOTTOM, ph);
    int hed = hedge(c), sw = csize(c, L_W, pw, 1), cwid;
    if (c->inner == IN_IMG || c->inner == IN_CTL) { int W, H; repl_size(c, pw, &W, &H); cwid = W; }
    else if (sw >= 0) cwid = sw;
    else if (l && r) cwid = pw - L - R - c->m[1] - c->m[3] - hed;
    else {
        int mn, mx;
        intr(c, &mn, &mx);
        int avail = pw - (l ? L : 0) - (r ? R : 0) - c->m[1] - c->m[3] - hed;
        cwid = imin_(imax_(mn, avail), mx);
    }
    cwid = imax_(clampw(c, cwid, pw), 0);
    c->fw = cwid;
    int c0 = 0;
    lay_block(c, 0, pw, 0, 0, &c0, LB_ABSY | LB_ROOT);
    int x, y;
    int mal = c->k[L_ML] == K_AUTO, mar = c->k[L_MR] == K_AUTO;
    if (l && r && (sw >= 0) && (mal || mar)) {
        int free = pw - L - R - c->w - c->m[1] - c->m[3];
        if (free > 0) { if (mal && mar) { c->m[3] = free / 2; c->m[1] = free - free / 2; } else if (mal) c->m[3] = free; else c->m[1] = free; }
    }
    if (l) x = px + L + c->m[3]; else if (r) x = px + pw - R - c->m[1] - c->w; else x = c->sx + c->m[3];
    if (t && bt && c->k[L_H] == K_AUTO) { int h = ph - T - Bm - c->m[0] - c->m[2]; if (h > c->h) c->h = h; }
    if (t) y = py + T + c->m[0]; else if (bt) y = py + ph - Bm - c->m[2] - c->h; else y = c->sy + c->m[0];
    shift(c, x - c->x, y - c->y);
}
static void lay_abs_for(int cbi) {
    for (int i = 0; i < nabs; i++) {
        struct bx *c = &B[ABS[i]];
        if (c->cb != cbi || c->absdone) continue;
        c->absdone = 1;
        lay_abs(c, cbi);
    }
}

static void place_float(struct bx *c, int cx, int cw, int y, struct bfc *f) {
    resolve(c, cw);
    int mn, mx, e = hedge(c) + c->m[1] + c->m[3];
    intr(c, &mn, &mx);
    int sw = csize(c, L_W, cw, 1);
    int fit = sw >= 0 ? sw : imin_(imax_(mn, cw - e), mx);
    c->fw = imax_(clampw(c, fit, cw), 0);
    int c0 = 0;
    lay_block(c, cx, cw, 0, 0, &c0, LB_ABSY | LB_ROOT);
    int mw = c->w + c->m[1] + c->m[3], mh = c->h + c->m[0] + c->m[2];
    int yc = imax_(y, f->ytop);
    if (c->clr) yc = fl_clear(f, c->clr, yc);
    int lx, rx;
    for (int guard = 0; guard < 200; guard++) {
        fl_edges(f, yc, yc + imax_(mh, 1), cx, cw, &lx, &rx);
        if (rx - lx >= mw) break;
        int ny = fl_next(f, yc, yc + imax_(mh, 1));
        if (ny <= yc) break;
        yc = ny;
    }
    int x = c->flt == 1 ? lx : rx - mw;
    shift(c, x + c->m[3] - c->x, yc + c->m[0] - c->y);
    if (f->n < MAXFL) f->f[f->n++] = (struct fl){ x, yc, mw, mh, c->flt };
    f->ytop = yc;
}

/* ---------- flexbox ---------- */
struct fi { struct bx *b; int seq, order, hyp, main, cross, edges, grow, shrink, basis, minc, maxc, ml, mr, mt, mb, line; };

static int fi_cmp(const void *a, const void *b) {
    const struct fi *x = a, *y = b;
    return x->order != y->order ? (x->order > y->order ? 1 : -1) : x->line - y->line;   /* equal order: document order (line holds it until the flex lines are built) */
}

static void lay_flex(struct bx *b, int cx, int cw, int cy, struct bfc *f, int *ch, int defh) {
    int n = 0;
    for (int c = b->first; c >= 0; c = B[c].next) if (!oof(&B[c])) n++;
    for (int c = b->first; c >= 0; c = B[c].next) if (oof(&B[c])) { B[c].sx = cx; B[c].sy = cy; }
    *ch = 0;
    if (!n) return;
    struct fi *it = calloc(n, sizeof *it);
    if (!it) return;
    int k = 0, row = !(b->fdir & 2), rev = b->fdir & 1;
    for (int c = b->first; c >= 0; c = B[c].next) {
        struct bx *x = &B[c];
        if (oof(x)) continue;
        resolve(x, cw);
        it[k].b = x; it[k].line = k; it[k].order = x->order; it[k].grow = x->grow; it[k].shrink = x->shrink;
        k++;
    }
    qsort(it, n, sizeof *it, fi_cmp);
    int mgap = row ? b->gapc : b->gapr, xgap = row ? b->gapr : b->gapc;
    if (row) {
        for (int i = 0; i < n; i++) {
            struct bx *x = it[i].b;
            int mn, mx, he = hedge(x);
            intr(x, &mn, &mx);
            it[i].edges = he + x->m[1] + x->m[3];
            int sw = csize(x, L_W, cw, 1), basis;
            if (x->k[L_BASIS] != K_AUTO) { basis = rl(x, L_BASIS, cw); if (x->bsz) basis -= he; if (basis < 0) basis = 0; }
            else if (x->inner == IN_IMG || x->inner == IN_CTL) { int W, H; repl_size(x, cw, &W, &H); basis = W; }
            else basis = sw >= 0 ? sw : mx;
            int mnw = csize(x, L_MINW, cw, 1), mxw = csize(x, L_MAXW, cw, 1);
            it[i].minc = mnw >= 0 ? mnw : x->ovf ? 0 : sw >= 0 ? imin_(mn, sw) : mn;   /* automatic minimum: min-content (0 when it scrolls or clips) */
            it[i].maxc = mxw >= 0 ? mxw : BIGW;
            it[i].basis = basis;
            it[i].hyp = imin_(imax_(basis, it[i].minc), it[i].maxc);
        }
        /* lines */
        int nl = 0, start = 0, sum = 0;
        int *lstart = calloc(n + 1, sizeof *lstart);
        if (!lstart) { free(it); return; }
        for (int i = 0; i < n; i++) {
            int w = it[i].hyp + it[i].edges + (i > start ? mgap : 0);
            if (b->fwrap && i > start && sum + w > cw) { lstart[nl++] = start; start = i; sum = it[i].hyp + it[i].edges; }
            else sum += w;
        }
        lstart[nl++] = start;
        lstart[nl] = n;
        int y = cy, tot = 0;
        int linecross[64];
        for (int li = 0; li < nl && li < 64; li++) {
            int a = lstart[li], z = lstart[li + 1], cnt = z - a, used = mgap * (cnt - 1), tg = 0, ts = 0;
            for (int i = a; i < z; i++) { used += it[i].hyp + it[i].edges; tg += it[i].grow; ts += it[i].shrink * imax_(it[i].basis, 1) / 1; }
            int free = cw - used;
            for (int i = a; i < z; i++) it[i].main = it[i].hyp;
            for (int pass = 0; pass < 3 && free != 0; pass++) {
                if (free > 0 && tg > 0) {
                    int dist = 0, ntg = 0;
                    for (int i = a; i < z; i++) {
                        int add = (int)((long long)free * it[i].grow / tg);
                        int nm = it[i].main + add;
                        if (nm > it[i].maxc) { add = it[i].maxc - it[i].main; nm = it[i].maxc; } else ntg += it[i].grow;
                        it[i].main = nm; dist += add;
                    }
                    free -= dist; tg = ntg;
                } else if (free < 0 && ts > 0) {
                    int dist = 0, nts = 0;
                    for (int i = a; i < z; i++) {
                        int w = it[i].shrink * imax_(it[i].basis, 1);
                        int sub = (int)((long long)(-free) * w / ts);
                        int nm = it[i].main - sub;
                        if (nm < it[i].minc) { sub = it[i].main - it[i].minc; nm = it[i].minc; } else nts += w;
                        it[i].main = nm; dist += sub;
                    }
                    free += dist; ts = nts;
                } else break;
            }
            int lc = 0;
            for (int i = a; i < z; i++) {
                struct bx *x = it[i].b;
                x->fw = imax_(it[i].main, 0);
                int c0 = 0;
                lay_block(x, cx, cw, y, 0, &c0, LB_ABSY | LB_ROOT);
                it[i].cross = x->h + x->m[0] + x->m[2];
                lc = imax_(lc, it[i].cross);
            }
            if (nl == 1 && defh >= 0) lc = defh;
            linecross[li] = lc;
            /* justify */
            int placed = 0;
            for (int i = a; i < z; i++) placed += it[i].main + it[i].edges;
            placed += mgap * (cnt - 1);
            int rest = cw - placed, autos = 0;
            for (int i = a; i < z; i++) { autos += it[i].b->k[L_ML] == K_AUTO; autos += it[i].b->k[L_MR] == K_AUTO; }
            int lead = 0, between = 0;
            if (rest > 0) {
                if (autos) { int per = rest / autos; for (int i = a; i < z; i++) { if (it[i].b->k[L_ML] == K_AUTO) it[i].b->m[3] = per; if (it[i].b->k[L_MR] == K_AUTO) it[i].b->m[1] = per; } }
                else switch (b->just) {
                case 1: lead = rest; break;
                case 2: lead = rest / 2; break;
                case 3: if (cnt > 1) between = rest / (cnt - 1); break;
                case 4: between = rest / cnt; lead = between / 2; break;
                case 5: between = rest / (cnt + 1); lead = between; break;
                }
            }
            int maxb = 0;                                       /* align-items: baseline: the deepest first baseline in the line */
            for (int i = a; i < z; i++) {
                struct bx *bb = it[i].b;
                if ((bb->als ? bb->als - 1 : b->ali) == 4) maxb = imax_(maxb, bb->m[0] + (bb->base >= 0 ? bb->base : bb->h));
            }
            int x = cx + lead;
            for (int j = 0; j < cnt; j++) {
                int i = rev ? z - 1 - j : a + j;
                struct bx *bb = it[i].b;
                int al = bb->als ? bb->als - 1 : b->ali;       /* als: 0 auto, else 1 + align value */
                int avail = lc - bb->m[0] - bb->m[2], yy = y + bb->m[0];
                if (al == 0 && bb->k[L_H] == K_AUTO && bb->h < avail) { int hh = clamph(bb, avail - vedge(bb), 0) + vedge(bb); bb->h = hh; }
                else if (al == 2) yy = y + lc - bb->m[2] - bb->h;
                else if (al == 3) yy = y + bb->m[0] + (avail - bb->h) / 2;
                else if (al == 4) yy = y + maxb - (bb->base >= 0 ? bb->base : bb->h);
                shift(bb, x + bb->m[3] - bb->x, yy - bb->y);
                x += bb->w + bb->m[3] + bb->m[1] + between + mgap;
            }
            y += lc + xgap;
            tot += lc + (li ? xgap : 0);
        }
        *ch = tot;
        free(lstart);
    } else {
        int y = cy, tot = 0;
        for (int i = 0; i < n; i++) {
            struct bx *x = it[i].b;
            int mn, mx, he = hedge(x);
            intr(x, &mn, &mx);
            int sw = csize(x, L_W, cw, 1), al = x->als ? x->als - 1 : b->ali;
            int e = he + x->m[1] + x->m[3], fit;
            if (x->inner == IN_IMG || x->inner == IN_CTL) { int W, H; repl_size(x, cw, &W, &H); fit = W; }
            else fit = sw >= 0 ? sw : al == 0 ? cw - e : imin_(imax_(mn, cw - e), mx);
            x->fw = imax_(clampw(x, fit, cw), 0);
            int c0 = 0;
            lay_block(x, cx, cw, y, 0, &c0, LB_ABSY | LB_ROOT);
            it[i].cross = x->h + x->m[0] + x->m[2];
            tot += it[i].cross;
        }
        int n1 = n > 1 ? mgap * (n - 1) : 0;
        int free = defh >= 0 ? defh - tot - n1 : 0, tg = 0;
        for (int i = 0; i < n; i++) tg += it[i].grow;
        if (free > 0 && tg > 0) for (int i = 0; i < n; i++) { it[i].b->h += (int)((long long)free * it[i].grow / tg); it[i].cross = it[i].b->h + it[i].b->m[0] + it[i].b->m[2]; }
        tot = n1;
        for (int i = 0; i < n; i++) tot += it[i].cross;
        int rest = defh >= 0 ? defh - tot : 0, lead = 0, between = 0;
        if (rest > 0) switch (b->just) {
        case 1: lead = rest; break;
        case 2: lead = rest / 2; break;
        case 3: if (n > 1) between = rest / (n - 1); break;
        case 4: between = rest / n; lead = between / 2; break;
        case 5: between = rest / (n + 1); lead = between; break;
        }
        y = cy + lead;
        for (int j = 0; j < n; j++) {
            int i = rev ? n - 1 - j : j;
            struct bx *x = it[i].b;
            int al = x->als ? x->als - 1 : b->ali, xx = cx + x->m[3], avail = cw - x->m[1] - x->m[3];
            if (al == 2) xx = cx + cw - x->m[1] - x->w;
            else if (al == 3) xx = cx + x->m[3] + (avail - x->w) / 2;
            if (x->k[L_ML] == K_AUTO && x->k[L_MR] == K_AUTO) xx = cx + (cw - x->w) / 2;
            else if (x->k[L_ML] == K_AUTO) xx = cx + cw - x->m[1] - x->w;
            shift(x, xx - x->x, y + x->m[0] - x->y);
            y += it[i].cross + mgap + between;
        }
        *ch = defh >= 0 ? imax_(defh, tot) : tot;
    }
    free(it);
}

/* ---------- grid ----------
 * Tracks from grid-template-columns (px, %, fr, auto, minmax(), repeat(n|auto-fill, ...)), items placed by named areas,
 * line numbers/spans or auto-flow (row-major, sparse), columns sized fixed -> auto (max-content) -> fr, rows by content
 * (or their px size). ponytail: no subgrid, no dense packing, no implicit column tracks beyond the explicit count. */
struct trk { uint8_t kind; int v, mn; };   /* 0 px, 1 fr (x100), 2 auto, 3 percent; mn: minmax()'s fixed minimum (px) */
#define GMAXC 24
#define GMAXR 400

static int css_px(const char *s) {          /* "12px", "1.5em", "3rem": px (em = 16) */
    double v = atof(s);
    const char *u = s;
    while (*u && (isdigit((unsigned char)*u) || *u == '.' || *u == '-')) u++;
    return (int)(!strncmp(u, "em", 2) || !strncmp(u, "rem", 3) ? v * 16 : v);
}

/* calc() in a track size: + - * / and parentheses over px, %, em, rem (ponytail: no min()/max()/clamp() inside) */
static double gc_sum(const char **p, int cw);
static double gc_atom(const char **p, int cw) {
    while (isspace((unsigned char)**p)) (*p)++;
    if (**p == '(') { (*p)++; double v = gc_sum(p, cw); while (isspace((unsigned char)**p)) (*p)++; if (**p == ')') (*p)++; return v; }
    char *e;
    double v = strtod(*p, &e);
    if (e == *p) { if (**p) (*p)++; return 0; }
    *p = e;
    if (**p == '%') { (*p)++; return v * cw / 100; }
    if (!strncmp(*p, "rem", 3)) { *p += 3; return v * 16; }
    if (!strncmp(*p, "em", 2)) { *p += 2; return v * 16; }
    if (!strncmp(*p, "px", 2)) *p += 2;
    return v;
}
static double gc_prod(const char **p, int cw) {
    double v = gc_atom(p, cw);
    for (;;) {
        while (isspace((unsigned char)**p)) (*p)++;
        if (**p == '*') { (*p)++; v *= gc_atom(p, cw); }
        else if (**p == '/') { (*p)++; double d = gc_atom(p, cw); if (d != 0) v /= d; }
        else return v;
    }
}
static double gc_sum(const char **p, int cw) {
    double v = gc_prod(p, cw);
    for (;;) {
        while (isspace((unsigned char)**p)) (*p)++;
        if (**p == '+') { (*p)++; v += gc_prod(p, cw); }
        else if (**p == '-') { (*p)++; v -= gc_prod(p, cw); }
        else return v;
    }
}

static int top_comma(const char *p) {                       /* offset of the last comma directly inside the outer brackets, -1 if none */
    int d = 0, r = -1;
    for (int i = 0; p[i]; i++) { if (p[i] == '(') d++; else if (p[i] == ')') d--; else if (p[i] == ',' && d == 1) r = i; }
    return r;
}
static int is_fn_len(const char *p) { return !strncmp(p, "calc(", 5) || !strncmp(p, "min(", 4) || !strncmp(p, "max(", 4) || !strncmp(p, "clamp(", 6); }
/* a calc()/min()/max()/clamp() length in px against cw; plain calc() keeps the local evaluator (its truncation is what the goldens expect),
 * anything with min/max/clamp inside (var() results nest them) goes to CSS's */
static int fn_len(const char *p, int cw, int *px) {
    if (!strncmp(p, "calc(", 5) && !strstr(p, "min(") && !strstr(p, "max(") && !strstr(p, "clamp(")) { const char *q = p + 4; *px = (int)gc_atom(&q, cw); return 1; }
    return css_calc_px(p, cw, px);
}

static int track_one(const char *t, int len, struct trk *o, int cw) {
    char b[1024];
    int px;
    o->mn = -1;
    if (len >= (int)sizeof b) len = sizeof b - 1;
    memcpy(b, t, len); b[len] = 0;
    char *p = b;
    if (!strncmp(p, "minmax(", 7)) {
        const char *a = p + 7;
        while (*a == ' ') a++;
        if (isdigit((unsigned char)*a) && strcspn(a, "%,") == strcspn(a, ",")) o->mn = css_px(a);
        else if (is_fn_len(a)) {          /* minmax(min(..)/calc(..), x): the minimum, evaluated against the container */
            char *mc = (char *)a; int d = 0;
            while (*mc && (d || *mc != ',')) { if (*mc == '(') d++; else if (*mc == ')') d--; mc++; }
            if (*mc == ',') { *mc = 0; if (fn_len(a, cw, &px)) o->mn = px < 0 ? 0 : px; *mc = ','; }
        }
    }
    if (!strncmp(p, "minmax(", 7) || !strncmp(p, "fit-content(", 12)) {
        int c = top_comma(p);
        p = c >= 0 ? p + c + 1 : strchr(p, '(') + 1;
        while (*p == ' ') p++;
        char *e = p + strlen(p);
        if (e > p && e[-1] == ')') e[-1] = 0;
    }
    if (is_fn_len(p) && fn_len(p, cw, &px)) { o->kind = 0; o->v = px < 0 ? 0 : px; }   /* calc()/min()/max()/clamp() track sizes */
    else if (strstr(p, "fr")) { o->kind = 1; o->v = (int)(atof(p) * 100); if (o->v <= 0) o->v = 100; }
    else if (strchr(p, '%')) { o->kind = 3; o->v = (int)atof(p); }
    else if (isdigit((unsigned char)*p) || *p == '.') { o->kind = 0; o->v = css_px(p); }
    else { o->kind = 2; o->v = 0; }
    return 1;
}

static int g_nitems;                        /* in-flow children of the grid being laid out (auto-fit track cap) */
static int track_min(const char *s, int cw) {   /* minmax()'s minimum: px, %, em or min(a, b) of those */
    while (*s == ' ') s++;
    if (!strncmp(s, "min(", 4)) {
        int a = track_min(s + 4, cw);
        const char *c = strchr(s, ',');
        int b = c ? track_min(c + 1, cw) : a;
        return imin_(a, b);
    }
    double v = atof(s);
    const char *u = s;
    while (*u && (isdigit((unsigned char)*u) || *u == '.' || *u == '-')) u++;
    return *u == '%' ? (int)(v * cw / 100) : css_px(s);
}

static int parse_tracks_raw(const char *s, struct trk *t, int max, int cw, int gap);
static int parse_tracks(const char *s, struct trk *t, int max, int cw, int gap) {   /* whitespace runs (newlines, tabs) -> one space */
    char buf[4096];                                           /* var() expansion makes long templates */
    if (!s) return 0;
    int k = 0, sp = 0;
    for (; *s && k < (int)sizeof buf - 1; s++) {
        if (isspace((unsigned char)*s)) { sp = 1; continue; }
        if (sp && k && buf[k - 1] != '(' && *s != ')' && *s != ',' && buf[k - 1] != ',' && k < (int)sizeof buf - 2) buf[k++] = ' ';
        sp = 0;
        buf[k++] = *s;
    }
    buf[k] = 0;
    return parse_tracks_raw(buf, t, max, cw, gap);
}
static int parse_tracks_raw(const char *s, struct trk *t, int max, int cw, int gap) {
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '[') { while (*s && *s != ']') s++; if (*s) s++; continue; }   /* line names: named_line() reads them from the template text */
        const char *e = s;
        int d = 0;
        while (*e && (*e != ' ' || d)) { if (*e == '(') d++; if (*e == ')') d--; e++; }
        if (!strncmp(s, "repeat(", 7)) {
            const char *a = s + 7, *comma = strchr(a, ',');
            if (comma && comma < e) {
                struct trk in[8];
                int ni = 0, cnt;
                char sub[1024];
                int sl = (int)(e - comma - 2);
                if (sl < 0) sl = 0;
                if (sl > 1023) sl = 1023;
                memcpy(sub, comma + 1, sl); sub[sl] = 0;
                ni = parse_tracks(sub, in, 8, cw, gap);
                if (isdigit((unsigned char)*a)) cnt = atoi(a);
                else {                                       /* auto-fill / auto-fit: as many as fit */
                    int m = 0;
                    const char *mm = strstr(sub, "minmax(");
                    m = mm ? track_min(mm + 7, cw) : in[0].kind == 0 ? in[0].v : 200;
                    if (m < 40) m = 40;
                    cnt = imax_(1, (cw + gap) / (m + gap));
                    if (!strncmp(a, "auto-fit", 8) && g_nitems > 0 && cnt > g_nitems) cnt = g_nitems;   /* auto-fit collapses the empty tracks */
                    for (int i = 0; i < ni; i++) if (in[i].kind == 1 || in[i].kind == 2) { in[i].kind = 1; in[i].v = 100; }
                }
                if (ni < 1) ni = 1, in[0] = (struct trk){ 2, 0 };
                for (int c = 0; c < cnt && n < max; c++) for (int i = 0; i < ni && n < max; i++) t[n++] = in[i];
            }
        } else { track_one(s, e - s, &t[n], cw); n++; }
        s = e;
    }
    return n;
}

/* shrink-to-fit width of a grid: items fall into the explicit columns in order; px tracks keep their size, the others take their widest item (ponytail: ignores spans and explicit placement) */
static void grid_intr(struct bx *b, int *mn, int *mx) {
    struct trk ct[GMAXC];
    g_nitems = 0;
    int nc = parse_tracks(b->gcols ? css_str(b->gcols - 1) : 0, ct, GMAXC, 0, b->gapc), i = 0, a = 0, z = 0;
    int cmn[GMAXC] = { 0 }, cmx[GMAXC] = { 0 };
    if (nc < 1) nc = 1;
    for (int c = b->first; c >= 0; c = B[c].next) {
        if (oof(&B[c])) continue;
        int x, y, q = i++ % nc;
        outer_intr(&B[c], &x, &y);
        if (x > cmn[q]) cmn[q] = x;
        if (y > cmx[q]) cmx[q] = y;
    }
    for (int q = 0; q < nc && q < GMAXC; q++) {
        if (b->gcols && q < nc && ct[q].kind == 0) { a += ct[q].v; z += ct[q].v; } else { a += cmn[q]; z += cmx[q]; }
    }
    a += b->gapc * (nc - 1); z += b->gapc * (nc - 1);
    *mn = a; *mx = z;
}

static int line_of(uint8_t k, int v, int nlines) {             /* 1-based line from a K_PX line number (negative counts from the end); 0 = auto */
    if (k != K_PX) return 0;
    return v > 0 ? v : nlines + 1 + v;
}

/* named grid lines: [a b] markers in a grid-template-columns/rows string; returns the 0-based line index of the first line called `name`, or -1 */
static int scan_lines(const char *s, const char *end, const char *nm, int ln, int *pos) {
    while (s < end) {
        while (s < end && *s == ' ') s++;
        if (s >= end) break;
        if (*s == '[') {
            const char *q = s + 1;
            while (q < end && *q != ']') {
                while (q < end && *q == ' ') q++;
                const char *w = q;
                while (q < end && *q != ' ' && *q != ']') q++;
                if ((int)(q - w) == ln && !strncmp(w, nm, ln)) return *pos;
            }
            s = q < end ? q + 1 : end;
            continue;
        }
        const char *e = s;
        int d = 0;
        while (e < end && (*e != ' ' || d)) { if (*e == '(') d++; if (*e == ')') d--; e++; }
        const char *comma = !strncmp(s, "repeat(", 7) ? memchr(s, ',', e - s) : 0;
        if (comma) {
            int cnt = isdigit((unsigned char)s[7]) ? atoi(s + 7) : 1, st = *pos;
            if (cnt > 40) cnt = 40;
            for (int i = 0; i < cnt; i++) { int r = scan_lines(comma + 1, e - 1, nm, ln, pos); if (r >= 0) return r; }
        } else (*pos)++;
        s = e;
    }
    return -1;
}
static int named_line(const char *tmpl, const char *base, int edge) {   /* edge 0: start, 1: end; `base` is a custom-ident */
    if (!tmpl || !base) return -1;
    char nm[48];
    int l = strlen(base), pos;
    if (l > 40) return -1;
    memcpy(nm, base, l); strcpy(nm + l, edge ? "-end" : "-start");
    pos = 0;
    int r = scan_lines(tmpl, tmpl + strlen(tmpl), nm, l + (edge ? 4 : 6), &pos);
    if (r >= 0) return r;
    pos = 0;
    return scan_lines(tmpl, tmpl + strlen(tmpl), base, l, &pos);
}

struct gitem { struct bx *b; int r, c, rs, cs; };

static void lay_grid(struct bx *b, int cx, int cw, int cy, struct bfc *f, int *ch) {
    struct trk ct[GMAXC], rt[GMAXR];
    g_nitems = 0;
    for (int c = b->first; c >= 0; c = B[c].next) if (!oof(&B[c])) g_nitems++;
    int nc = parse_tracks(b->gcols ? css_str(b->gcols - 1) : 0, ct, GMAXC, cw, b->gapc), nr_t = parse_tracks(b->grows ? css_str(b->grows - 1) : 0, rt, GMAXR, cw, b->gapr);
    if (nc < 1) { nc = 1; ct[0] = (struct trk){ 1, 100 }; }
    /* areas */
    struct { char name[24]; int r0, c0, r1, c1; } ar[32];
    int nar = 0;
    if (b->gareas) {
        const char *s = css_str(b->gareas - 1);
        int row = 0;
        while (*s) {
            if (*s != '"' && *s != 39) { s++; continue; }
            char qt = *s++;
            int col = 0;
            while (*s && *s != qt) {
                while (*s == ' ') s++;
                const char *e = s;
                while (*e && *e != ' ' && *e != qt) e++;
                if (e > s) {
                    int l = imin_((int)(e - s), 23), q;
                    for (q = 0; q < nar; q++) if (!strncmp(ar[q].name, s, l) && !ar[q].name[l]) break;
                    if (q == nar && nar < 32) { memcpy(ar[nar].name, s, l); ar[nar].name[l] = 0; ar[nar].r0 = row; ar[nar].c0 = col; ar[nar].r1 = row; ar[nar].c1 = col; nar++; }
                    else if (q < nar) { ar[q].r1 = row; ar[q].c1 = col; }
                    col++;
                }
                s = e;
            }
            if (*s) s++;
            row++;
        }
    }
    int n = 0;
    for (int c = b->first; c >= 0; c = B[c].next) if (!oof(&B[c])) n++; else { B[c].sx = cx; B[c].sy = cy; }
    *ch = 0;
    if (!n) return;
    struct gitem *it = calloc(n, sizeof *it);
    uint8_t *occ = calloc((size_t)GMAXR * GMAXC, 1);
    if (!it || !occ) { free(it); free(occ); return; }
    int k = 0, cr = 0, cc = 0, nrows = 0;
    for (int c = b->first; c >= 0; c = B[c].next) {
        struct bx *x = &B[c];
        if (oof(x)) continue;
        resolve(x, cw);
        int r0 = -1, c0 = -1, rs = 1, cs = 1;
        if (x->garea) {
            const char *nm = css_str(x->garea - 1);
            for (int q = 0; q < nar; q++) if (!strcmp(ar[q].name, nm)) { r0 = ar[q].r0; c0 = ar[q].c0; rs = ar[q].r1 - ar[q].r0 + 1; cs = ar[q].c1 - ar[q].c0 + 1; }
            if (r0 < 0) {                                   /* no such template area: lines called nm-start / nm-end define one */
                const char *ctm = b->gcols ? css_str(b->gcols - 1) : 0, *rtm = b->grows ? css_str(b->grows - 1) : 0;
                int a = named_line(ctm, nm, 0), z = named_line(ctm, nm, 1), ra = named_line(rtm, nm, 0), rz = named_line(rtm, nm, 1);
                if (a >= 0) { c0 = a; if (z > a) cs = z - a; }
                if (ra >= 0) { r0 = ra; if (rz > ra) rs = rz - ra; }
            }
        } else {
            const char *ctm = b->gcols ? css_str(b->gcols - 1) : 0, *rtm = b->grows ? css_str(b->grows - 1) : 0;
            int s = line_of(x->gk[0], x->g[0], nc + 1), e = line_of(x->gk[1], x->g[1], nc + 1);
            if (x->gk[0] == K_STR) s = named_line(ctm, css_str(x->g[0]), 0) + 1;
            if (x->gk[1] == K_STR) e = named_line(ctm, css_str(x->g[1]), 1) + 1;
            if (x->gk[1] == K_SPAN) cs = x->g[1]; else if (s && e > s) cs = e - s;
            if (x->gk[0] == K_SPAN) cs = x->g[0];
            if (s > 0) c0 = s - 1;
            s = line_of(x->gk[2], x->g[2], nr_t + 1); e = line_of(x->gk[3], x->g[3], nr_t + 1);
            if (x->gk[2] == K_STR) s = named_line(rtm, css_str(x->g[2]), 0) + 1;
            if (x->gk[3] == K_STR) e = named_line(rtm, css_str(x->g[3]), 1) + 1;
            if (x->gk[3] == K_SPAN) rs = x->g[3]; else if (s && e > s) rs = e - s;
            if (x->gk[2] == K_SPAN) rs = x->g[2];
            if (s > 0) r0 = s - 1;
        }
        cs = imax_(1, imin_(cs, nc)); rs = imax_(1, imin_(rs, 50));
        if (c0 >= 0 && c0 + cs > nc) c0 = imax_(0, nc - cs);
        if (r0 >= GMAXR - 60) r0 = GMAXR - 61;
        int pr = r0, pc = c0;
        if (pr < 0 && pc < 0) { pr = cr; pc = cc; }
        else if (pr < 0) pr = cr;
        else if (pc < 0) pc = 0;
        if (pc + cs > nc) { pc = imax_(0, c0 >= 0 ? nc - cs : 0); if (c0 < 0) pr++; }
        if (!(r0 >= 0 && c0 >= 0))
            for (int guard = 0; guard < GMAXR * GMAXC && pr < GMAXR - 61; guard++) {
                int free = 1;
                for (int rr = pr; rr < pr + rs && free; rr++) for (int q = pc; q < pc + cs; q++) if (occ[rr * GMAXC + q]) { free = 0; break; }
                if (free) break;
                if (c0 >= 0) pr++; else { pc++; if (pc + cs > nc) { pc = 0; pr++; } }
            }
        if (pr > GMAXR - 61) pr = GMAXR - 61;
        for (int rr = pr; rr < pr + rs; rr++) for (int q = pc; q < pc + cs; q++) occ[rr * GMAXC + q] = 1;
        it[k] = (struct gitem){ x, pr, pc, rs, cs };
        if (r0 < 0 && c0 < 0) { cr = pr; cc = pc + cs; if (cc >= nc) { cc = 0; cr = pr + 1; } }
        else if (r0 < 0) cr = pr;
        if (it[k].r + it[k].rs > nrows) nrows = it[k].r + it[k].rs;
        k++;
    }
    if (nrows > GMAXR - 60) nrows = GMAXR - 60;
    /* column widths */
    int cwid[GMAXC], fixed = 0, frsum = 0, gaps = b->gapc * (nc - 1);
    for (int i = 0; i < nc; i++) {
        cwid[i] = 0;
        if (ct[i].kind == 3 && ct[i].mn >= 0) { ct[i].kind = 0; ct[i].v = pctof(ct[i].v, cw); }   /* minmax(0, 25%): a range, grows to 25% only if there's room */
        if (ct[i].kind == 0) cwid[i] = ct[i].v;
        else if (ct[i].kind == 3) cwid[i] = pctof(ct[i].v, cw);
        else if (ct[i].kind == 2) {
            for (int q = 0; q < k; q++) if (it[q].cs == 1 && it[q].c == i) { int mn, mx; outer_intr(it[q].b, &mn, &mx); cwid[i] = imax_(cwid[i], mx); }
        } else frsum += ct[i].v;
        if (ct[i].kind != 1) fixed += cwid[i];
    }
    for (int i = 0; i < nc; i++) if (ct[i].kind == 0 && ct[i].mn >= 0 && ct[i].mn < ct[i].v) { fixed -= cwid[i] - ct[i].mn; cwid[i] = ct[i].mn; }   /* minmax(a,b): start at a */
    int fmin = 0;
    for (int i = 0; i < nc; i++) if (ct[i].kind == 1 && ct[i].mn > 0) fmin += ct[i].mn;
    int room = cw - gaps - fixed;
    {                                                                /* minmax(a,b) tracks grow towards b with what the fr minimums leave */
        int extra = 0, give = imax_(room - fmin, 0);
        for (int i = 0; i < nc; i++) if (ct[i].kind == 0 && ct[i].mn >= 0 && ct[i].mn < ct[i].v) extra += ct[i].v - cwid[i];
        if (extra > 0 && give > 0) {
            give = imin_(give, extra);
            for (int i = 0; i < nc; i++) if (ct[i].kind == 0 && ct[i].mn >= 0 && ct[i].mn < ct[i].v) { int g = (int)((long long)give * (ct[i].v - cwid[i]) / extra); cwid[i] += g; fixed += g; }
            room = cw - gaps - fixed;
        }
    }
    if (room < 0 && fixed > 0) {                                  /* auto tracks overflow: shrink them, never below 0 */
        int autos = 0;
        for (int i = 0; i < nc; i++) if (ct[i].kind == 2) autos += cwid[i];
        if (autos > 0) { int take = imin_(-room, autos); for (int i = 0; i < nc; i++) if (ct[i].kind == 2) cwid[i] -= (int)((long long)take * cwid[i] / autos); room += take; }
    }
    if (frsum) {
        int rem = imax_(room, 0), used = 0, lastfr = -1, fs = frsum;
        uint8_t frz[GMAXC] = { 0 };
        for (int again = 1; again;) {                                /* an fr track whose share is under its minimum freezes at the minimum */
            again = 0;
            for (int i = 0; i < nc; i++) if (ct[i].kind == 1 && !frz[i] && ct[i].mn > 0 && fs > 0 && (long long)rem * ct[i].v / fs < ct[i].mn) { frz[i] = 1; cwid[i] = ct[i].mn; rem -= ct[i].mn; fs -= ct[i].v; again = 1; }
            if (rem < 0) rem = 0;
        }
        for (int i = 0; i < nc; i++) if (ct[i].kind == 1 && !frz[i]) { cwid[i] = fs > 0 ? (int)((long long)rem * ct[i].v / fs) : 0; used += cwid[i]; lastfr = i; }
        if (lastfr >= 0) cwid[lastfr] += rem - used;
    }
    int colx[GMAXC + 1], x = cx;
    for (int i = 0; i < nc; i++) { colx[i] = x; x += cwid[i] + b->gapc; }
    /* lay the items out at their column widths, then rows */
    int rowh[GMAXR] = { 0 };
    for (int q = 0; q < k; q++) {
        struct bx *xb = it[q].b;
        int w = b->gapc * (it[q].cs - 1);
        for (int i = it[q].c; i < it[q].c + it[q].cs; i++) w += cwid[i];
        int e = hedge(xb) + xb->m[1] + xb->m[3], sw = csize(xb, L_W, w, 1);
        xb->fw = imax_(clampw(xb, sw >= 0 ? sw : w - e, w), 0);
        int c0 = 0;
        lay_block(xb, colx[it[q].c], w, 0, 0, &c0, LB_ABSY | LB_ROOT);
        int h = xb->h + xb->m[0] + xb->m[2];
        if (it[q].rs == 1) rowh[it[q].r] = imax_(rowh[it[q].r], h);
    }
    for (int r = 0; r < nrows; r++) if (r < nr_t && rt[r].kind == 0) rowh[r] = imax_(rowh[r], rt[r].v);
    for (int q = 0; q < k; q++) if (it[q].rs > 1) {
        int h = it[q].b->h + it[q].b->m[0] + it[q].b->m[2], have = b->gapr * (it[q].rs - 1);
        for (int r = it[q].r; r < it[q].r + it[q].rs; r++) have += rowh[r];
        if (h > have) rowh[it[q].r + it[q].rs - 1] += h - have;
    }
    int rowy[GMAXR + 1], y = cy;
    for (int r = 0; r < nrows; r++) { rowy[r] = y; y += rowh[r] + b->gapr; }
    for (int q = 0; q < k; q++) {
        struct bx *xb = it[q].b;
        int span = b->gapr * (it[q].rs - 1);
        for (int r = it[q].r; r < it[q].r + it[q].rs; r++) span += rowh[r];
        int al = xb->als ? xb->als - 1 : b->ali, avail = span - xb->m[0] - xb->m[2], yy = rowy[it[q].r] + xb->m[0];
        if (al == 0 && xb->k[L_H] == K_AUTO && xb->h < avail) xb->h = clamph(xb, avail - vedge(xb), 0) + vedge(xb);
        else if (al == 2) yy += avail - xb->h;
        else if (al == 3) yy += (avail - xb->h) / 2;
        shift(xb, colx[it[q].c] + xb->m[3] - xb->x, yy - xb->y);
    }
    *ch = nrows ? y - b->gapr - cy : 0;
    free(it); free(occ);
}

/* ---------- the block box ---------- */
static void lay_block(struct bx *b, int cx, int cw, int y, struct bfc *pf, int *carry, int flags) {
    resolve(b, cw);
    int outer_inl = b->outer == OUT_INLINE, root = (flags & LB_ROOT) || is_bfc(b);
    int hed = hedge(b), fw = b->fw;
    b->fw = -1;
    int cwid, sw = csize(b, L_W, cw, 1);
    int ml = b->m[3], mr = b->m[1];
    if (b->inner == IN_IMG || b->inner == IN_CTL) {
        int W, H;
        repl_size(b, cw - ml - mr, &W, &H);
        cwid = fw >= 0 ? fw : W;                              /* a flex item's / abs box's resolved main size wins */
    } else if (b->inner == IN_TABLE) {
        int avail = imax_(cw - ml - mr - hed, 1);
        cwid = tab_plan(b, avail, fw >= 0 ? fw : sw);
    } else if (fw >= 0) cwid = fw;
    else if (sw >= 0) cwid = sw;
    else cwid = cw - ml - mr - hed;
    if (fw < 0 && b->inner != IN_TABLE && b->inner != IN_IMG && b->inner != IN_CTL) cwid = clampw(b, cwid, cw);
    if (cwid < 0) cwid = 0;
    int total = ml + cwid + hed + mr;
    if (!outer_inl && !(flags & LB_ABSY)) {          /* auto margins centre in-flow blocks; floats, flex/grid items and abs boxes place themselves */
        if (cw > total) {
            int free = cw - total, al = b->k[L_ML] == K_AUTO, ar = b->k[L_MR] == K_AUTO;
            if (al && ar) { ml = free / 2; mr = free - free / 2; } else if (al) ml = free; else if (ar) mr = free;
        } else if (b->k[L_ML] == K_AUTO && b->inner != IN_TABLE) ml = 0;
    }
    int x = cx + ml;
    if (root && !b->flt && !oof(b) && !outer_inl && pf && pf->n && !(flags & LB_ABSY)) {     /* a BFC box doesn't overlap floats: narrow it or move it beside them */
        int ytop = y + imax_(carry ? *carry : 0, 0), lx, rx;
        fl_edges(pf, ytop, ytop + 1, cx, cw, &lx, &rx);
        if (lx > cx || rx < cx + cw) {
            if (sw < 0 && fw < 0 && b->inner != IN_TABLE) cwid = imax_(imin_(cwid, rx - lx - ml - mr - hed), 0);
            x = imax_(x, lx + ml);
        }
    }
    b->m[3] = ml; b->m[1] = mr;
    /* vertical position */
    int top;
    int empty = b->inner == IN_FLOW && b->first < 0 && b->aspect <= 0 && b->k[L_H] == K_AUTO && b->k[L_MINH] == K_AUTO && !vedge(b) && !root && !b->li;
    if (flags & LB_ABSY) top = y + b->m[0];
    else if (flags & LB_TOPDONE) top = y;
    else if (empty) top = y;
    else { top = y + collapse(*carry, top_chain(b, cw)); *carry = 0; }
    if (empty && !(flags & LB_ABSY)) {
        b->x = x; b->y = top; b->w = cwid + hed; b->h = 0;
        *carry = collapse(*carry, (flags & LB_TOPDONE) ? b->m[2] : collapse(b->m[0], b->m[2]));
        if (b->pos != POS_STATIC) lay_abs_for(b - B);
        return;
    }
    b->x = x; b->y = top; b->w = cwid + hed;
    int ix = x + b->bw[3] + b->p[3], iy = top + b->bw[0] + b->p[0];
    struct bfc *own = 0, *f = pf;
    if (root) {
        own = calloc(1, sizeof *own);
        if (!own) return;
        f = own; own->ytop = iy;
    }
    int ch = 0, carry_last = 0, lastbase = -1, sh = b->k[L_H] == K_PCT ? -1 : csize(b, L_H, 0, 0);
    int nf0 = f ? f->n : 0;
    int bottom_collapse = 0;
    switch (b->inner) {
    case IN_IMG: case IN_CTL: { int W, H; repl_size(b, cw - ml - mr, &W, &H); ch = H; break; }
    case IN_FLEX: lay_flex(b, ix, cwid, iy, f, &ch, sh); break;
    case IN_GRID:
        lay_grid(b, ix, cwid, iy, f, &ch);
        break;
    case IN_TABLE:
        if (b->inner == IN_TABLE) { tab_place(b, ix, iy, &ch); break; }
        /* fallthrough */
    default:
        if (has_inline_content(b)) {
            ch = lay_inlines(b, ix, cwid, iy, f, &lastbase);
            if (lastbase >= 0) b->base = lastbase - top;
        } else {
            int bot = lay_blocks(b, ix, cwid, iy, f, &carry_last, can_ctop(b));
            ch = bot - iy;
            bottom_collapse = can_cbot(b);
            struct bx *lb = last_block(b);
            if (lb && lb->base >= 0) b->base = lb->y - top + lb->base;
        }
    }
    if (bottom_collapse) *carry = collapse(carry_last, b->m[2]);
    else { ch += carry_last; *carry = b->m[2]; }
    if (ch == 0 && f && f->n > nf0) {                              /* a box holding only floats still contains them */
        int bot = iy;
        for (int i = nf0; i < f->n; i++) bot = imax_(bot, f->f[i].y + f->f[i].h);
        ch = bot - iy;
    }
    if (own) {
        for (int i = 0; i < own->n; i++) ch = imax_(ch, own->f[i].y + own->f[i].h - iy);
        free(own);
    }
    int hh = sh >= 0 ? sh : ch;
    if (b->inner == IN_TABLE || b->inner == IN_FLEX) hh = sh >= 0 ? imax_(sh, 0) : ch;
    if (b->aspect > 0 && sh < 0 && b->inner != IN_IMG && b->inner != IN_CTL) hh = imax_(hh, (int)((long long)cwid * 1000 / b->aspect));
    if (b->inner == IN_IMG || b->inner == IN_CTL) hh = ch;
    else if (b->inner == IN_TCELL && sh >= 0) hh = imax_(sh, ch);
    hh = clamph(b, hh, 0);
    b->h = hh + vedge(b);
    if (b->inner == IN_FLOW && b->ovf && b->base >= 0) b->base = b->h;      /* inline-block with overflow: baseline = bottom margin edge */
    if (b->pos != POS_STATIC || b->par < 0) lay_abs_for(b - B);
    if (flags & LB_ABSY) *carry = 0;
}

/* ---------- tables ----------
 * Rows and cells are boxes (build adds anonymous ones). A table is a grid (colspan/rowspan), sized from the cells'
 * min/max-content widths by tab_cols, then every cell is laid out at its column width, shifted to its row and
 * vertically aligned. Cells are laid out in document order, so the items stay in reading order. */
#define TC_MAX 64
#define TR_MAX 3000
#define TCELL_MAX 8000
enum { W_NONE, W_PX, W_PCT };

struct tcell { int box, r, c, rs, cn, minw, maxw, fixw, wv, x, w, h; uint8_t wk; };
struct tab {
    struct tcell *c; int nc, cap;
    int *rows, nr, rcap;
    uint64_t *occ; int ocap;
    int ncols, sp, spy, collapse, capbox;
    int minc[TC_MAX], maxc[TC_MAX], fix[TC_MAX], pct[TC_MAX], w[TC_MAX], x[TC_MAX];
};

static void tab_free(struct tab *T) { if (T) { free(T->c); free(T->rows); free(T->occ); free(T); } }

static int tab_cell_add(struct tab *T, int row, int cell, int *curc) {
    if (T->nc >= TCELL_MAX) return 0;
    struct bx *cb = &B[cell];
    int cn = cb->cs < 1 ? 1 : cb->cs > TC_MAX ? TC_MAX : cb->cs, rs = cb->rs < 1 ? 1 : cb->rs > 1000 ? 1000 : cb->rs;
    int col = *curc;
    while (col < TC_MAX && row < T->ocap && (T->occ[row] >> col & 1)) col++;
    if (col >= TC_MAX) return 0;
    if (col + cn > TC_MAX) cn = TC_MAX - col;
    int last = row + rs < TR_MAX ? row + rs : TR_MAX;
    if (last > T->ocap) {
        int nc = last + 16;
        uint64_t *n = realloc(T->occ, nc * sizeof *n);
        if (!n) return 0;
        memset(n + T->ocap, 0, (nc - T->ocap) * sizeof *n);
        T->occ = n; T->ocap = nc;
    }
    uint64_t bits = (cn == 64 ? ~0ull : (1ull << cn) - 1) << col;
    for (int r = row; r < last; r++) T->occ[r] |= bits;
    *curc = col + cn;
    if (col + cn > T->ncols) T->ncols = col + cn;
    if (T->nc == T->cap) {
        int nc = T->cap ? T->cap * 2 : 32;
        struct tcell *n = realloc(T->c, nc * sizeof *n);
        if (!n) return 0;
        T->c = n; T->cap = nc;
    }
    T->c[T->nc++] = (struct tcell){ .box = cell, .r = row, .c = col, .rs = rs, .cn = cn };
    return 1;
}

static void tab_row_add(struct tab *T, int row, int *r) {
    if (T->nr >= TR_MAX) return;
    if (T->nr == T->rcap) {
        int nc = T->rcap ? T->rcap * 2 : 16;
        int *n = realloc(T->rows, nc * sizeof *n);
        if (!n) return;
        T->rows = n; T->rcap = nc;
    }
    T->rows[T->nr] = row;
    *r = T->nr++;
    int curc = 0;
    for (int c = B[row].first; c >= 0; c = B[c].next) if (B[c].inner == IN_TCELL) tab_cell_add(T, *r, c, &curc);
}

static struct tab *tab_build(struct bx *b) {
    struct tab *T = calloc(1, sizeof *T);
    if (!T) return 0;
    T->capbox = -1;
    T->sp = b->hdr ? 0 : b->gapc; T->spy = b->hdr ? 0 : b->gapr; T->collapse = b->hdr;
    for (int c = b->first; c >= 0; c = B[c].next) {
        int r;
        if (B[c].inner == IN_CAP) { if (T->capbox < 0) T->capbox = c; }
        else if (B[c].inner == IN_TROW) tab_row_add(T, c, &r);
        else if (B[c].inner == IN_TGROUP) for (int q = B[c].first; q >= 0; q = B[q].next) if (B[q].inner == IN_TROW) tab_row_add(T, q, &r);
    }
    return T;
}

/* border-collapse: neighbouring cells share one line (the wider wins; the right/bottom cell stops drawing its own) */
static void tab_collapse(struct tab *T) {
    int nc = T->ncols, nr = T->nr;
    uint16_t *cm = calloc((size_t)nr * nc, sizeof *cm);
    if (!cm) return;
    for (int i = 0; i < T->nc; i++) {
        const struct tcell *c = &T->c[i];
        for (int r = c->r; r < c->r + c->rs && r < nr; r++) for (int k = c->c; k < c->c + c->cn && k < nc; k++) cm[r * nc + k] = i + 1;
    }
    for (int i = 0; i < T->nc; i++) {
        struct bx *x = &B[T->c[i].box];
        int k = T->c[i].c + T->c[i].cn, r = T->c[i].r + T->c[i].rs;
        if (k < nc && cm[T->c[i].r * nc + k]) {
            struct bx *y = &B[T->c[cm[T->c[i].r * nc + k] - 1].box];
            if (y->bw[3] > x->bw[1]) { x->bw[1] = y->bw[3]; x->bc[1] = y->bc[3]; }
            y->bw[3] = 0;
        }
        if (r < nr && cm[r * nc + T->c[i].c]) {
            struct bx *y = &B[T->c[cm[r * nc + T->c[i].c] - 1].box];
            if (y->bw[0] > x->bw[2]) { x->bw[2] = y->bw[0]; x->bc[2] = y->bc[0]; }
            y->bw[0] = 0;
        }
    }
    free(cm);
}

static int shrink(int *w, const int *mn, const uint8_t *sel, int n, int over) {
    long room = 0;
    for (int i = 0; i < n; i++) if (sel[i]) room += w[i] - mn[i];
    if (room <= 0 || over <= 0) return over;
    int take = over < room ? over : (int)room, done = 0;
    for (int i = 0; i < n; i++) if (sel[i]) { int c = (int)((long)take * (w[i] - mn[i]) / room); w[i] -= c; done += c; }
    for (int i = 0; i < n && done < take; i++) if (sel[i] && w[i] > mn[i]) { w[i]--; done++; }
    return over - done;
}

static void grow(int *w, const int *wt, const uint8_t *sel, int n, int extra) {
    long tot = 0;
    int ns = 0, done = 0;
    for (int i = 0; i < n; i++) if (sel[i]) { tot += wt[i]; ns++; }
    if (!ns) return;
    for (int i = 0; i < n; i++) if (sel[i]) { int c = tot ? (int)((long)extra * wt[i] / tot) : extra / ns; w[i] += c; done += c; }
    for (int i = 0; i < n && done < extra; i++) if (sel[i]) { w[i]++; done++; }
}

/* cell min/max-content (outer: padding and border included) */
static void tab_measure(struct tab *T) {
    for (int i = 0; i < T->nc; i++) {
        struct tcell *c = &T->c[i];
        struct bx *cb = &B[c->box];
        resolve(cb, 0);
        int mn, mx, edge = hedge(cb);
        intr(cb, &mn, &mx);
        c->minw = mn + edge; c->maxw = mx + edge;
        if (cb->k[L_W] == K_PX) { c->wk = W_PX; c->wv = cb->v[L_W]; c->fixw = (cb->bsz ? c->wv : c->wv + edge); if (c->fixw < c->minw) c->fixw = c->minw; }
        else if (cb->k[L_W] == K_PCT) { c->wk = W_PCT; c->wv = cb->v[L_W]; }
    }
}

/* column widths for a grid `target` wide (-1 = fit the content, at most `avail`); returns the grid width (spacing included) */
static int tab_cols(struct tab *T, int avail, int target) {
    int nc = T->ncols, sptot = T->sp * (nc + 1);
    memset(T->minc, 0, sizeof T->minc); memset(T->maxc, 0, sizeof T->maxc); memset(T->fix, 0, sizeof T->fix); memset(T->pct, 0, sizeof T->pct);
    for (int i = 0; i < T->nc; i++) {
        const struct tcell *c = &T->c[i];
        if (c->cn != 1) continue;
        int k = c->c;
        if (c->minw > T->minc[k]) T->minc[k] = c->minw;
        if (c->maxw > T->maxc[k]) T->maxc[k] = c->maxw;
        if (c->wk == W_PX && c->fixw > T->fix[k]) T->fix[k] = c->fixw;
        if (c->wk == W_PCT && c->wv > T->pct[k]) T->pct[k] = c->wv;
    }
    for (int span = 2; span <= nc; span++)
        for (int i = 0; i < T->nc; i++) {
            const struct tcell *c = &T->c[i];
            if (c->cn != span || c->c + span > nc) continue;
            for (int pass = 0; pass < 2; pass++) {
                int *a = pass ? T->maxc : T->minc, have = T->sp * (span - 1), need = pass ? c->maxw : c->minw;
                for (int k = c->c; k < c->c + span; k++) have += a[k];
                if (need <= have) continue;
                int add = need - have, each = add / span, rem = add % span;
                for (int k = c->c; k < c->c + span; k++) a[k] += each + (k - c->c < rem);
            }
        }
    for (int k = 0; k < nc; k++) if (T->maxc[k] < T->minc[k]) T->maxc[k] = T->minc[k];
    int pref[TC_MAX], sum = 0, anypct = 0;
    uint8_t flex[TC_MAX] = { 0 }, rigid[TC_MAX] = { 0 };
    if (target < 0) for (int k = 0; k < nc; k++) anypct |= T->pct[k] > 0;
    int basis = (target >= 0 ? target : anypct ? avail : 0) - sptot;
    for (int k = 0; k < nc; k++) {
        pref[k] = T->fix[k] ? (T->fix[k] > T->minc[k] ? T->fix[k] : T->minc[k]) : T->maxc[k];
        if (T->pct[k] && basis > 0 && !T->fix[k]) { int p = basis * T->pct[k] / 100; pref[k] = p > T->minc[k] ? p : T->minc[k]; }
        sum += pref[k];
        flex[k] = !T->fix[k] && !T->pct[k];
        rigid[k] = !flex[k];
    }
    int tw = target >= 0 ? target : anypct ? avail : sum + sptot;
    if (tw > avail) tw = avail;
    int cw = tw - sptot;
    if (cw < nc) cw = nc;
    for (int k = 0; k < nc; k++) T->w[k] = pref[k];
    if (sum < cw) {
        if (target >= 0 || anypct) {
            int none = 1;
            for (int k = 0; k < nc; k++) if (flex[k]) none = 0;
            grow(T->w, T->maxc, none ? rigid : flex, nc, cw - sum);
        } else tw = sum + sptot;
    } else if (sum > cw) {
        int over = sum - cw;
        over = shrink(T->w, T->minc, flex, nc, over);
        over = shrink(T->w, T->minc, rigid, nc, over);
        if (over > 0) {                                          /* even the minimums don't fit: squeeze everything, words will break */
            int tot = 0, done = 0;
            for (int k = 0; k < nc; k++) tot += T->w[k];
            for (int k = 0; k < nc; k++) { int nw = (int)((long)T->w[k] * (tot - over) / tot); if (nw < 1) nw = 1; done += T->w[k] - nw; T->w[k] = nw; }
            T->w[nc - 1] += done - over;
            if (T->w[nc - 1] < 1) T->w[nc - 1] = 1;
        }
    }
    int x = T->sp;
    for (int k = 0; k < nc; k++) { T->x[k] = x; x += T->w[k] + T->sp; }
    return tw;
}

static void tab_intr(struct bx *b, int *mn, int *mx) {
    struct tab *T = tab_build(b);
    *mn = *mx = 0;
    if (!T) return;
    if (T->nc && T->ncols) {
        for (int i = 0; i < T->nc; i++) if (T->c[i].rs > T->nr - T->c[i].r) T->c[i].rs = T->nr - T->c[i].r;
        tab_measure(T);
        int tw = tab_cols(T, BIGW, -1), m = T->sp * (T->ncols + 1);
        for (int k = 0; k < T->ncols; k++) m += T->minc[k];
        *mx = tw; *mn = m;
        if (T->capbox >= 0) { int a, z; outer_intr(&B[T->capbox], &a, &z); *mn = imax_(*mn, a); }
    }
    tab_free(T);
}

static int tab_plan(struct bx *b, int avail, int target) {
    tab_free(b->tab);
    struct tab *T = b->tab = tab_build(b);
    if (!T || !T->nc || !T->ncols) return target >= 0 ? imin_(target, avail) : 0;
    for (int i = 0; i < T->nc; i++) if (T->c[i].rs > T->nr - T->c[i].r) T->c[i].rs = T->nr - T->c[i].r;
    if (T->collapse) tab_collapse(T);
    tab_measure(T);
    if (target > avail) target = avail;
    return tab_cols(T, avail, target);
}

static void tab_place(struct bx *b, int cx, int cy, int *ch) {
    struct tab *T = b->tab;
    *ch = 0;
    if (!T) return;
    int tw = b->w - hedge(b), y0 = cy;
    if (T->capbox >= 0) {                                         /* caption: a block above the grid */
        struct bx *cp = &B[T->capbox];
        int c0 = 0;
        lay_block(cp, cx, tw, y0, 0, &c0, LB_ABSY | LB_ROOT);
        y0 += cp->h + cp->m[0] + cp->m[2];
    }
    if (!T->nc || !T->ncols) { *ch = y0 - cy; tab_free(T); b->tab = 0; return; }
    for (int i = 0; i < T->nc; i++) {                             /* real layout at the top of the grid, shifted into place below */
        struct tcell *c = &T->c[i];
        struct bx *cb = &B[c->box];
        c->x = T->x[c->c];
        c->w = T->sp * (c->cn - 1);
        for (int k = c->c; k < c->c + c->cn; k++) c->w += T->w[k];
        resolve(cb, c->w);
        cb->m[0] = cb->m[1] = cb->m[2] = cb->m[3] = 0;
        cb->fw = imax_(c->w - hedge(cb), 0);
        int c0 = 0;
        lay_block(cb, cx + c->x, c->w, y0, 0, &c0, LB_ABSY | LB_ROOT);
        c->h = cb->h;
    }
    int *rowh = calloc(T->nr + 1, sizeof *rowh), *rowy = calloc(T->nr + 1, sizeof *rowy), maxrs = 1;
    if (!rowh || !rowy) { free(rowh); free(rowy); return; }
    for (int i = 0; i < T->nc; i++) if (T->c[i].rs > maxrs) maxrs = T->c[i].rs;
    for (int span = 1; span <= maxrs; span++)
        for (int i = 0; i < T->nc; i++) {
            const struct tcell *c = &T->c[i];
            if (c->rs != span) continue;
            int have = T->spy * (span - 1);
            for (int r = c->r; r < c->r + span; r++) have += rowh[r];
            if (c->h > have) rowh[c->r + span - 1] += c->h - have;
        }
    int y = y0 + T->spy;
    for (int r = 0; r < T->nr; r++) { rowy[r] = y; y += rowh[r] + T->spy; }
    int total = y - y0;
    if (!T->nr) total = 0;
    for (int i = 0; i < T->nc; i++) {
        struct tcell *c = &T->c[i];
        struct bx *cb = &B[c->box];
        int top = rowy[c->r], H = rowy[c->r + c->rs - 1] + rowh[c->r + c->rs - 1] - top;
        shift(cb, cx + c->x - cb->x, top - cb->y);
        int inner = H - cb->h, ex = cb->valn == 1 ? inner / 2 : cb->valn == 2 ? inner : 0;
        if (cb->valn == 3 && cb->base >= 0) ex = 0;
        if (ex > 0) shift_kids(cb, 0, ex);
        cb->h = H;
    }
    for (int r = 0; r < T->nr; r++) {                             /* row (and row group) boxes cover their rows, for their backgrounds */
        struct bx *rb = &B[T->rows[r]];
        rb->x = cx; rb->w = tw; rb->y = rowy[r] - T->spy / 2 * 0; rb->h = rowh[r];
        struct bx *g = rb->par >= 0 && B[rb->par].inner == IN_TGROUP ? &B[rb->par] : 0;
        if (g) {
            if (!g->w) { g->x = cx; g->w = tw; g->y = rowy[r]; g->h = 0; }
            g->h = rowy[r] + rowh[r] - g->y;
        }
    }
    *ch = (y0 - cy) + total;
    free(rowh); free(rowy);
    tab_free(T); b->tab = 0;
}

/* ---------- paint: boxes -> doc items and decors ---------- */
static int cur_clip;
struct pend { int b, z, seq; };
static struct pend *PE;
static int npe, pecap;

static void emit_decor(int x, int y, int w, int h, color_t col, int rad, int ring) {
    struct doc *d = D;
    if (!col || w <= 0 || h <= 0) return;
    if (d->ndc == d->dccap) {
        int nc = d->dccap ? d->dccap * 2 : 256;
        struct decor *nd = realloc(d->dc, nc * sizeof *nd);
        if (!nd) return;
        d->dc = nd; d->dccap = nc;
    }
    d->dc[d->ndc++] = (struct decor){ .x = x, .y = y, .w = w, .h = h, .col = col, .it = d->nit, .clip = cur_clip, .rad = rad > 255 ? 255 : rad, .ring = ring };
}

static struct item *emit_item(void) {
    static struct item sink;
    struct doc *d = D;
    if (d->nit >= 250000) { d->truncated = 1; return &sink; }
    if (d->nit == d->itcap) {
        int nc = d->itcap ? d->itcap * 2 : 1024;
        struct item *ni = realloc(d->it, nc * sizeof *ni);
        if (!ni) return &sink;
        d->it = ni; d->itcap = nc;
    }
    struct item *it = &d->it[d->nit++];
    memset(it, 0, sizeof *it);
    it->clip = cur_clip; it->link = -1;
    return it;
}

static int clip_push(int x, int y, int w, int h) {         /* returns the previous clip index */
    struct doc *d = D;
    int prev = cur_clip;
    if (prev) {
        struct cliprect *p = &d->cl[prev];
        int x1 = imax_(x, p->x), y1 = imax_(y, p->y), x2 = imin_(x + w, p->x + p->w), y2 = imin_(y + h, p->y + p->h);
        x = x1; y = y1; w = x2 - x1; h = y2 - y1;
    }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (d->ncl == d->clcap) {
        int nc = d->clcap ? d->clcap * 2 : 64;
        struct cliprect *n = realloc(d->cl, nc * sizeof *n);
        if (!n) return prev;
        d->cl = n; d->clcap = nc;
        if (!d->ncl) d->ncl = 1;                                   /* index 0 = no clip */
    }
    if (d->ncl >= 32000) return prev;
    d->cl[d->ncl] = (struct cliprect){ x, y, w, h };
    cur_clip = d->ncl++;
    return prev;
}

/* Gradients. Axis-aligned linear ones without a radius are painted as consecutive two-colour rects (decor.gdir 1/2, cheap); everything else
 * (angled linear, radial, conic, any with border-radius) is a decor with gdir 3 that grad_render() fills per pixel at draw time.
 * ponytail: repeating-*-gradient paints as the plain one; stop colour hints and two-position stops are ignored. */
#define GS_MAX 16
#define gmin_(a, b) ((a) < (b) ? (a) : (b))
#define gmax_(a, b) ((a) > (b) ? (a) : (b))
struct gspec {
    int kind;                                        /* 0 linear, 1 radial, 2 conic */
    float ang;                                       /* radians, CSS convention (0 = up, clockwise); conic: start angle */
    int cdx, cdy;                                    /* linear "to top right" etc: corner direction (the angle then depends on the box) */
    int circle, size;                                /* radial: circle?; 0 farthest-corner 1 closest-side 2 farthest-side 3 closest-corner 4 explicit */
    int ex[2], ek[2];                                /* explicit radii */
    int px[2], pk[2];                                /* centre (K_PX px or K_PCT x100) */
    int n; color_t c[GS_MAX]; int k[GS_MAX], v[GS_MAX]; float t[GS_MAX];   /* stop kind 0 unset 1 pct 2 px 3 deg (x100) */
};
static int gs_len(const char *s, int *kind, int *out) {      /* "12px" "30%" "45deg" "0.5turn" -> kind (1 pct, 2 px, 3 angle), value x100 (px: x1) */
    char *e; double v = strtod(s, &e);
    if (e == s) return 0;
    if (*e == '%') { *kind = 1; *out = (int)(v * 100); }
    else if (!strncmp(e, "deg", 3)) { *kind = 3; *out = (int)(v * 100); }
    else if (!strncmp(e, "turn", 4)) { *kind = 3; *out = (int)(v * 36000); }
    else if (!strncmp(e, "rad", 3)) { *kind = 3; *out = (int)(v * 5729.58); }
    else { *kind = 2; *out = (int)(!strncmp(e, "em", 2) || !strncmp(e, "rem", 3) ? v * 16 : v); }
    return 1;
}
static int gs_parse(const char *s, struct gspec *g) {
    const char *p = strstr(s, "-gradient(");
    if (!p) return 0;
    memset(g, 0, sizeof *g);
    if (p - s >= 6 && !strncmp(p - 6, "radial", 6)) g->kind = 1; else if (p - s >= 5 && !strncmp(p - 5, "conic", 5)) g->kind = 2; else g->kind = 0;
    g->ang = 3.14159265f; g->px[0] = g->px[1] = 5000; g->pk[0] = g->pk[1] = K_PCT;
    p += 10;
    int d = 0, first = 1;
    char a[200];
    while (*p && g->n < GS_MAX) {
        const char *e = p;
        for (; *e && (d || (*e != ',' && *e != ')')); e++) { if (*e == '(') d++; if (*e == ')') d--; }
        int l = imin_((int)(e - p), 199);
        memcpy(a, p, l); a[l] = 0;
        char *q = a;
        while (*q == ' ') q++;
        int isdir = 0;
        if (first) {
            if (g->kind == 0) isdir = !strncmp(q, "to ", 3) || ((isdigit((unsigned char)*q) || *q == '-' || *q == '.') && (strstr(q, "deg") || strstr(q, "turn") || strstr(q, "rad")));
            else isdir = strstr(q, "at ") || !strncmp(q, "from", 4) || !strncmp(q, "circle", 6) || !strncmp(q, "ellipse", 7) || !strncmp(q, "closest", 7) || !strncmp(q, "farthest", 8);
        }
        if (first && isdir) {
            char *at = strstr(q, "at ");
            if (at) { *at = 0; at += 3; }
            if (g->kind == 0 && !strncmp(q, "to ", 3)) {
                int up = !!strstr(q, "top"), lf = !!strstr(q, "left"), rt = !!strstr(q, "right"), dn = !!strstr(q, "bottom");
                g->ang = (up ? (lf ? 315 : rt ? 45 : 0) : dn ? (lf ? 225 : rt ? 135 : 180) : lf ? 270 : 90) * 0.0174532925f;
                if ((up || dn) && (lf || rt)) { g->cdx = rt ? 1 : -1; g->cdy = dn ? 1 : -1; }
            } else if (g->kind == 0) {
                int kk, vv;
                if (gs_len(q, &kk, &vv)) g->ang = vv * 0.0174532925f / 100;
            } else if (g->kind == 2) {
                g->ang = 0;
                if (!strncmp(q, "from", 4)) { char *t = q + 4; int kk, vv; while (*t == ' ') t++; if (gs_len(t, &kk, &vv) && kk == 3) g->ang = vv * 0.0174532925f / 100; }
            } else {
                g->circle = !!strstr(q, "circle");
                g->size = strstr(q, "closest-side") ? 1 : strstr(q, "farthest-side") ? 2 : strstr(q, "closest-corner") ? 3 : 0;
                char *t = q; int ni = 0;
                while (*t && ni < 2) {                             /* explicit radius/radii: leading numbers */
                    while (*t == ' ') t++;
                    if (!(isdigit((unsigned char)*t) || *t == '.')) break;
                    gs_len(t, &g->ek[ni], &g->ex[ni]); ni++; g->size = 4;
                    while (*t && *t != ' ') t++;
                }
                if (ni == 1) { g->ek[1] = g->ek[0]; g->ex[1] = g->ex[0]; }
                if (g->size == 4) for (int i = 0; i < 2; i++) g->ek[i] = g->ek[i] == 1 ? K_PCT : K_PX;
            }
            if (at) {                                              /* centre: 1 or 2 keywords/lengths */
                char *t = at;
                int val[2] = { 5000, 5000 }, kd[2] = { K_PCT, K_PCT }, ni = 0, vfirst = 0;
                while (*t && ni < 2) {
                    while (*t == ' ') t++;
                    if (!*t) break;
                    int v = -1, kk = K_PCT;
                    if (!strncmp(t, "left", 4)) v = 0; else if (!strncmp(t, "right", 5)) v = 10000; else if (!strncmp(t, "center", 6)) v = 5000;
                    else if (!strncmp(t, "top", 3)) { v = 0; vfirst = ni == 0; } else if (!strncmp(t, "bottom", 6)) { v = 10000; vfirst = ni == 0; }
                    else { int k2, v2; if (gs_len(t, &k2, &v2)) { kk = k2 == 1 ? K_PCT : K_PX; v = v2; } }
                    if (v >= 0) { val[ni] = v; kd[ni] = kk; ni++; }
                    while (*t && *t != ' ') t++;
                }
                if (ni == 1 && vfirst) { val[1] = val[0]; kd[1] = kd[0]; val[0] = 5000; kd[0] = K_PCT; }   /* "at top": the y keyword alone */
                else if (ni == 2 && vfirst) { int tv = val[0], tk = kd[0]; val[0] = val[1]; kd[0] = kd[1]; val[1] = tv; kd[1] = tk; }   /* "top left" */
                for (int i = 0; i < 2; i++) { g->px[i] = val[i]; g->pk[i] = kd[i]; }
            }
        } else {
            char *sp = 0;                                          /* a trailing position: the last top-level space */
            int dd = 0;
            for (char *t = q; *t; t++) { if (*t == '(') dd++; else if (*t == ')') dd--; else if (*t == ' ' && !dd) sp = t; }
            int kk = 0, vv = 0;
            if (sp && (isdigit((unsigned char)sp[1]) || sp[1] == '.' || sp[1] == '-') && gs_len(sp + 1, &kk, &vv)) *sp = 0; else kk = 0;
            color_t c;
            if (css_color(q, &c, 0)) { g->c[g->n] = c; g->k[g->n] = kk; g->v[g->n] = vv; g->n++; }
        }
        first = 0;
        p = e;
        if (*p == ')' && !d) break;
        if (*p == ',') p++;
    }
    return g->n >= 2;
}
static int gs_axis(const struct gspec *g) {                  /* plain horizontal/vertical linear: the rect path paints it */
    if (g->kind || g->cdx) return 0;
    int a = (int)(g->ang * 57.29578f + 0.5f);
    return a % 90 == 0;
}
static color_t gs_mix(color_t a, color_t b, int t) {
    if (!(a >> 24)) a = b & 0xFFFFFF; else if (!(b >> 24)) b = a & 0xFFFFFF;
    uint32_t r = 0;
    for (int s = 0; s <= 24; s += 8) r |= (uint32_t)((((a >> s) & 255) * (255 - t) + ((b >> s) & 255) * t + 127) / 255) << s;
    return r;
}
/* rows [y0, y1) of the w x h box into out (w ARGB pixels per row); colours are evaluated once per step x step block */
void grad_render(const char *s, int w, int h, int rad, int y0, int y1, uint32_t *out, int step) {
    struct gspec g;
    if (!gs_parse(s, &g) || w <= 0 || h <= 0) return;
    float cx = g.pk[0] == K_PCT ? w * g.px[0] / 10000.f : g.px[0], cy = g.pk[1] == K_PCT ? h * g.px[1] / 10000.f : g.px[1];
    float len = 1, dx = 0, dy = 0, rx = 1, ry = 1;
    if (g.kind == 0) {
        if (g.cdx) { dx = g.cdx * (float)h; dy = g.cdy * (float)w; float m = sqrtf(dx * dx + dy * dy); dx /= m; dy /= m; }
        else { dx = sinf(g.ang); dy = -sinf(g.ang + 1.5707963f); }
        len = fabsf(w * dx) + fabsf(h * dy);
    } else if (g.kind == 1) {
        float mnx = gmin_(cx, w - cx), mny = gmin_(cy, h - cy), mxx = gmax_(cx, w - cx), mxy = gmax_(cy, h - cy);
        if (g.size == 4) {
            rx = g.ek[0] == K_PCT ? w * g.ex[0] / 10000.f : g.ex[0];
            ry = g.ek[1] == K_PCT ? h * g.ex[1] / 10000.f : g.ex[1];
        } else if (g.circle) {
            rx = g.size == 1 ? gmin_(mnx, mny) : g.size == 2 ? gmax_(mxx, mxy) : g.size == 3 ? sqrtf(mnx * mnx + mny * mny) : sqrtf(mxx * mxx + mxy * mxy);
            ry = rx;
        } else {
            int near = g.size == 1 || g.size == 3;
            rx = near ? mnx : mxx; ry = near ? mny : mxy;
            if (g.size == 0 || g.size == 3) { rx *= 1.41421356f; ry *= 1.41421356f; }
        }
        if (rx < 0.5f) rx = 0.5f;
        if (ry < 0.5f) ry = 0.5f;
        len = rx;
    }
    float *t = g.t; int n = g.n;
    for (int i = 0; i < n; i++) t[i] = g.k[i] == 1 ? g.v[i] / 10000.f : g.k[i] == 2 ? g.v[i] / len : g.k[i] == 3 ? g.v[i] / 36000.f : -1.f;
    if (t[0] < 0) t[0] = 0;
    if (t[n - 1] < 0) t[n - 1] = 1;
    for (int i = 1; i < n; i++) if (t[i] >= 0 && t[i] < t[i - 1]) t[i] = t[i - 1];
    for (int i = 1; i < n - 1; i++) if (t[i] < 0) {
        int a = i - 1, z = i + 1;
        while (t[z] < 0) z++;
        for (int k = i; k < z; k++) t[k] = t[a] + (t[z] - t[a]) * (k - a) / (z - a);
    }
    if (step < 1) step = 1;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = out + (size_t)(y - y0) * w;
        if (step > 1 && y > y0 && (y - y0) % step) { memcpy(row, row - w, (size_t)w * 4); continue; }
        for (int x = 0; x < w; x++) {
            if (x % step) { row[x] = row[x - 1]; continue; }
            float px = x + step * 0.5f, py = y + step * 0.5f, u;
            if (g.kind == 0) u = ((px - w * 0.5f) * dx + (py - h * 0.5f) * dy) / len + 0.5f;
            else if (g.kind == 1) { float ax = (px - cx) / rx, ay = (py - cy) / ry; u = sqrtf(ax * ax + ay * ay); }
            else { u = (atan2f(px - cx, -(py - cy)) - g.ang) / 6.28318531f; u -= floorf(u); }
            int i = 0;
            while (i < n - 2 && u >= t[i + 1]) i++;
            if (u <= t[0]) row[x] = g.c[0];
            else if (u >= t[n - 1]) row[x] = g.c[n - 1];
            else { float sp = t[i + 1] - t[i]; row[x] = gs_mix(g.c[i], g.c[i + 1], sp > 1e-6f ? (int)((u - t[i]) / sp * 255 + 0.5f) : 255); }
        }
    }
    int R = rad > w / 2 ? w / 2 : rad;
    if (R > h / 2) R = h / 2;
    for (int y = y0; y < y1 && R > 0; y++) {                       /* rounded corners: anti-aliased coverage, only in the corner squares */
        int ccy = y < R ? R : y >= h - R ? h - R : -1;
        if (ccy < 0) continue;
        uint32_t *row = out + (size_t)(y - y0) * w;
        for (int k = 0; k < 2 * R; k++) {
            int x = k < R ? k : w - 2 * R + k, ccx = k < R ? R : w - R;
            float ex = x + 0.5f - ccx, ey = y + 0.5f - ccy, dd = sqrtf(ex * ex + ey * ey) - R + 0.5f;
            if (dd > 0) row[x] = (row[x] & 0xFFFFFF) | (dd >= 1 ? 0u : (uint32_t)((1 - dd) * (row[x] >> 24))) << 24;
        }
    }
}

/* linear-gradient() axis path: stops painted as consecutive two-colour rects */
#define GSTOPS 12
struct gstop { color_t c; int kind, v; };                  /* kind 0 unset, 1 percent (x100), 2 px */
static int grad_parse(const char *s, int *horiz, int *rev, struct gstop *st) {
    const char *p = strstr(s, "linear-gradient(");
    if (!p) return 0;
    p += 16;
    *horiz = 0; *rev = 0;
    int n = 0, d = 0, first = 1;
    char a[160];
    while (*p && n < GSTOPS) {
        const char *e = p;
        for (; *e && (d || (*e != ',' && *e != ')')); e++) { if (*e == '(') d++; if (*e == ')') d--; }
        int l = imin_((int)(e - p), 159);
        memcpy(a, p, l); a[l] = 0;
        char *q = a;
        while (*q == ' ') q++;
        int isdir = !strncmp(q, "to ", 3) || (first && (isdigit((unsigned char)*q) || *q == '-' || *q == '.') && strstr(q, "deg"));
        if (first && isdir) {
            if (!strncmp(q, "to ", 3)) {
                int up = !!strstr(q, "top"), lf = !!strstr(q, "left"), rt = !!strstr(q, "right"), dn = !!strstr(q, "bottom");
                if ((lf || rt) && !up && !dn) { *horiz = 1; *rev = lf; } else *rev = up && !dn;
            } else {
                int deg = ((int)atof(q) % 360 + 360) % 360;
                if (deg >= 45 && deg < 135) *horiz = 1;
                else if (deg >= 225 && deg < 315) { *horiz = 1; *rev = 1; }
                else *rev = deg < 45 || deg >= 315;
            }
        } else {
            char *sp = 0;
            int dd = 0;
            for (char *t = q; *t; t++) { if (*t == '(') dd++; else if (*t == ')') dd--; else if (*t == ' ' && !dd) sp = t; }
            st[n].kind = 0; st[n].v = 0;
            if (sp && (isdigit((unsigned char)sp[1]) || sp[1] == '.' || sp[1] == '-')) {
                st[n].kind = strchr(sp, '%') ? 1 : 2;
                st[n].v = (int)(atof(sp + 1) * (st[n].kind == 1 ? 100 : 1));
                *sp = 0;
            }
            color_t c;
            if (css_color(q, &c, 0)) st[n++].c = c;
        }
        first = 0;
        p = e;
        if (*p == ')' && !d) break;
        if (*p == ',') p++;
    }
    return n >= 2 ? n : 0;
}

static void paint_grad(struct bx *b, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    struct gspec gs;
    if (!gs_parse(css_str(b->grd - 1), &gs)) return;
    if (b->rad > 0 || !gs_axis(&gs)) {                              /* per pixel at draw time; px = the gradient's css_str id */
        int n0 = D->ndc;
        emit_decor(x, y, w, h, 0xFF000001, b->rad, 0);
        if (D->ndc == n0 + 1) { struct decor *r = &D->dc[n0]; r->gdir = 3; r->px = b->grd - 1; }
        return;
    }
    struct gstop st[GSTOPS];
    int horiz, rev, n = grad_parse(css_str(b->grd - 1), &horiz, &rev, st);
    if (n < 2) return;
    int len = horiz ? w : h, pos[GSTOPS];
    for (int i = 0; i < n; i++) pos[i] = st[i].kind == 1 ? len * st[i].v / 10000 : st[i].kind == 2 ? st[i].v : -1;
    if (pos[0] < 0) pos[0] = 0;
    if (pos[n - 1] < 0) pos[n - 1] = len;
    for (int i = 1; i < n; i++) if (pos[i] >= 0 && pos[i] < pos[i - 1]) pos[i] = pos[i - 1];
    for (int i = 1; i < n - 1; i++) if (pos[i] < 0) {              /* unset stops spread evenly between their neighbours */
        int a = i - 1, z = i + 1;
        while (pos[z] < 0) z++;
        for (int k = i; k < z; k++) pos[k] = pos[a] + (pos[z] - pos[a]) * (k - a) / (z - a);
    }
    if (rev) {                                                      /* "to top"/"to left": run the stops backwards */
        for (int i = 0; i < n / 2; i++) { struct gstop t = st[i]; st[i] = st[n - 1 - i]; st[n - 1 - i] = t; }
        for (int i = 0; i < n / 2; i++) { int t = pos[i]; pos[i] = len - pos[n - 1 - i]; pos[n - 1 - i] = len - t; }
        if (n & 1) pos[n / 2] = len - pos[n / 2];
    }
    if (pos[0] > 0) { if (horiz) emit_decor(x, y, pos[0], h, st[0].c, 0, 0); else emit_decor(x, y, w, pos[0], st[0].c, 0, 0); }
    for (int i = 0; i + 1 < n; i++) {
        int a = imin_(pos[i], len), z = imin_(pos[i + 1], len);
        if (z <= a) continue;
        int n0 = D->ndc;
        color_t c0 = st[i].c ? st[i].c : 1;                          /* emit_decor drops colour 0 */
        if (horiz) emit_decor(x + a, y, z - a, h, c0, 0, 0); else emit_decor(x, y + a, w, z - a, c0, 0, 0);
        if (D->ndc == n0 + 1) { struct decor *r = &D->dc[n0]; r->g1 = st[i + 1].c ? st[i + 1].c : 1; r->gdir = horiz ? 2 : 1; }
    }
    if (pos[n - 1] < len) { if (horiz) emit_decor(x + pos[n - 1], y, len - pos[n - 1], h, st[n - 1].c, 0, 0); else emit_decor(x, y + pos[n - 1], w, len - pos[n - 1], st[n - 1].c, 0, 0); }
}

static void paint_bg(struct bx *b, int ox, int oy) {
    int x = b->x + ox, y = b->y + oy, w = b->w, h = b->h;
    if (b->shc && w > 0 && h > 0) {                       /* box-shadow: a few growing translucent rounded rects stand in for the blur */
        int bl = b->sh[2] < 0 ? 0 : b->sh[2], n = bl > 12 ? 4 : bl > 4 ? 3 : bl ? 2 : 1;
        uint32_t a = (b->shc >> 24) * (bl ? 2 : 3) / (n * 3 / 2 + 1);
        color_t c = (b->shc & 0xFFFFFF) | (a > 255 ? 255u : a) << 24;
        for (int i = n - 1; i >= 0; i--) {
            int g = b->sh[3] + (n > 1 ? bl * i / (n - 1) : 0) - (n > 1 ? bl / 4 : 0);
            int sx = x + b->sh[0] - g, sy = y + b->sh[1] - g, sw = w + 2 * g, sh = h + 2 * g;
            if ((b->bg >> 24) == 0xFF) { emit_decor(sx, sy, sw, sh, c, b->rad + g, 0); continue; }
            /* a transparent box shows what's behind it: the shadow only exists outside the border box (4 strips around it) */
            int top = imin_(imax_(y - sy, 0), sh), bot = imin_(imax_(sy + sh - (y + h), 0), sh), mid = sh - top - bot;
            int l = imin_(imax_(x - sx, 0), sw), r = imin_(imax_(sx + sw - (x + w), 0), sw);
            emit_decor(sx, sy, sw, top, c, 0, 0);
            emit_decor(sx, sy + sh - bot, sw, bot, c, 0, 0);
            if (mid > 0) { emit_decor(sx, sy + top, l, mid, c, 0, 0); emit_decor(sx + sw - r, sy + top, r, mid, c, 0, 0); }
        }
    }
    if (b->bg && !b->msk) emit_decor(x, y, w, h, b->opacity == 255 ? b->bg : (b->bg & 0xFFFFFF) | (b->bg >> 24) * b->opacity / 255 << 24, b->rad, 0);   /* opacity: only the box's own paint is faded (no group compositing), enough for translucent overlays */
    if (b->bgi && w > 0 && h > 0) {                      /* background-image, placed in the padding box */
        int l = b->bw[3], t = b->bw[0];
        color_t under = b->msk || (b->bg >> 24) == 0xFF ? b->bg : 1;       /* the box's own colour: what a transparent picture is drawn over */
        emit_decor(x + l, y + t, w - l - b->bw[1], h - t - b->bw[2], under, 0, 0);
        struct decor *r = &D->dc[D->ndc - 1];
        if (D->ndc && r->it == D->nit && r->col == under && !r->img) {
            r->img = b->bgi; r->rep = b->brep; r->kx = b->bkx; r->ky = b->bky; r->kw = b->bkw; r->kh = b->bkh;
            r->px = b->bpx; r->py = b->bpy; r->sw = b->bsw; r->sh = b->bsh; r->msk = b->msk; r->opq = 255 - b->opacity;
        }
    }
    if (b->grd && w > 0 && h > 0) paint_grad(b, x + b->bw[3], y + b->bw[0], w - b->bw[1] - b->bw[3], h - b->bw[0] - b->bw[2]);
    if (b->outw && b->outc >> 24) emit_decor(x - b->outo - b->outw, y - b->outo - b->outw, w + 2 * (b->outo + b->outw), h + 2 * (b->outo + b->outw), b->outc, b->rad, b->outw);
    int bt = b->bw[0], br = b->bw[1], bb = b->bw[2], bl = b->bw[3];
    if (!(bt | br | bb | bl)) return;
    if (b->rad > 0 && bt == br && br == bb && bb == bl && b->bc[0] == b->bc[1] && b->bc[1] == b->bc[2] && b->bc[2] == b->bc[3]) {
        emit_decor(x, y, w, h, b->bc[0], b->rad, bt);
        return;
    }
    emit_decor(x, y, w, bt, b->bc[0], 0, 0);
    emit_decor(x, y + h - bb, w, bb, b->bc[2], 0, 0);
    emit_decor(x, y + bt, bl, h - bt - bb, b->bc[3], 0, 0);
    emit_decor(x + w - br, y + bt, br, h - bt - bb, b->bc[1], 0, 0);
}

static void rel_offset(const struct bx *b, int *dx, int *dy) {
    int pw = b->par >= 0 ? B[b->par].w : rootw, ph = b->par >= 0 ? B[b->par].h : VIEWH;
    *dx = b->k[L_LEFT] != K_AUTO ? rl(b, L_LEFT, pw) : -rl(b, L_RIGHT, pw);
    *dy = b->k[L_TOP] != K_AUTO ? rl(b, L_TOP, ph) : -rl(b, L_BOTTOM, ph);
}

static void paint_box(int bi, int ox, int oy);

static void defer(int bi) {
    if (npe == pecap) {
        int nc = pecap ? pecap * 2 : 64;
        struct pend *p = realloc(PE, nc * sizeof *p);
        if (!p) return;
        PE = p; pecap = nc;
    }
    PE[npe] = (struct pend){ bi, B[bi].zset ? B[bi].zi : 0, npe };
    npe++;
}

static int first_text_y(const struct bx *b, int *y) {
    for (int i = 0; i < b->nfr; i++) if (b->fr[i].t == F_TEXTRUN) { *y = b->fr[i].y; return 1; }
    for (int c = b->first; c >= 0; c = B[c].next) {
        if (B[c].flt || oof(&B[c])) continue;
        if (B[c].inner == IN_FLOW || B[c].inner == IN_TCELL || B[c].inner == IN_INLINE) if (first_text_y(&B[c], y)) return 1;
        if (B[c].inner == IN_IMG) { *y = B[c].y; return 1; }
    }
    return 0;
}

static void paint_marker(struct bx *b, int ox, int oy) {
    if (b->mark < 0) return;
    char m[24] = "\xE2\x80\xA2";
    if (b->lt >= LT_DECIMAL && b->lt != LT_NONE) { int n = css_counter_format(b->lt, b->mark, m, sizeof m - 2); m[n] = '.'; m[n + 1] = 0; }
    const struct font *f = FT[b->font];
    int w = gfx_text_width(f, m), y;
    if (!first_text_y(b, &y)) y = b->y + b->bw[0] + b->p[0];
    struct item *it = emit_item();
    it->x = b->x + b->bw[3] + b->p[3] - 8 - w + ox; if (it->x < 2) it->x = 2;
    it->y = y + oy; it->w = w; it->h = f->height; it->font = b->font; it->col = b->markc ? b->markc : mix(D->fg, D->bg, 130);
    it->len = strlen(m); it->off = arena(m, it->len);
    if (it->h > D->maxh) D->maxh = it->h;
}

static void paint_frags(struct bx *b, int ox, int oy) {
    for (int i = 0; i < b->nfr; i++) {
        const struct frag *f = &b->fr[i];
        if (f->t == F_BG) {
            struct bx *ob = &B[f->box];
            int x = f->x + ox, y = f->y + oy;
            if (ob->bg) emit_decor(x, y, f->w, f->h, ob->bg, ob->rad, 0);
            if (ob->bw[0]) emit_decor(x, y, f->w, ob->bw[0], ob->bc[0], 0, 0);
            if (ob->bw[2]) emit_decor(x, y + f->h - ob->bw[2], f->w, ob->bw[2], ob->bc[2], 0, 0);
            if (ob->bw[3] && (f->edges & 1)) emit_decor(x, y, ob->bw[3], f->h, ob->bc[3], 0, 0);
            if (ob->bw[1] && (f->edges & 2)) emit_decor(x + f->w - ob->bw[1], y, ob->bw[1], f->h, ob->bc[1], 0, 0);
        }
    }
    for (int i = 0; i < b->nfr; i++) {
        const struct frag *f = &b->fr[i];
        if (f->t == F_BOX) { paint_box(f->box, ox, oy); continue; }
        if (f->t != F_TEXTRUN || f->box || !f->len) continue;          /* box: visibility */
        struct item *it = emit_item();
        it->x = f->x + ox; it->y = f->y + oy; it->w = f->w; it->h = f->h; it->font = f->font; it->flags = f->flags;
        it->col = f->col; it->link = f->link; it->off = f->off; it->len = f->len; it->node = f->node;
        if (it->h > D->maxh) D->maxh = it->h;
    }
}

static void paint_replaced(struct bx *b, int ox, int oy) {
    int x = b->x + b->bw[3] + b->p[3] + ox, y = b->y + b->bw[0] + b->p[0] + oy;
    int w = b->w - hedge(b), h = b->h - vedge(b);
    if (b->inner == IN_IMG) {
        struct item *it = emit_item();
        it->x = x; it->y = y; it->w = w; it->h = h; it->flags = FL_IMG; it->ref = b->ref; it->node = b->node; it->fit = b->objfit;
        if (b->tlen) { it->len = imin_(b->tlen, 200); it->off = arena(hd_str(b->toff), it->len); }
        if (it->h > D->maxh) D->maxh = it->h;
    } else {
        struct item *it = emit_item();
        it->x = x; it->y = y; it->w = w; it->h = h; it->flags = FL_CTL; it->ref = b->ref; it->node = b->node;
        if (b->ref >= 0 && b->ref < D->nct) D->ct[b->ref].item = D->nit - 1;
        if (it->h > D->maxh) D->maxh = it->h;
    }
}

static int cmp_pend(const void *a, const void *b) {
    const struct pend *x = a, *y = b;
    return x->z != y->z ? (x->z > y->z ? 1 : -1) : x->seq - y->seq;
}

static void paint_neg(int bi, int ox, int oy) {          /* positioned boxes with negative z-index belong behind the context's own content */
    for (int c = B[bi].first; c >= 0; c = B[c].next) {
        struct bx *x = &B[c];
        if (x->pos != POS_STATIC && x->pos != POS_STICKY) {
            if (x->zset && x->zi < 0) paint_box(c, ox, oy);
            continue;
        }
        paint_neg(c, ox, oy);
    }
}

/* children that aren't inline content of the box's own lines: blocks, floats, positioned boxes (recursing through inline boxes) */
static void paint_kids(int bi, int ox, int oy, int pass) {
    for (int c = B[bi].first; c >= 0; c = B[c].next) {
        struct bx *x = &B[c];
        if (x->inner == IN_TEXT || x->inner == IN_BR) continue;
        if (x->pos != POS_STATIC && x->pos != POS_STICKY) {
            if (pass == 0 && !(x->zset && x->zi < 0)) defer(c);
            continue;
        }
        if (x->flt) { if (pass == 1) paint_box(c, ox, oy); continue; }
        if (x->inner == IN_INLINE) { paint_kids(c, ox, oy, pass); continue; }
        if (x->outer == OUT_INLINE) continue;               /* atomic inline: painted with its line */
        if (pass == 0) paint_box(c, ox, oy);
    }
}

static int in_stk;
static void paint_box(int bi, int ox, int oy) {
    struct bx *b = &B[bi];
    if (b->opacity == 0 || (b->inner == IN_CTL && b->ref < 0)) return;
    int positioned = b->pos != POS_STATIC, mark = npe;
    struct stick *sg = 0;
    if (b->anm && !in_stk && D->nsk < MAXSTK && !sg) {                   /* a CSS animation: what the box emits is shifted at draw time, like a sticky one */
        sg = &D->sk[D->nsk++];
        *sg = (struct stick){ .i0 = D->nit, .d0 = D->ndc, .c0 = imax_(D->ncl, 1), .y0 = b->y + oy, .h = b->h, .lim = 1 << 30, .akf = b->anm, .adur = b->adur, .adel = b->adel, .ait = b->ait, .adr = b->adr, .bw = b->w };
        in_stk = 1;
    }
    int fixd = b->pos == POS_FIXED;                                      /* fixed = sticky with top = where it sits at scroll 0 and no parent limit */
    if ((fixd || (b->pos == POS_STICKY && b->k[L_TOP] != K_AUTO)) && !in_stk && D->nsk < MAXSTK) {       /* moved at draw time: remember what it emits */
        int ph = b->par >= 0 ? B[b->par].h : VIEWH;
        sg = &D->sk[D->nsk++];
        struct bx *pp = b->par >= 0 ? &B[b->par] : 0;
        *sg = (struct stick){ .i0 = D->nit, .d0 = D->ndc, .c0 = imax_(D->ncl, 1), .y0 = b->y + oy, .h = b->h, .top = fixd ? b->y + oy : rl(b, L_TOP, ph),
                              .lim = pp && !fixd ? pp->y + oy + pp->h - pp->bw[2] - pp->p[2] : 1 << 30 };
        in_stk = 1;
    }
    if (b->pos == POS_REL) { int dx, dy; rel_offset(b, &dx, &dy); ox += dx; oy += dy; }
    if (b->txv || b->tyv || b->txp || b->typ) { ox += b->txv + b->w * b->txp / 100; oy += b->tyv + b->h * b->typ / 100; }
    if (b->aname) anchor_add(b->aname, b->y + oy);
    int prev = cur_clip, hidden = b->vis;
    if (!hidden) paint_bg(b, ox, oy);
    if (b->ovf) clip_push(b->x + b->bw[3] + ox, b->y + b->bw[0] + oy, b->w - b->bw[1] - b->bw[3], b->h - b->bw[0] - b->bw[2]);
    if (b->hasneg) {                                          /* behind our own content */
        int save = cur_clip;
        cur_clip = prev;
        paint_neg(bi, ox, oy);
        cur_clip = save;
    }
    if (!hidden && b->li) paint_marker(b, ox, oy);
    if (b->inner == IN_IMG || b->inner == IN_CTL) { if (!hidden) paint_replaced(b, ox, oy); }
    else {
        paint_kids(bi, ox, oy, 0);
        paint_kids(bi, ox, oy, 1);
        paint_frags(b, ox, oy);
    }
    if (positioned || b->par < 0) {                           /* this box is a context: its positioned descendants, back to front */
        int n = npe;
        qsort(PE + mark, n - mark, sizeof *PE, cmp_pend);
        for (int i = mark; i < n; i++) {
            int save = npe;
            npe = n;
            paint_box(PE[i].b, ox, oy);
            npe = save;
        }
        npe = mark;
    }
    cur_clip = prev;
    if (sg) { sg->i1 = D->nit; sg->d1 = D->ndc; sg->c1 = imax_(D->ncl, 1); in_stk = 0; }
}

/* ---------- build: HD + CSS -> boxes ---------- */
#define PM(p) (1u << (p))
static color_t col_em, col_dim;
static int nord, curform_;

struct inh { uint8_t tt, font, flags, nul, ws, align, nomark, vis, valign, icon; color_t col; int link, lh, node; };
struct bctx { int incenter, cb, curform, tpad, tborder, tcollapse, nested, js; int *cnt; uint8_t tva; };
static struct el E[128];
static int nel;

static int dark(color_t c) { return (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10 < 110; }
static int font_bold(int f) { return f == F_UI ? F_UIB : f == F_MONO ? F_MONOB : f; }
static int font_plain(int f) { return f == F_UIB ? F_UI : f == F_MONOB ? F_MONO : f; }
static int fnt_mono(int f) { return f == F_UIB || f == F_MONOB ? F_MONOB : F_MONO; }

static void lens_init(struct bx *S) {
    for (int i = 0; i < NL; i++) { S->k[i] = i < 8 ? K_PX : K_AUTO; S->v[i] = 0; }
}

static int link_add(const char *href) {
    struct doc *d = D;
    if (d->nlink >= 20000) return -1;
    if (d->nlink == d->linkcap) {
        int nc = d->linkcap ? d->linkcap * 2 : 64;
        char **nl = realloc(d->link, nc * sizeof *nl);
        if (!nl) return -1;
        d->link = nl; d->linkcap = nc;
    }
    char *s = strdup(href);
    if (!s) return -1;
    d->link[d->nlink] = s;
    return d->nlink++;
}

static void *grow_(void *p, int *cap, size_t sz) {
    int nc = *cap ? *cap * 2 : 16;
    void *np = realloc(p, nc * sz);
    if (!np) return p;
    *cap = nc;
    return np;
}
#define GROW(p, n, cap) do { if ((n) >= (cap)) (p) = grow_((p), &(cap), sizeof *(p)); } while (0)

static void anon_style(struct bx *A, const struct bx *P) {      /* an anonymous box inherits what text inherits */
    A->font = P->font; A->flags = P->flags; A->nul = P->nul; A->ws = P->ws; A->align = P->align; A->col = P->col;
    A->link = P->link; A->node = P->node; A->lh = P->lh; A->vis = P->vis; A->anon = 1;
}

static int is_ws_text(const struct bx *b) {
    if (b->inner != IN_TEXT || b->ws == 2 || b->ws == 3) return 0;
    const char *s = hd_str(b->toff);
    for (int i = 0; i < b->tlen; i++) if (!(s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r' || s[i] == '\f')) return 0;
    return 1;
}

/* move children [a, e) (e exclusive, -1 = to the end) of p into a new anonymous box of the given kind, placed where a was */
static int wrap_range(int p, int prev, int a, int e, int outer, int inner) {
    int w = bx_new();
    if (w < 0) return -1;
    B[w].outer = outer; B[w].inner = inner;
    lens_init(&B[w]);
    anon_style(&B[w], &B[p]);
    if (inner == IN_TCELL) { B[w].cs = B[w].rs = 1; }
    B[w].par = p;
    B[w].next = e;
    if (prev >= 0) B[prev].next = w; else B[p].first = w;
    if (e < 0) B[p].last = w;
    int last = -1;
    for (int c = a; c >= 0 && c != e; c = B[c].next) { B[c].par = w; last = c; }
    B[w].first = a; B[w].last = last;
    if (last >= 0) B[last].next = -1;
    return w;
}

static int out_of_flow_or_float(const struct bx *b) { return oof(b) || b->flt; }

/* drop q (whose previous sibling is `prev`, -1 = first) from p's child list */
static void unlink_after(int p, int prev, int q) {
    int nx = B[q].next;
    if (prev >= 0) B[prev].next = nx; else B[p].first = nx;
    if (B[p].last == q) B[p].last = prev;
    B[q].par = B[q].next = -1;
}

/* the blank text boxes of the run [c, e) go; returns the last kept box before e (or prev) */
static int drop_blanks(int p, int prev, int c, int e) {
    int n;
    for (int q = c; q != e; q = n) {
        n = B[q].next;
        if (is_ws_text(&B[q])) unlink_after(p, prev, q); else prev = q;
    }
    return prev;
}

/* a container holds blocks or inline content, never both: inline runs among blocks get anonymous blocks */
static void fix_flow(int p, int force) {
    int anyblk = force;
    for (int c = B[p].first; c >= 0 && !anyblk; c = B[c].next) if (in_flow_block(&B[c])) anyblk = 1;
    if (!anyblk) return;
    int c = B[p].first, prev = -1;
    while (c >= 0) {
        if (in_flow_block(&B[c])) { prev = c; c = B[c].next; continue; }
        int e = c, real = 0;
        while (e >= 0 && !in_flow_block(&B[e])) { if (!out_of_flow_or_float(&B[e]) && !is_ws_text(&B[e])) real = 1; e = B[e].next; }
        if (!real) { prev = drop_blanks(p, prev, c, e); c = e; continue; }
        prev = wrap_range(p, prev, c, e, OUT_BLOCK, IN_FLOW);
        c = e;
    }
}

static void fix_table_kids(int p, int want_row, int want_cell) {   /* rows under a table/group, cells under a row */
    int c = B[p].first, prev = -1;
    while (c >= 0) {
        int ok = want_cell ? B[c].inner == IN_TCELL : B[c].inner == IN_TROW || (want_row == 2 && (B[c].inner == IN_CAP || B[c].inner == IN_TGROUP));
        if (ok) { prev = c; c = B[c].next; continue; }
        int e = c, real = 0;
        for (;;) {
            if (e < 0) break;
            int eok = want_cell ? B[e].inner == IN_TCELL : B[e].inner == IN_TROW || (want_row == 2 && (B[e].inner == IN_CAP || B[e].inner == IN_TGROUP));
            if (eok) break;
            if (!is_ws_text(&B[e]) && !oof(&B[e])) real = 1;
            e = B[e].next;
        }
        if (!real) { prev = drop_blanks(p, prev, c, e); c = e; continue; }
        if (want_cell) {
            int w = wrap_range(p, prev, c, e, OUT_BLOCK, IN_TCELL);
            if (w >= 0) fix_flow(w, 0);
            prev = w;
        } else {                                              /* stray cells or content under a table: an anonymous row (and cell) */
            int w = wrap_range(p, prev, c, e, OUT_BLOCK, IN_TROW);
            if (w >= 0) {
                int needcell = 0;
                for (int q = B[w].first; q >= 0; q = B[q].next) if (B[q].inner != IN_TCELL) needcell = 1;
                if (needcell) fix_table_kids(w, 0, 1);
            }
            prev = w;
        }
        c = e;
    }
}

static void fix_items(int p) {                                  /* flex / grid: every in-flow child is a block item */
    int c = B[p].first, prev = -1;
    for (int q = c; q >= 0; q = B[q].next) {
        struct bx *x = &B[q];
        if (!out_of_flow_or_float(x) && x->outer == OUT_INLINE && x->inner != IN_TEXT && x->inner != IN_BR) { x->outer = OUT_BLOCK; if (x->inner == IN_INLINE) x->inner = IN_FLOW; }   /* items (inline elements too) are blockified */
    }
    while (c >= 0) {
        struct bx *x = &B[c];
        if (x->flt || oof(x) || x->outer == OUT_BLOCK) { prev = c; c = B[c].next; continue; }
        int e = c, real = 0;
        while (e >= 0 && !(B[e].flt || oof(&B[e]) || B[e].outer == OUT_BLOCK)) { if (!is_ws_text(&B[e])) real = 1; e = B[e].next; }
        if (!real) { int n; for (int q = c; q != e; q = n) { n = B[q].next; unlink_after(p, prev, q); } c = e; continue; }
        prev = wrap_range(p, prev, c, e, OUT_BLOCK, IN_FLOW);
        c = e;
    }
}

static void fixup(int bi) {
    switch (B[bi].inner) {
    case IN_TABLE: fix_table_kids(bi, 2, 0); break;
    case IN_TGROUP: fix_table_kids(bi, 1, 0); break;
    case IN_TROW: fix_table_kids(bi, 0, 1); break;
    case IN_FLEX: case IN_GRID: fix_items(bi); break;
    case IN_FLOW: case IN_TCELL: case IN_CAP: fix_flow(bi, 0); break;
    }
}

/* ---------- element -> style ---------- */
static const char *ATTR(int dn, const char *n) { return hd_attr(dn, n); }

static int parse_color(const char *s, color_t *out) { return css_color(s, out, 1) && *out; }

static void el_fill(struct el *e, int dn, int first) {
    const char *v;
    memset(e, 0, sizeof *e);
    strlcpy(e->name, hd_str(DN(dn).nm), sizeof e->name);
    if ((v = ATTR(dn, "id"))) strlcpy(e->id, v, sizeof e->id);
    if ((v = ATTR(dn, "class"))) strlcpy(e->cls, v, sizeof e->cls);
    e->js = (v = ATTR(dn, "data-vjs")) ? atoi(v) : 0;
    e->first = first;
    if (css_attrs()) {
        int k = 0;
        for (int i = 0; i < DN(dn).na; i++) {
            const char *n = hd_str(HD.a[DN(dn).a0 + i].n), *val = hd_str(HD.a[DN(dn).a0 + i].v);
            int ln = strlen(n), lv = strlen(val);
            if (!strcmp(n, "style") || k + ln + lv + 3 >= (int)sizeof e->at) continue;
            e->at[k++] = 1; memcpy(e->at + k, n, ln); k += ln;
            e->at[k++] = 2; memcpy(e->at + k, val, lv); k += lv;
        }
    }
}

static int block_tag(int t) {
    switch (t) {
    case T_HTML: case T_BODY: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_P: case T_DIV: case T_CENTER:
    case T_UL: case T_OL: case T_LI: case T_DL: case T_DT: case T_DD: case T_BQ: case T_PRE: case T_FORM: case T_HR: case T_LEGEND:
    case T_TABLE: case T_TGROUP: case T_TR: case T_TD: case T_TH: case T_CAP: return 1;
    }
    return 0;
}

static void set_px(struct bx *S, int i, int v) { S->k[i] = K_PX; S->v[i] = v; }

/* a computed length -> bx lens: auto/none/keywords -> auto, % stays %, everything else px (calc() against the viewport) */
static void lenset(struct bx *S, int li, const struct cprops *cp, int prop) {
    int k = cp->k[prop];
    if (k == K_AUTO || k == K_NONE || k == K_FIT || k == K_MINC || k == K_MAXC) { S->k[li] = K_AUTO; S->v[li] = 0; }
    else if (k == K_PCT) { S->k[li] = K_PCT; S->v[li] = cp->v[prop] / 100; }
    else { S->k[li] = K_PX; S->v[li] = k == K_PX ? cp->v[prop] : css_len(cp, prop, rootw); }
}

/* ---------- css_dom: the node tree seen by css_compute ---------- */
static int cd_parent(int n) { int p = DN(n).par; return p > 0 && DN(p).type == 1 ? p : 0; }
static int cd_prev(int n) { for (int c = DN(n).prev; c >= 0; c = DN(c).prev) if (DN(c).type == 1) return c; return 0; }
static int cd_next(int n) { for (int c = DN(n).next; c >= 0; c = DN(c).next) if (DN(c).type == 1) return c; return 0; }
static int cd_first(int n) { for (int c = DN(n).first; c >= 0; c = DN(c).next) if (DN(c).type == 1) return c; return 0; }
static const char *cd_tag(int n) { return hd_str(DN(n).nm); }
static const char *cd_attr(int n, const char *a) { return hd_attr(n, a); }
static int cd_state(int n) {
    int t = DN(n).tag;
    return (t == T_A || !strcmp(hd_str(DN(n).nm), "area")) && hd_attr(n, "href") ? CS_LINK : 0;
}
static int cd_has_text(int n) { for (int c = DN(n).first; c >= 0; c = DN(c).next) if (DN(c).type == 3 && DN(c).tn) return 1; return 0; }
static const struct css_dom CD = { cd_parent, cd_prev, cd_next, cd_first, cd_tag, cd_attr, cd_state, cd_has_text };

static int map_just(int v) { return v == AL_END ? 1 : v == AL_CENTER ? 2 : v == AL_BETWEEN ? 3 : v == AL_AROUND ? 4 : v == AL_EVENLY ? 5 : 0; }
static int map_align(int v) { return v == AL_START ? 1 : v == AL_END ? 2 : v == AL_CENTER ? 3 : v == AL_BASELINE ? 4 : 0; }

/* the presentational attributes of the old web, applied only where CSS left the user-agent value (author CSS wins) */
static void hints(int dn, int tag, struct bx *S, struct inh *ni, struct bctx *cx, struct bctx *cx2) {
    const char *v;
    if ((v = ATTR(dn, "align")) && (tag == T_P || tag == T_DIV || (tag >= T_H1 && tag <= T_H6) || tag == T_FORM || tag == T_TD || tag == T_TH || tag == T_TR || tag == T_CAP || tag == T_TGROUP || tag == T_IMG || tag == T_HR)) {
        if (tag == T_TABLE || tag == T_IMG || tag == T_HR) { }
        else if (ni->align == 0) ni->align = !strcasecmp(v, "center") ? 1 : !strcasecmp(v, "right") ? 2 : 0;
    }
    if (tag == T_TABLE && (v = ATTR(dn, "align")) && S->k[L_ML] == K_PX && S->v[L_ML] == 0 && S->k[L_MR] == K_PX && S->v[L_MR] == 0) {
        if (!strcasecmp(v, "center")) S->k[L_ML] = S->k[L_MR] = K_AUTO; else if (!strcasecmp(v, "right")) S->k[L_ML] = K_AUTO;
    }
    if ((v = ATTR(dn, "bgcolor")) && !S->bg && (tag == T_TABLE || tag == T_TR || tag == T_TD || tag == T_TH || tag == T_BODY || tag == T_TGROUP)) parse_color(v, &S->bg);
    if ((tag == T_TD || tag == T_TH || tag == T_TABLE || tag == T_HR || tag == T_IMG || tag == T_DIV) && S->k[L_W] == K_AUTO && (v = ATTR(dn, "width")) && atoi(v) > 0) {
        if (strchr(v, '%')) { S->k[L_W] = K_PCT; S->v[L_W] = atoi(v); } else set_px(S, L_W, atoi(v));
    }
    if ((tag == T_TD || tag == T_TH || tag == T_IMG) && S->k[L_H] == K_AUTO && (v = ATTR(dn, "height")) && atoi(v) > 0 && !strchr(v, '%')) set_px(S, L_H, atoi(v));
    if ((tag == T_TD || tag == T_TH) && ATTR(dn, "nowrap") && ni->ws == 0) ni->ws = 1;
    if (tag == T_FONT && (v = ATTR(dn, "color"))) parse_color(v, &ni->col);
    if (tag == T_TABLE) {
        int bd = (v = ATTR(dn, "border")) ? (*v ? atoi(v) : 1) : 0;
        bd = bd < 0 ? 0 : bd > 8 ? 8 : bd;
        if (bd) for (int i = 0; i < 4; i++) if (!S->bw[i]) { S->bw[i] = bd; S->bc[i] = mix(D->bg, ni->col, 120); }
        cx2->tborder = bd;
        cx2->tpad = (v = ATTR(dn, "cellpadding")) && *v ? imax_(0, imin_(atoi(v), 40)) : -1;
        if ((v = ATTR(dn, "cellspacing")) && *v) S->gapc = S->gapr = imax_(0, imin_(atoi(v), 40));
    }
    if ((tag == T_TD || tag == T_TH)) {
        if (cx->tpad >= 0) for (int i = 0; i < 4; i++) if (S->k[L_PT + i] == K_PX && S->v[L_PT + i] == 1) S->v[L_PT + i] = cx->tpad;
        if (cx->tborder > 0) for (int i = 0; i < 4; i++) if (!S->bw[i]) { S->bw[i] = 1; S->bc[i] = mix(D->bg, ni->col, 120); }
        if ((v = ATTR(dn, "valign"))) S->valn = !strcasecmp(v, "middle") ? 1 : !strcasecmp(v, "bottom") ? 2 : !strcasecmp(v, "baseline") ? 3 : 0;
        else if (cx->tva != 255) S->valn = cx->tva;
    }
    if (tag == T_TR || tag == T_TGROUP) { if ((v = ATTR(dn, "valign"))) cx2->tva = !strcasecmp(v, "middle") ? 1 : !strcasecmp(v, "bottom") ? 2 : !strcasecmp(v, "baseline") ? 3 : 0; }
    if (tag == T_TABLE) cx2->tva = 255;
    if (tag == T_IMG && (v = ATTR(dn, "align")) && S->flt == 0) { if (!strcasecmp(v, "left")) S->flt = 1; else if (!strcasecmp(v, "right")) S->flt = 2; }
}

/* fill S (a fresh bx) and ni (what children inherit) from the element's computed style; returns the display (D_*) */
static int style_cp(int dn, int tag, struct bx *S, struct inh *ni, const struct inh *in, struct bctx *cx, struct bctx *cx2, const struct cprops *cp) {
    *ni = *in;
    memset(S, 0, sizeof *S);
    S->par = S->first = S->last = S->next = -1; S->opacity = 255; S->fw = -1; S->cb = -1; S->base = -1; S->mark = -1; S->link = -1;
    lens_init(S);
    *cx2 = *cx;
    if (tag == T_CENTER) cx2->incenter = 1;
    int disp = cp->v[P_DISPLAY];
    int bold = cp->v[P_BOLD], mono = cp->v[P_FAMILY] == FAM_MONO, size = cp->v[P_FSIZE];
    int base = mono ? (bold ? F_MONOB : F_MONO) : size >= 22 ? F_TITLE : bold ? F_UIB : F_UI;
    ni->font = size >= 22 || css_ff_n() ? font_vec(cp->k[P_FFAM] == K_STR ? css_str(cp->v[P_FFAM]) : "", mono, cp->v[P_FWEIGHT], cp->v[P_ITALIC], size, base) : base;       /* vector outlines: web fonts at any size, the built-in ones from 22 px up; the baked faces otherwise */
    ni->col = cp->v[P_COLOR];
    ni->align = cp->v[P_ALIGN] == TA_CENTER ? 1 : cp->v[P_ALIGN] == TA_RIGHT ? 2 : 0;
    ni->ws = cp->v[P_WS] == WS_BREAK_SPACES ? 3 : cp->v[P_WS];
    ni->flags = in->flags | (cp->v[P_BLEND] ? FL_DIFF : 0) | ((cp->v[P_DECO] & DECO_UNDERLINE) ? FL_UL : 0) | ((cp->v[P_DECO] & DECO_LINETHROUGH) ? FL_STRIKE : 0);
    ni->nul = 1;
    ni->vis = cp->v[P_VISIB] != 0 || cp->v[P_CLIP];
    ni->icon = cp->v[P_FAMILY] == FAM_ICON;
    ni->nomark = cp->v[P_LISTT] == LT_NONE;
    int va = cp->v[P_VALIGN];
    ni->valign = va == VA_SUPER ? VA_SUPER : va == VA_SUB ? VA_SUB : in->valign;
    int lhk = cp->k[P_LH];
    ni->lh = lhk == K_AUTO || lhk == K_NONE ? 0 : lhk == K_NUM ? FT[ni->font]->height * cp->v[P_LH] / 100 : cp->v[P_LH];
    if (ni->lh < 0) ni->lh = 0;
    for (int i = 0; i < 4; i++) {
        lenset(S, L_MT + i, cp, P_MT + i);
        lenset(S, L_PT + i, cp, P_PT + i);
        if (S->k[L_PT + i] == K_AUTO) set_px(S, L_PT + i, 0);
        S->bw[i] = cp->v[P_BSTT + i] == BS_NONE ? 0 : cp->v[P_BWT + i];
        S->bc[i] = cp->v[P_BCT + i];
        if (!(S->bc[i] >> 24) && S->bw[i]) S->bw[i] = 0;
    }
    lenset(S, L_TOP, cp, P_TOP); lenset(S, L_RIGHT, cp, P_RIGHT); lenset(S, L_BOTTOM, cp, P_BOTTOM); lenset(S, L_LEFT, cp, P_LEFT);
    lenset(S, L_W, cp, P_WIDTH); lenset(S, L_H, cp, P_HEIGHT); lenset(S, L_MINW, cp, P_MINW); lenset(S, L_MAXW, cp, P_MAXW);
    lenset(S, L_MINH, cp, P_MINH); lenset(S, L_MAXH, cp, P_MAXH); lenset(S, L_BASIS, cp, P_FBASIS);
    S->bg = cp->v[P_BG];
    if (cp->k[P_SHC] != K_NONE && !cp->v[P_SHI] && (cp->v[P_SHC] >> 24)) { S->shc = cp->v[P_SHC]; S->sh[0] = css_len(cp, P_SHX, 0); S->sh[1] = css_len(cp, P_SHY, 0); S->sh[2] = css_len(cp, P_SHB, 0); S->sh[3] = css_len(cp, P_SHS, 0); }
    if (cp->v[P_OUTS] != BS_NONE && cp->v[P_OUTW] > 0) { S->outw = cp->v[P_OUTW]; S->outc = cp->v[P_OUTC]; S->outo = css_len(cp, P_OUTO, 0); }
    ni->tt = cp->v[P_TT];
    S->aspect = cp->k[P_ASPECT] == K_PX ? cp->v[P_ASPECT] : 0;
    if (cp->k[P_ANAME] == K_STR && cp->v[P_ADUR] > 0) { int kf = css_kf_find(cp->v[P_ANAME]); if (kf >= 0) { S->anm = kf + 1; S->adur = cp->v[P_ADUR]; S->adel = cp->v[P_ADELAY]; S->ait = cp->v[P_AITER]; S->adr = cp->v[P_ADIR]; } }
    S->txv = css_len(cp, P_TRX, 0); S->tyv = css_len(cp, P_TRY, 0);                 /* px part, and the % of the box (translate(calc(-100% - 10px)) works) */
    S->txp = css_len(cp, P_TRX, 100) - S->txv; S->typ = css_len(cp, P_TRY, 100) - S->tyv;
    S->bgi = 0; S->grd = cp->k[P_BGIMG] == K_GRAD ? cp->v[P_BGIMG] + 1 : 0;
    if (cp->k[P_BGIMG] == K_STR) {                       /* url(...) is absolute already (css.c); a gradient or none paints nothing */
        int ix = img_ref_ex(css_str(cp->v[P_BGIMG]), "lazy");
        S->bgi = ix >= 0 ? ix + 1 : 0;
        S->brep = cp->v[P_BGREP];
        S->bkx = cp->k[P_BGPX]; S->bpx = cp->v[P_BGPX]; S->bky = cp->k[P_BGPY]; S->bpy = cp->v[P_BGPY];
        S->bkw = cp->k[P_BGSW]; S->bsw = cp->v[P_BGSW]; S->bkh = cp->k[P_BGSH]; S->bsh = cp->v[P_BGSH];
    }
    S->msk = 0;
    if (cp->k[P_MASKIMG] != K_NONE) {                    /* mask-image icon: the background colour shows through the picture's alpha; no usable picture -> no square */
        int ix = cp->k[P_MASKIMG] == K_STR ? img_ref_ex(css_str(cp->v[P_MASKIMG]), "lazy") : -1;
        if (ix >= 0 && S->bg >> 24) { S->bgi = ix + 1; S->msk = 1; S->brep = REP_NONE; S->bkx = S->bky = K_PCT; S->bpx = S->bpy = 5000; S->bkw = S->bkh = K_CONTAIN; }
        else S->bg = 0;
    }
    S->objfit = cp->v[P_OBJFIT];
    S->rad = cp->v[P_RTL] > 0 ? css_len(cp, P_RTL, 100) : 0;
    if (cp->k[P_RTL] == K_PCT) S->rad = 999;
    S->opacity = cp->v[P_OPACITY];
    S->pos = cp->v[P_POSITION];
    S->flt = cp->v[P_FLOAT]; S->clr = cp->v[P_CLEAR];
    S->ovf = cp->v[P_OVX] != OV_VISIBLE || cp->v[P_OVY] != OV_VISIBLE;
    S->vis = ni->vis;
    S->bsz = cp->v[P_BOXSZ] == 1;
    if (cp->k[P_ZINDEX] != K_AUTO) { S->zset = 1; S->zi = cp->v[P_ZINDEX]; }
    S->fdir = cp->v[P_FLEXDIR]; S->fwrap = cp->v[P_FLEXWRAP] != 0;
    S->just = map_just(cp->v[P_JUSTC]); S->ali = map_align(cp->v[P_ALIGNI]);
    S->als = cp->k[P_ALIGNS] == K_AUTO || cp->v[P_ALIGNS] == AL_NORMAL ? 0 : 1 + map_align(cp->v[P_ALIGNS]);
    S->grow = cp->k[P_FGROW] == K_NUM ? cp->v[P_FGROW] : 0;
    S->shrink = cp->k[P_FSHRINK] == K_NUM ? cp->v[P_FSHRINK] : 100;
    S->order = cp->v[P_ORDER];
    S->gapr = css_len(cp, P_RGAP, 0); S->gapc = css_len(cp, P_CGAP, 0);
    S->hdr = cp->v[P_COLLAPSE] != 0;
    if (tag == T_TABLE || disp == D_TABLE || disp == D_INLINE_TABLE) { S->gapc = cp->v[P_SPACING]; S->gapr = cp->v[P_SPACINGY] ? cp->v[P_SPACINGY] : cp->v[P_SPACING]; }
    S->gcols = cp->k[P_GCOLS] == K_STR ? cp->v[P_GCOLS] + 1 : 0; S->grows = cp->k[P_GROWS] == K_STR ? cp->v[P_GROWS] + 1 : 0;
    S->gareas = cp->k[P_GAREAS] == K_STR ? cp->v[P_GAREAS] + 1 : 0; S->garea = cp->k[P_GAREA] == K_STR ? cp->v[P_GAREA] + 1 : 0;
    { static const uint8_t gp[4] = { P_GCS, P_GCE, P_GRS, P_GRE }; for (int i = 0; i < 4; i++) { S->gk[i] = cp->k[gp[i]]; S->g[i] = cp->v[gp[i]]; } }
    S->cs = 1; S->rs = 1;
    int cv = cp->v[P_VALIGN];
    S->valn = cv == VA_MIDDLE ? 1 : cv == VA_BOTTOM ? 2 : cv == VA_BASELINE ? 3 : 0;
    if (tag == T_TD || tag == T_TH) {
        const char *v;
        S->cs = (v = ATTR(dn, "colspan")) ? atoi(v) : 1;
        S->rs = (v = ATTR(dn, "rowspan")) ? (*v && atoi(v) == 0 ? 1000 : atoi(v)) : 1;
        S->valn = 1;                                              /* vertical-align: middle is the table cell default */
        if (cv == VA_TOP) S->valn = 0; else if (cv == VA_BOTTOM) S->valn = 2; else if (cv == VA_BASELINE && !ATTR(dn, "valign")) S->valn = 1;
    }
    if (tag == T_TR || tag == T_TGROUP) { cx2->tva = cv == VA_TOP ? 0 : cv == VA_MIDDLE ? 1 : cv == VA_BOTTOM ? 2 : 255; }
    hints(dn, tag, S, ni, cx, cx2);
    if (ATTR(dn, "hidden")) disp = D_NONE;
    return disp;
}

/* ---------- controls ---------- */
static void parse_options(int dn, struct ctl *c) {
    struct doc *d = D;
    c->o0 = d->nop;
    for (int o = DN(dn).first; o >= 0; o = DN(o).next) {
        int opt = o;
        if (DN(o).type != 1) continue;
        if (!strcmp(hd_str(DN(o).nm), "optgroup")) { for (int q = DN(o).first; q >= 0; q = DN(q).next) if (DN(q).type == 1 && DN(q).tag == T_OPTION) { opt = q; goto have; } continue; }
        if (DN(o).tag != T_OPTION) continue;
    have:;
        for (;; ) {
            const char *val = hd_attr(opt, "value");
            int sel = hd_attr(opt, "selected") != 0;
            char lab[200];
            int k = 0, sp = 0;
            for (int t = DN(opt).first; t >= 0; t = DN(t).next) {
                if (DN(t).type != 3) continue;
                const char *s = hd_str(DN(t).nm);
                for (int q = 0; q < DN(t).tn && k < (int)sizeof lab - 1; q++) {
                    if (isspace((unsigned char)s[q])) { if (k && !sp) lab[k++] = ' '; sp = 1; }
                    else if ((unsigned char)s[q] == 0xC2 && q + 1 < DN(t).tn && (unsigned char)s[q + 1] == 0xA0) { if (k && !sp) lab[k++] = ' '; sp = 1; q++; }
                    else { lab[k++] = s[q]; sp = 0; }
                }
            }
            while (k && lab[k - 1] == ' ') k--;
            lab[k] = 0;
            GROW(d->op, d->nop, d->opcap);
            if (d->nop >= d->opcap) return;
            d->op[d->nop++] = (struct opt){ astr(lab), astr(val ? val : lab) };
            if (sel) c->rows = d->nop - 1 - c->o0;
            if (opt == o) break;
            opt = DN(opt).next;                                /* the rest of an optgroup */
            while (opt >= 0 && !(DN(opt).type == 1 && DN(opt).tag == T_OPTION)) opt = DN(opt).next;
            if (opt < 0) break;
        }
    }
    c->no = d->nop - c->o0;
}

static void text_of(int dn, char *out, int cap) {
    int k = 0, sp = 0;
    for (int c = DN(dn).first; c >= 0 && k < cap - 1; c = DN(c).next) {
        if (DN(c).type != 3) { if (DN(c).type == 1 && DN(c).tag != T_SVG) { char t[200]; text_of(c, t, sizeof t); for (int q = 0; t[q] && k < cap - 1; q++) { out[k++] = t[q]; sp = 0; } } continue; }
        const char *s = hd_str(DN(c).nm);
        for (int q = 0; q < DN(c).tn && k < cap - 1; q++) {
            if (isspace((unsigned char)s[q])) { if (k && !sp) out[k++] = ' '; sp = 1; }
            else if ((unsigned char)s[q] == 0xC2 && q + 1 < DN(c).tn && (unsigned char)s[q + 1] == 0xA0) { if (k && !sp) out[k++] = ' '; sp = 1; q++; }
            else { out[k++] = s[q]; sp = 0; }
        }
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static int tw_ui(const char *s) { return gfx_text_width(&font_ui, s); }

static int has_icon(int dn) {
    for (int c = DN(dn).first; c >= 0; c = DN(c).next)
        if (DN(c).type == 1 && (DN(c).tag == T_SVG || DN(c).tag == T_IMG || has_icon(c))) return 1;
    return 0;
}

static int has_elem(int dn) {                              /* an element child (<button><span>..</span></button>: author-styled content, not a label) */
    for (int c = DN(dn).first; c >= 0; c = DN(c).next) if (DN(c).type == 1) return 1;
    return 0;
}

/* returns the control index and sets b's size; -1 for hidden inputs */
static int build_ctl(int dn, int tag, struct bx *b, const struct bctx *cx, const struct inh *ni) {
    struct doc *d = D;
    const char *ty = ATTR(dn, "type"), *v;
    struct ctl c = { .form = cx->curform, .ord = nord++, .item = -1, .node = ni->node };
    char name[120], val[400], lab[200];
    int w = 0, h = 26, bel = 9, cols;
    strlcpy(name, (v = ATTR(dn, "name")) ? v : "", sizeof name);
    strlcpy(val, (v = ATTR(dn, "value")) ? v : "", sizeof val);
    cols = (v = ATTR(dn, "size")) && atoi(v) > 0 ? atoi(v) : 20;
    lab[0] = 0;
    c.chk = ATTR(dn, "checked") != 0;
    if (tag == T_INPUT) {
        char k[16] = "text";
        if (ty) { strlcpy(k, ty, sizeof k); for (char *p = k; *p; p++) *p = tolower((unsigned char)*p); }
        if (!strcmp(k, "hidden")) c.kind = C_HIDDEN;
        else if (!strcmp(k, "password")) c.kind = C_PASS;
        else if (!strcmp(k, "checkbox")) c.kind = C_CHECK;
        else if (!strcmp(k, "radio")) c.kind = C_RADIO;
        else if (!strcmp(k, "submit") || !strcmp(k, "image")) { c.kind = C_BTN; c.sub = 1; }
        else if (!strcmp(k, "reset")) { c.kind = C_BTN; c.sub = 2; }
        else if (!strcmp(k, "button")) c.kind = C_BTN;
        else c.kind = C_TEXT;
        if (c.kind == C_BTN) strlcpy(lab, val[0] || (ATTR(dn, "value") && strcmp(k, "image")) ? val : c.sub == 1 ? "Submit" : c.sub == 2 ? "Reset" : "", sizeof lab);
    } else if (tag == T_BUTTON) {
        c.kind = C_BTN;
        c.sub = ty && !strcasecmp(ty, "reset") ? 2 : ty && !strcasecmp(ty, "button") ? 0 : 1;
        text_of(dn, lab, sizeof lab);
        /* an empty <button> stays empty (the label "Button" is not a browser default) */
    } else if (tag == T_TEXTAREA) {
        c.kind = C_AREA;
        c.cols = (v = ATTR(dn, "cols")) && atoi(v) > 0 ? atoi(v) : 30;
        c.rows = (v = ATTR(dn, "rows")) && atoi(v) > 0 ? atoi(v) : 4;
        if (c.cols > 120) c.cols = 120;
        if (c.rows > 30) c.rows = 30;
    } else c.kind = C_SELECT;
    c.name = astr(name);
    c.ph = astr((v = ATTR(dn, "placeholder")) ? v : "");
    c.fl = (ATTR(dn, "required") ? CF_REQ : 0) | (ty && !strcasecmp(ty, "email") ? CF_EMAIL : 0) | (ATTR(dn, "autofocus") ? CF_AUTO : 0) | (ATTR(dn, "readonly") || ATTR(dn, "disabled") ? CF_RO : 0);
    if (tag == T_TEXTAREA) {
        char *tv = malloc(1);
        size_t tn = 0;
        for (int t = DN(dn).first; t >= 0; t = DN(t).next) if (DN(t).type == 3) {
            char *nv = realloc(tv, tn + DN(t).tn + 1);
            if (!nv) break;
            tv = nv; memcpy(tv + tn, hd_str(DN(t).nm), DN(t).tn); tn += DN(t).tn;
        }
        if (tv) {
            tv[tn] = 0;
            for (char *p = tv; *p; p++) if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xA0) { p[0] = ' '; memmove(p + 1, p + 2, strlen(p + 2) + 1); }
            c.val = astr(tv);
            free(tv);
        } else c.val = astr("");
    } else c.val = astr((c.kind == C_CHECK || c.kind == C_RADIO) && !val[0] ? "on" : val);
    if (c.kind == C_BTN) c.o0 = astr(lab);
    if (c.kind == C_SELECT) {
        c.rows = 0;
        parse_options(dn, &c);
        int mw = 40;
        for (int i = 0; i < c.no; i++) { int x = tw_ui(d->txt + d->op[c.o0 + i].lab); if (x > mw) mw = x; }
        w = mw + 34;
    }
    switch (c.kind) {
    case C_HIDDEN: break;
    case C_TEXT: case C_PASS: w = cols * 8 + 14; break;
    case C_BTN: w = tw_ui(lab) + 28; break;
    case C_CHECK: case C_RADIO: w = h = 16; bel = 2; break;
    case C_AREA: w = c.cols * 8 + 14; h = c.rows * 18 + 10; bel = 4; break;
    }
    GROW(d->ct, d->nct, d->ctcap);
    if (d->nct >= d->ctcap) return -1;
    int ci = d->nct++;
    d->ct[ci] = c;
    b->iw = w; b->ih = h; b->bel = bel;
    return c.kind == C_HIDDEN ? -1 : ci;
}

/* ---------- build ---------- */
static void build_children(int dn, int pb, const struct inh *in, struct bctx *cx);
static int root_box = -1, is_js;

static int mk_text(int pb, uint32_t off, int n, const struct inh *in) {
    if (in->icon) return -1;                                    /* icon-font text (Material ligature names, private-use glyphs): nothing to show */
    int t = bx_new();
    if (t < 0) return -1;
    struct bx *x = &B[t];
    lens_init(x);
    x->outer = OUT_INLINE; x->inner = IN_TEXT; x->toff = off; x->tlen = n;
    x->tt = in->tt; x->font = in->font; x->flags = in->flags; x->nul = in->nul; x->ws = in->ws; x->col = in->col; x->link = in->link; x->node = in->node;
    x->lh = in->lh; x->vis = in->vis; x->valign = in->valign; x->align = in->align;
    bx_append(pb, t);
    return t;
}

static void build_text(int tn, int pb, const struct inh *in) {
    if (!DN(tn).tn) return;
    mk_text(pb, DN(tn).nm, DN(tn).tn, in);
}

static const char *cur_base_url;
static int plain_text;

static void build_img(int dn, int pb, struct bx *S, const struct inh *ni) {
    const char *src = ATTR(dn, "src"), *alt = ATTR(dn, "alt");
    int idx = -1, iw = 0, ih = 0, st = IMG_FAILED;
    char u[URL_MAX], pk[URL_MAX];
    int par = HD.n[dn].par, picked = 0;
    if (par > 0 && !strcmp(hd_str(HD.n[par].nm), "picture")) {          /* <picture>: the first usable <source> wins */
        for (int s = HD.n[par].first; s >= 0 && s != dn && !picked; s = HD.n[s].next)
            if (HD.n[s].type == 1 && !strcmp(hd_str(HD.n[s].nm), "source") && img_type_ok(ATTR(s, "type")) && (!ATTR(s, "media") || css_media(ATTR(s, "media"))) &&
                img_pick(0, ATTR(s, "srcset"), ATTR(s, "sizes"), rootw, pk, sizeof pk)) { src = pk; picked = 1; }
    }
    if (!picked && ATTR(dn, "srcset") && img_pick(src, ATTR(dn, "srcset"), ATTR(dn, "sizes"), rootw, pk, sizeof pk)) src = pk;
    if (src && *src) {
        if (!strncasecmp(src, "data:", 5)) idx = img_ref_ex(src, ATTR(dn, "loading"));
        else { url_resolve(cur_base_url, src, u, sizeof u); idx = img_ref_ex(u, ATTR(dn, "loading")); }
        st = img_info(idx, &iw, &ih);
    }
    const char *aw = ATTR(dn, "width"), *ah = ATTR(dn, "height");
    int W = aw && atoi(aw) > 0 && !strchr(aw, '%') ? atoi(aw) : 0, H = ah && atoi(ah) > 0 && !strchr(ah, '%') ? atoi(ah) : 0;
    int cssw = S->k[L_W] != K_AUTO, cssh = S->k[L_H] == K_PX;
    if (st == IMG_FAILED && !((W || cssw) && (H || cssh))) {      /* broken and unsized: just the alt text, like a text browser */
        char buf[200];
        snprintf(buf, sizeof buf, "[image%s%s]", alt && *alt ? ": " : "", alt && *alt ? alt : "");
        struct inh dim = *ni;
        dim.col = col_dim;
        mk_text(pb, hd_pool_add(buf, strlen(buf)), strlen(buf), &dim);
        return;
    }
    int bi = bx_new();
    if (bi < 0) return;
    B[bi] = *S;
    struct bx *b = &B[bi];
    b->outer = b->flt || oof(b) || S->outer == OUT_BLOCK ? OUT_BLOCK : OUT_INLINE; b->inner = IN_IMG; b->ref = idx;
    b->link = ni->link; b->node = ni->node; b->vis = ni->vis; b->col = ni->col; b->font = ni->font;
    if (st == IMG_READY && iw > 0 && ih > 0) { b->iw = iw; b->ih = ih; }
    else if (W && H) { b->iw = W; b->ih = H; }
    else if (W) { b->iw = W; b->ih = W * 3 / 4; }
    else if (H) { b->iw = H * 4 / 3; b->ih = H; }
    else { b->iw = 96; b->ih = 64; }
    if (W && !cssw) { b->k[L_W] = K_PX; b->v[L_W] = W; }
    if (H && !cssh) { b->k[L_H] = K_PX; b->v[L_H] = H; }
    if (aw && strchr(aw, '%') && !cssw && atoi(aw) > 0) { b->k[L_W] = K_PCT; b->v[L_W] = atoi(aw); }
    if (alt && *alt) { b->toff = (uint32_t)(alt - HD.pool); b->tlen = strlen(alt); }
    bx_append(pb, bi);
    if (oof(b)) abs_reg(bi);
}

static void build_svg(int dn, int pb, struct bx *S, const struct inh *ni, const struct cprops *cp) {
    int c = HD.n[dn].first;
    if (c < 0) return;
    const char *mk = hd_str(HD.n[c].nm);
    size_t mn = HD.n[c].tn;
    char *res = 0;
    if (svg_mem(mk, mn, "<use", 4)) {                         /* sprite references: copy the <symbol>/element from this or another inline svg */
        static const char *dp[64]; static size_t dl[64];
        int nd = 0;
        for (int j = 0; j < HD.nn && nd < 64; j++) {
            int f = HD.n[j].type == 1 && HD.n[j].tag == T_SVG && j != dn ? HD.n[j].first : -1;
            if (f >= 0 && HD.n[f].tn > 32 && svg_mem(hd_str(HD.n[f].nm), HD.n[f].tn, "<symbol", 7)) { dp[nd] = hd_str(HD.n[f].nm); dl[nd++] = HD.n[f].tn; }
        }
        size_t rn = 0;
        if ((res = svg_resolve_use(mk, mn, dp, dl, nd, &rn))) { mk = res; mn = rn; }
    }
    char *styled = 0;
    if (cp->k[P_FILL] == K_PX || cp->k[P_STROKE] == K_PX || cp->k[P_STROKEW] == K_PX) {   /* CSS fill/stroke on the svg (or inherited): a style attribute at the end of the root tag */
        const char *gt = memchr(mk, '>', mn);
        if (gt) {
            char st[120];
            int sl = 0;
            for (int w = 0; w < 3; w++) {
                int p = w == 0 ? P_FILL : w == 1 ? P_STROKE : P_STROKEW;
                if (cp->k[p] != K_PX) continue;
                if (w == 2) sl += snprintf(st + sl, sizeof st - sl, "stroke-width:%d;", cp->v[p]);
                else if (!(cp->v[p] >> 24)) sl += snprintf(st + sl, sizeof st - sl, "%s:none;", w ? "stroke" : "fill");
                else sl += snprintf(st + sl, sizeof st - sl, "%s:#%06X;", w ? "stroke" : "fill", (unsigned)cp->v[p] & 0xFFFFFF);
            }
            size_t at = gt - mk - (gt > mk && gt[-1] == '/');
            if ((styled = malloc(mn + sl + 16))) {
                size_t o = at;
                memcpy(styled, mk, at);
                o += snprintf(styled + o, sl + 16, " style=\"%s\"", st);
                memcpy(styled + o, mk + at, mn - at);
                mk = styled; mn = mn + (o - at);
            }
        }
    }
    int idx = img_svg_inline(mk, mn, ni->col & 0xFFFFFF), iw = 0, ih = 0;
    free(res); free(styled);
    if (idx < 0 || img_info(idx, &iw, &ih) != IMG_READY || iw <= 0 || ih <= 0) return;
    const char *aw = ATTR(dn, "width"), *ah = ATTR(dn, "height");
    int bi = bx_new();
    if (bi < 0) return;
    B[bi] = *S;
    struct bx *b = &B[bi];
    b->outer = b->flt || oof(b) || S->outer == OUT_BLOCK ? OUT_BLOCK : OUT_INLINE; b->inner = IN_IMG; b->ref = idx;
    b->link = ni->link; b->node = ni->node; b->vis = ni->vis; b->col = ni->col; b->font = ni->font;
    b->iw = iw; b->ih = ih;
    if (b->k[L_W] == K_AUTO && aw && atoi(aw) > 0 && !strchr(aw, '%')) { b->k[L_W] = K_PX; b->v[L_W] = atoi(aw); }
    if (b->k[L_H] == K_AUTO && ah && atoi(ah) > 0 && !strchr(ah, '%')) { b->k[L_H] = K_PX; b->v[L_H] = atoi(ah); }
    bx_append(pb, bi);
    if (oof(b)) abs_reg(bi);
}

static int bg_from_html;
static struct cprops CPS[256];
#ifdef LT_DEBUG
static double t_css, t_sty;
#endif
static int cpd;

static int luma(color_t c) { return (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10; }

/* the page background comes from <html> or <body>; a dark one gets light text and links unless the page says otherwise */
static void root_colors(const struct bx *S, int tag, int dn, struct inh *ni) {
    struct doc *d = D;
    if (S->bg >> 24 == 0xFF && (tag == T_HTML || (tag == T_BODY && !bg_from_html))) {
        d->bg = S->bg;
        if (tag == T_HTML) bg_from_html = 1;
    }
    if (tag == T_BODY) {
        const char *v;
        if ((v = ATTR(dn, "text"))) parse_color(v, &ni->col);
        if ((v = ATTR(dn, "link"))) { parse_color(v, &d->linkc); d->hoverc = ni->col; }
    }
    if (dark(d->bg)) {
        d->linkc = RGB(0x8C, 0xB4, 0xFF); d->hoverc = RGB(0xFF, 0x9A, 0xD0);
    }
    d->fg = ni->col;
    col_dim = mix(d->fg, d->bg, 130);
}

static void build_el(int dn, int pb, const struct inh *in, struct bctx *cx, int isfirst, int pe);
static void build_el_body(int dn, int pb, const struct inh *in, struct bctx *cx, int isfirst, int pe, int tag, struct cprops *cp);

static void build_pseudo(int dn, int pb, const struct inh *in, struct bctx *cx, int pe) {
    build_el(dn, pb, in, cx, 0, pe);
}

static void build_el(int dn, int pb, const struct inh *in, struct bctx *cx, int isfirst, int pe) {
    int tag = pe ? T_NONE : DN(dn).tag;
    const char *name = hd_str(DN(dn).nm);
    if (!pe && (tag == T_HEAD || tag == T_SKIP || tag == T_COL || tag == T_OPTION)) return;
    if (!pe && tag == T_DIV && !strcmp(name, "noscript") && is_js) return;
    if (cpd >= 250) return;
    struct cprops *cp = &CPS[cpd];
    const struct cprops *par = cpd ? &CPS[cpd - 1] : 0;
#ifdef LT_DEBUG
    double tq = now_ms();
#endif
    if (pe) { if (!css_compute(dn, pe, par, cp)) return; }
    else css_compute(dn, PE_NONE, par, cp);
#ifdef LT_DEBUG
    t_css += now_ms() - tq;
#endif
    if (tag == T_TABLE && (HD.quirks || cx->incenter)) cp->v[P_ALIGN] = TA_LEFT;   /* quirks (or <center><table>, which relied on it): a table doesn't inherit text-align */
    if (!pe) css_cnt_enter(cp, cpd);
    build_el_body(dn, pb, in, cx, isfirst, pe, tag, cp);
    if (!pe) css_cnt_leave(cpd);
}

static void build_el_body(int dn, int pb, const struct inh *in, struct bctx *cx, int isfirst, int pe, int tag, struct cprops *cp) {
    const char *v;
    struct bx S;
    struct inh ni;
    struct bctx cx2;
#ifdef LT_DEBUG
    double ts = now_ms();
#endif
    int disp = style_cp(dn, tag, &S, &ni, in, cx, &cx2, cp);
#ifdef LT_DEBUG
    t_sty += now_ms() - ts;
#endif
    if (!pe && HD.n[dn].js) ni.node = HD.n[dn].js;
    else if (!pe && (v = ATTR(dn, "data-vjs"))) ni.node = atoi(v);
    if (disp == D_NONE) return;
    if (tag == T_HTML || tag == T_BODY) {
        root_colors(&S, tag, dn, &ni);
        S.bg = 0;
        if (tag == T_BODY && !css_any()) {                      /* no author CSS at all: a readable centred column, like the old engine */
            for (int i = 0; i < 4; i++) set_px(&S, L_MT + i, 0);
            S.k[L_ML] = S.k[L_MR] = K_AUTO; set_px(&S, L_MAXW, 880); set_px(&S, L_PL, 20); set_px(&S, L_PR, 20); set_px(&S, L_PT, 14);
        }
    }
    if (plain_text && tag == T_PRE) { for (int i = 0; i < 8; i++) set_px(&S, L_MT + i, 0); S.bg = 0; }
    if (tag == T_A && (v = ATTR(dn, "href")) && strncasecmp(v, "javascript:", 11)) ni.link = link_add(v);
    /* display -> box kind */
    S.outer = OUT_INLINE; S.inner = IN_INLINE;
    int li = 0, contents = 0;
    switch (disp) {
    case D_BLOCK: S.outer = OUT_BLOCK; S.inner = IN_FLOW; break;
    case D_INLINE_BLOCK: S.inner = IN_FLOW; break;
    case D_FLEX: S.outer = OUT_BLOCK; S.inner = IN_FLEX; break;
    case D_INLINE_FLEX: S.inner = IN_FLEX; break;
    case D_GRID: S.outer = OUT_BLOCK; S.inner = IN_GRID; break;
    case D_INLINE_GRID: S.inner = IN_GRID; break;
    case D_TABLE: S.outer = OUT_BLOCK; S.inner = IN_TABLE; break;
    case D_INLINE_TABLE: S.inner = IN_TABLE; break;
    case D_TABLE_ROW: S.outer = OUT_BLOCK; S.inner = IN_TROW; break;
    case D_TABLE_CELL: S.outer = OUT_BLOCK; S.inner = IN_TCELL; break;
    case D_TABLE_ROWGROUP: case D_TABLE_HEADER: case D_TABLE_FOOTER: S.outer = OUT_BLOCK; S.inner = IN_TGROUP; break;
    case D_TABLE_CAPTION: S.outer = OUT_BLOCK; S.inner = IN_CAP; break;
    case D_TABLE_COL: case D_TABLE_COLGROUP: return;
    case D_LIST_ITEM: S.outer = OUT_BLOCK; S.inner = IN_FLOW; li = 1; break;
    case D_CONTENTS: contents = 1; break;
    }
    S.font = ni.font; S.flags = ni.flags; S.nul = ni.nul; S.ws = ni.ws; S.align = ni.align; S.col = ni.col; S.link = ni.link; S.node = ni.node;
    S.lh = ni.lh; S.vis = ni.vis; S.valign = ni.valign;
    if (S.flt || oof(&S)) { S.outer = OUT_BLOCK; if (S.inner == IN_INLINE) S.inner = IN_FLOW; }
    if (!pe && ((v = ATTR(dn, "id")) || (tag == T_A && (v = ATTR(dn, "name")))) && *v) S.aname = astr(v) + 1;
    S.cb = cx->cb; S.dn = pe ? 0 : dn;
    if (li) { struct cprops mp; if (!pe && css_compute(dn, PE_MARKER, cp, &mp) && mp.v[P_COLOR]) S.markc = mp.v[P_COLOR]; S.li = 1; S.mark = ni.nomark ? -1 : cx->cnt ? ++*cx->cnt : 1; S.lt = cp->v[P_LISTT]; }

    if (tag == T_BR) {
        int bi = bx_new();
        if (bi >= 0) { B[bi] = S; B[bi].outer = OUT_INLINE; B[bi].inner = IN_BR; bx_append(pb, bi); }
        return;
    }
    if (tag == T_IMG) { build_img(dn, pb, &S, &ni); return; }
    if (tag == T_SVG) { build_svg(dn, pb, &S, &ni, cp); return; }
    int iconbtn = 0;                                          /* <button><svg/></button>: lay the children out as an inline-block instead of a text control */
    if (tag == T_BUTTON && (has_icon(dn) || S.inner == IN_FLEX || S.inner == IN_GRID || has_elem(dn))) {                    /* (a bare unstyled submit button keeps the real control, so it still submits) */
        const char *ty2 = ATTR(dn, "type");
        iconbtn = S.bg || S.bw[0] | S.bw[1] | S.bw[2] | S.bw[3] || !(ty2 && !strcasecmp(ty2, "submit"));
        if (iconbtn && S.inner == IN_INLINE) S.inner = IN_FLOW;
    }
    if (!iconbtn && (tag == T_INPUT || tag == T_TEXTAREA || tag == T_SELECT || tag == T_BUTTON)) {
        int bi = bx_new();
        if (bi >= 0) {
            B[bi] = S;
            int ci = build_ctl(dn, tag, &B[bi], cx, &ni);
            if (ci < 0) nb--;
            else {
                struct bx *b = &B[bi];
                b->outer = S.flt || oof(&S) || S.outer == OUT_BLOCK ? OUT_BLOCK : OUT_INLINE;
                b->inner = IN_CTL; b->ref = ci; b->li = 0;
                if (b->bw[2] && !b->bw[0] && !b->bw[1] && !b->bw[3] && (tag == T_INPUT || tag == T_TEXTAREA)) D->ct[ci].fl |= CF_FLAT;
                bx_append(pb, bi);
                if (oof(b)) abs_reg(bi);
            }
        }
        return;
    }
    if (tag == T_FORM) {
        GROW(D->fm, D->nfm, D->fmcap);
        if (D->nfm < D->fmcap) {
            const char *m = ATTR(dn, "method");
            D->fm[D->nfm] = (struct form){ .action = astr(ATTR(dn, "action") ? ATTR(dn, "action") : ""), .post = m && !strcasecmp(m, "post"), .node = ni.node };
            cx2.curform = D->nfm++;
        }
    }
    int cnt = 0;
    if (tag == T_OL || tag == T_UL || tag == T_NONE) { cnt = tag == T_OL && (v = ATTR(dn, "start")) ? atoi(v) - 1 : 0; if (tag != T_NONE || disp == D_BLOCK) cx2.cnt = &cnt; }

    /* an inline element with nothing to paint or position needs no box of its own: its children join the parent */
    int plain = contents || (S.outer == OUT_BLOCK && 0);
    if (!plain && !(pb >= 0 && (B[pb].inner == IN_FLEX || B[pb].inner == IN_GRID)) && S.outer == OUT_INLINE && S.inner == IN_INLINE && S.pos == POS_STATIC && !S.bg && !S.aname && !S.li && !pe) {
        plain = 1;
        for (int i = 0; i < 4 && plain; i++) if (S.bw[i]) plain = 0;
        for (int i = 0; i < 8 && plain; i++) if (S.k[L_MT + i] == K_AUTO || S.v[L_MT + i]) plain = 0;
        if (S.opacity != 255) plain = 0;
    }
    if (plain) {
        cpd++;
        build_children(dn, pb, &ni, &cx2);
        cpd--;
        return;
    }
    int bi = bx_new();
    if (bi < 0) return;
    B[bi] = S;
    if (pb >= 0) bx_append(pb, bi);
    if (S.pos != POS_STATIC) cx2.cb = bi;
    if (oof(&S)) abs_reg(bi);
    if (S.zset && S.zi < 0) { int o = cx->cb >= 0 ? cx->cb : root_box; if (o >= 0) B[o].hasneg = 1; }
    if (pe) {
        const char *t = css_str(cp->v[P_CONTENT]);
        if (t && *t) mk_text(bi, hd_pool_add(t, strlen(t)), strlen(t), &ni);
    } else {
        if (tag == T_PRE) {
            int c = DN(dn).first;
            if (c >= 0 && DN(c).type == 3 && hd_str(DN(c).nm)[0] == '\n') { DN(c).nm++; DN(c).tn--; }   /* the first newline after <pre> is dropped */
        }
        cpd++;
        build_pseudo(dn, bi, &ni, &cx2, PE_BEFORE);
        build_children(dn, bi, &ni, &cx2);
        build_pseudo(dn, bi, &ni, &cx2, PE_AFTER);
        cpd--;
    }
    if (B[bi].inner == IN_INLINE) {                           /* blocks inside an inline element: it becomes a block */
        for (int c = B[bi].first; c >= 0; c = B[c].next) if (in_flow_block(&B[c])) { B[bi].outer = OUT_BLOCK; B[bi].inner = IN_FLOW; break; }
    }
    fixup(bi);
}

static void build_children(int dn, int pb, const struct inh *in, struct bctx *cx) {
    int seen = 0;
    for (int c = DN(dn).first; c >= 0; c = DN(c).next) {
        if (DN(c).type == 3) { build_text(c, pb, in); continue; }
        if (DN(c).type != 1) continue;
        build_el(c, pb, in, cx, !seen, PE_NONE);
        seen = 1;
    }
}

static int build_root(void) {
    int r = bx_new();
    struct bx *b = &B[r];
    lens_init(b);
    b->outer = OUT_BLOCK; b->inner = IN_FLOW;
    root_box = r;
    struct inh in = { .font = F_UI, .col = D->fg, .link = -1 };
    struct bctx cx = { .cb = -1, .curform = -1, .tpad = -1, .tva = 255 };
    is_js = HD.js;
    for (int c = DN(0).first; c >= 0; c = DN(c).next) if (DN(c).type == 1 && DN(c).tag == T_HTML && hd_attr(c, "data-vjs")) is_js = 1;
    b->font = in.font; b->col = in.col; b->link = -1;
    css_dom_set(&CD);
    cpd = 0;
    css_cnt_reset_all();
    build_children(0, r, &in, &cx);
    fixup(r);
    return r;
}

#ifdef LT_DEBUG
static void dump(int bi, int depth) {
    struct bx *b = &B[bi];
    const char *nm = b->inner == IN_TEXT ? "#text" : b->dn > 0 ? hd_str(HD.n[b->dn].nm) : b->anon ? "(anon)" : "?";
    const char *id = b->dn > 0 && b->inner != IN_TEXT ? hd_attr(b->dn, "id") : 0, *cl = b->dn > 0 && b->inner != IN_TEXT ? hd_attr(b->dn, "class") : 0;
    fprintf(stderr, "%*s%s%s%s%s%s o%d i%d p%d f%d [%d,%d %dx%d] %s fr%d@%d,%d\n", depth * 2, "", nm, id ? "#" : "", id ? id : "", cl ? "." : "", cl ? cl : "", b->outer, b->inner, b->pos, b->flt, b->x, b->y, b->w, b->h,
            b->inner == IN_TEXT ? "" : "", b->nfr, b->nfr ? b->fr[0].x : 0, b->nfr ? b->fr[0].y : 0);
    for (int c = b->first; c >= 0; c = B[c].next) if (depth < 14) dump(c, depth + 1);
}
#endif

/* ---------- entry ---------- */
static void free_boxes(void) {
    for (int i = 0; i < nb; i++) { free(B[i].fr); tab_free(B[i].tab); }
    nb = 0;
}

#ifdef LT_DEBUG
#define PH(name) do { double n_ = now_ms(); if (getenv("LT_TIME")) fprintf(stderr, "  %-8s %6.1f ms\n", name, n_ - t_phase); t_phase = n_; } while (0)
#else
#define PH(name) ((void)0)
#endif

static void layout_do(struct doc *d, const char *src, size_t n, int mode, int width, const char *base) {   /* mode: 0 plain text, 1 html source, 2 js.c tree */
    char title[sizeof d->title];
    strlcpy(title, d->title, sizeof title);          /* a title survives relayout */
    doc_free(d);
    D = d;
    cur_base_url = base;
    css_reset();
    css_viewport_size(width, VIEWH);
    css_base(base);
    rootw = width;
    d->bg = RGB(0xFF, 0xFF, 0xFF); d->fg = RGB(0x1A, 0x1A, 0x22);
    d->linkc = RGB(0x0B, 0x57, 0xD0); d->hoverc = RGB(0xC0, 0x1F, 0x7A);
    col_em = RGB(0x8A, 0x1C, 0x8A);
    col_dim = mix(d->fg, d->bg, 130);
#ifdef LT_DEBUG
    double t_phase = now_ms();
#endif
    free_boxes();
    nabs = npe = nord = nel = cur_clip = in_stk = 0;
    bg_from_html = is_js = 0;
    root_box = -1;
    plain_text = mode == 0;
    if (mode == 2) hd_from_js(d, base); else if (mode == 1) hd_parse(src, n, base, d); else hd_plain(src, n);
    PH("parse");
    int root = build_root();
    PH("build");
#ifdef LT_DEBUG
    if (getenv("LT_TIME")) fprintf(stderr, "  (css_compute %.1f, style_cp %.1f ms of build)\n", t_css, t_sty);
    t_css = 0;
#endif
    rootw = width;
    int c0 = 0;
    B[root].fw = -1;
    lay_block(&B[root], 0, width, 0, 0, &c0, LB_ROOT);
    lay_abs_for(-1);
    PH("layout");
    d->height = B[root].y + B[root].h + 24;
    for (int i = 0; i < nabs; i++) {                          /* abs boxes add to the scrollable height unless an overflow clip or a fixed box holds them */
        const struct bx *c = &B[ABS[i]];
        if (c->pos != POS_ABS || c->vis) continue;
        int p = c->par;
        while (p >= 0 && (!B[p].ovf || p == root || B[p].par == root) && B[p].pos != POS_FIXED) p = B[p].par;   /* html/body overflow goes to the viewport */
        if (p < 0 && c->y + c->h + 24 > d->height) d->height = c->y + c->h + 24;
    }
#ifdef LT_DEBUG
    if (getenv("LT_DUMP")) dump(root, 0);
#endif
    if (js_active())                                          /* getBoundingClientRect() & co: the border boxes of the scripts' elements */
        for (int i = 0; i < nb; i++) {
            const struct bx *b = &B[i];
            if (b->dn > 0 && b->node && b->inner != IN_TEXT && !b->anon && (b->w || b->h)) js_set_rect(b->node, b->x, b->y, b->w, b->h);
        }
    int bot = 0;
    paint_box(root, 0, 0);
    (void)bot;
    PH("paint");
    free_boxes();
    hd_free();
    d->pmax = malloc((d->nit + 1) * sizeof *d->pmax);
    d->smin = malloc((d->nit + 1) * sizeof *d->smin);
    if (!d->pmax || !d->smin) d->nit = 0;                       /* out of memory: nothing to draw rather than a broken search */
    else {
        int hi = -(1 << 30), lo = 1 << 30;
        for (int i = 0; i < d->nit; i++) { if (d->it[i].y + d->it[i].h > hi) hi = d->it[i].y + d->it[i].h; d->pmax[i] = hi; }
        for (int i = d->nit - 1; i >= 0; i--) { if (d->it[i].y < lo) lo = d->it[i].y; d->smin[i] = lo; }
    }
    if (!d->title[0]) strlcpy(d->title, title, sizeof d->title);
}

void doc_layout(struct doc *d, const char *src, size_t n, int is_html, int width, const char *base) { layout_do(d, src, n, is_html, width, base); }
void doc_layout_js(struct doc *d, int width, const char *base) { layout_do(d, 0, 0, 2, width, base); }
