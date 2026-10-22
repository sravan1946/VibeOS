/* html.c - tolerant HTML subset: tokenizer + inline/block layout into a flat display list.
 * Layout runs once per page/resize; doc_draw only touches the items that are on screen.
 * CSS (css.c) is applied per element as the tags stream past; images and form controls are inline boxes.
 * ponytail: no real table grid (cells flow inline, separated by " | "), no floats, no inline backgrounds. */
#include "browser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

enum { F_UI, F_UIB, F_TITLE, F_MONO, F_MONOB };
static const struct font *const FT[] = { &font_ui, &font_ui_bold, &font_title, &font_mono, &font_mono_bold };
#define LEAD 3
#define LN_MAX 256
#define MAXFR 128
#define MAX_ITEMS 250000

enum { T_NONE, T_HTML, T_BODY, T_TITLE, T_RAW, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_P, T_DIV, T_CENTER,
       T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_BQ, T_PRE, T_CODE, T_B, T_I, T_U, T_S, T_A, T_FONT,
       T_BR, T_HR, T_IMG, T_TABLE, T_TR, T_TD, T_TH, T_CAP, T_FORM, T_INPUT, T_TEXTAREA, T_SELECT, T_BUTTON, T_CSS };

static const struct { const char *n; int t; } TAGS[] = {
    { "html", T_HTML }, { "head", T_HTML }, { "body", T_BODY }, { "title", T_TITLE },
    { "script", T_RAW }, { "style", T_RAW }, { "svg", T_RAW }, { "template", T_RAW },
    { "h1", T_H1 }, { "h2", T_H2 }, { "h3", T_H3 }, { "h4", T_H4 }, { "h5", T_H5 }, { "h6", T_H6 },
    { "p", T_P }, { "div", T_DIV }, { "section", T_DIV }, { "article", T_DIV }, { "header", T_DIV },
    { "footer", T_DIV }, { "nav", T_DIV }, { "main", T_DIV }, { "aside", T_DIV }, { "form", T_FORM },
    { "input", T_INPUT }, { "textarea", T_TEXTAREA }, { "select", T_SELECT }, { "button", T_BUTTON },
    { "figure", T_DIV }, { "figcaption", T_DIV }, { "address", T_DIV }, { "details", T_DIV },
    { "summary", T_DIV }, { "fieldset", T_DIV }, { "center", T_CENTER },
    { "ul", T_UL }, { "menu", T_UL }, { "dir", T_UL }, { "ol", T_OL }, { "li", T_LI },
    { "dl", T_DL }, { "dt", T_DT }, { "dd", T_DD }, { "blockquote", T_BQ },
    { "pre", T_PRE }, { "listing", T_PRE }, { "xmp", T_PRE },
    { "code", T_CODE }, { "tt", T_CODE }, { "kbd", T_CODE }, { "samp", T_CODE },
    { "b", T_B }, { "strong", T_B }, { "i", T_I }, { "em", T_I }, { "cite", T_I }, { "var", T_I }, { "dfn", T_I },
    { "u", T_U }, { "ins", T_U }, { "s", T_S }, { "strike", T_S }, { "del", T_S },
    { "a", T_A }, { "font", T_FONT }, { "br", T_BR }, { "hr", T_HR }, { "img", T_IMG },
    { "table", T_TABLE }, { "tr", T_TR }, { "td", T_TD }, { "th", T_TH }, { "caption", T_CAP },
};

struct style { uint8_t font, flags, pre, center, nul, nomark; int link; color_t col; int indent, rin; };   /* center: 0 left 1 centre 2 right */
struct frame {
    int tag; struct style s; int after, kind, deco_y, n, ordered;   /* kind: 1 quote, 2 pre */
    int elidx;                       /* element-stack depth this frame belongs to (css.c ancestry) */
    int box, y0, bx0, bx1, pb, bbw;  /* CSS box: decorations to finish at the end; top y, extent, padding-bottom, border-bottom */
    int dv[3];                       /* decor index + 1 whose height is patched at the end: bg, left border, right border */
    color_t bbc;
    int rgap;                        /* inline element: right margin+padding, applied when it ends */
};

static struct {
    struct doc *d;
    int ml, R;                       /* left margin, right edge of the text column */
    int y, gap;                      /* top of the next line; bottom margin already inserted */
    int x, lleft, lasc, ldesc, lcenter, nln;
    struct item ln[LN_MAX];
    char wb[512]; int wn;
    int pend;                        /* whitespace seen since the last word */
    struct frame fr[MAXFR], dummy; int nfr;
    int cell_n, stop, skip_nl, col, cw;
    color_t em, dim;
    struct el el[64]; int nel;       /* open elements, for selectors */
    int pushed; struct frame *lastpush;
    int ovr;                         /* CSS margin-top replacing the next block() margin (-1 = none) */
    const struct cprops *cp;         /* properties of the element being opened */
    const char *base;                /* document URL, to resolve <img src> and the like */
    int curform, nord;
    int hgap;                        /* horizontal margin/padding of inline elements, added before the next box */
    const char *content; int clen;   /* body of <textarea>/<select>/<button> */
} L;
#define S (L.fr[L.nfr - 1].s)
static int right(void) { return L.R - S.rin; }
static int cur_node(void) { return L.nel ? L.el[L.nel - 1].js : 0; }   /* js.c's node id of the innermost element (0 = none) */

static int dark(color_t c) { return (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10 < 110; }

/* ---------- document storage ---------- */
void doc_free(struct doc *d) {
    for (int i = 0; i < d->nlink; i++) free(d->link[i]);
    free(d->it); free(d->dc); free(d->txt); free(d->link); free(d->an); free(d->ct); free(d->op); free(d->fm);
    memset(d, 0, sizeof *d);
}

static uint32_t arena(const char *s, int n) {
    struct doc *d = L.d;
    if (d->ntxt + n + 1 > d->txtcap) {
        size_t nc = d->txtcap ? d->txtcap * 2 : 16384;
        while (nc < d->ntxt + n + 1) nc *= 2;
        char *nt = realloc(d->txt, nc);
        if (!nt) { L.stop = 1; return 0; }
        d->txt = nt; d->txtcap = nc;
    }
    uint32_t off = d->ntxt;
    memcpy(d->txt + off, s, n);
    d->ntxt += n;
    if (d->ntxt > (8u << 20)) { L.stop = 1; L.d->truncated = 1; }
    return off;
}

static struct item *doc_item(void) {
    static struct item sink;
    struct doc *d = L.d;
    if (d->nit >= MAX_ITEMS) { L.stop = 1; d->truncated = 1; return &sink; }
    if (d->nit == d->itcap) {
        int nc = d->itcap ? d->itcap * 2 : 1024;
        struct item *ni = realloc(d->it, nc * sizeof *ni);
        if (!ni) { L.stop = 1; return &sink; }
        d->it = ni; d->itcap = nc;
    }
    return &d->it[d->nit++];
}

static int add_decor(int x, int y, int w, int h, color_t col) {      /* returns the index */
    struct doc *d = L.d;
    if (d->ndc == d->dccap) {
        int nc = d->dccap ? d->dccap * 2 : 64;
        struct decor *nd = realloc(d->dc, nc * sizeof *nd);
        if (!nd) return -1;
        d->dc = nd; d->dccap = nc;
    }
    d->dc[d->ndc] = (struct decor){ x, y, w, h, col };
    return d->ndc++;
}

static void *grow_(void *p, int *cap, size_t sz) {
    int nc = *cap ? *cap * 2 : 16;
    void *np = realloc(p, nc * sz);
    if (!np) return p;
    *cap = nc;
    return np;
}
#define GROW(p, n, cap) do { if ((n) >= (cap)) (p) = grow_((p), &(cap), sizeof *(p)); } while (0)

static uint32_t arena(const char *s, int n);
static uint32_t astr(const char *s) { return arena(s, strlen(s) + 1); }       /* NUL-terminated copy */

static int link_add(const char *href) {
    struct doc *d = L.d;
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

static void anchor_add(const char *name) {
    struct doc *d = L.d;
    if (!*name || d->nan >= 512) return;
    if (!(d->nan & 63)) {
        struct anchor *na = realloc(d->an, (d->nan + 64) * sizeof *na);
        if (!na) return;
        d->an = na;
    }
    strlcpy(d->an[d->nan].name, name, sizeof d->an[0].name);
    d->an[d->nan++].y = L.y;
}

/* ---------- line building ---------- */
static int left(void) { return L.ml + S.indent; }

static void line_flush(int force) {
    if (!L.nln) {
        if (force) { L.y += FT[S.font]->height + LEAD; L.gap = 0; }
        return;
    }
    int shift = 0;
    if (L.lcenter) {
        shift = right() - (L.ln[L.nln - 1].x + L.ln[L.nln - 1].w);
        if (L.lcenter == 1) shift /= 2;
        if (shift < 0) shift = 0;
    }
    for (int i = 0; i < L.nln; i++) {
        struct item *it = doc_item();
        *it = L.ln[i];
        it->x += shift;
        it->y = L.y + L.lasc - (it->flags & (FL_IMG | FL_CTL) ? it->h - it->bel : FT[it->font]->ascent);
        if (it->h > L.d->maxh) L.d->maxh = it->h;
        if ((it->flags & FL_CTL) && it->ref < L.d->nct) L.d->ct[it->ref].item = L.d->nit - 1;
    }
    L.y += L.lasc + L.ldesc + LEAD;
    L.gap = 0;
    L.nln = L.lasc = L.ldesc = 0;
}

static void emit(const char *s, int n, int w, int sp) {
    const struct font *f = FT[S.font];
    uint8_t flags = S.flags | (S.link >= 0 && !S.nul ? FL_UL : 0);
    int sw = gfx_text_width(f, " ");
    if (L.nln == LN_MAX) line_flush(0);
    if (!L.nln) { L.x = L.lleft = left(); L.lcenter = S.center; sp = 0; }
    struct item *last = L.nln ? &L.ln[L.nln - 1] : 0;
    int hg = L.hgap;
    L.hgap = 0;
    if (last && !hg && last->font == S.font && last->flags == flags && last->col == S.col && last->link == S.link && last->node == cur_node() &&
        last->off + last->len == L.d->ntxt && last->len + n + 1 < 400) {   /* same style: one longer run */
        if (sp) arena(" ", 1);
        arena(s, n);
        last->len += n + sp;
        last->w += w + (sp ? sw : 0);
        L.x = last->x + last->w;
        return;
    }
    struct item *it = &L.ln[L.nln++];
    it->x = L.x + (sp ? sw : 0) + hg;
    it->w = w; it->h = f->height; it->font = S.font; it->flags = flags; it->col = S.col; it->link = S.link;
    it->len = n; it->off = arena(s, n); it->node = cur_node();
    L.x = it->x + w;
    if (f->ascent > L.lasc) L.lasc = f->ascent;
    if (f->height - f->ascent > L.ldesc) L.ldesc = f->height - f->ascent;
}

static void place_word(const char *s, int n) {
    const struct font *f = FT[S.font];
    int sw = gfx_text_width(f, " "), w = gfx_text_width_n(f, s, n), sp = L.pend && L.nln;
    L.pend = 0;
    if (L.nln && L.x + (sp ? sw : 0) + w + L.hgap > right()) { line_flush(0); sp = 0; }
    for (;;) {                                         /* a word wider than the line: break it */
        int fit = L.nln ? right() - L.x - (sp ? sw : 0) : right() - left();
        if (w <= fit || n <= 1) break;
        int k = 0, kw = 0;
        while (k < n) {
            uint32_t cp;
            int l = utf8_decode(s + k, n - k, &cp), cwid = gfx_text_width_n(f, s + k, l);
            if (kw + cwid > fit && k) break;
            k += l; kw += cwid;
        }
        if (k >= n) break;
        emit(s, k, kw, sp);
        line_flush(0);
        sp = 0; s += k; n -= k;
        w = gfx_text_width_n(f, s, n);
    }
    emit(s, n, w, sp);
}

static void pre_run(void) {
    if (!L.wn) return;
    emit(L.wb, L.wn, gfx_text_width_n(FT[S.font], L.wb, L.wn), 0);
    L.wn = 0;
}

static void pre_char(const char *u, int ul) {
    int maxcols = (right() - left()) / L.cw;
    if (maxcols < 8) maxcols = 8;
    if (L.col >= maxcols) { pre_run(); line_flush(0); L.col = 0; }
    if (L.wn + ul >= (int)sizeof L.wb) pre_run();
    memcpy(L.wb + L.wn, u, ul);
    L.wn += ul;
    L.col++;
}

static void flush_word(void) {
    if (S.pre) pre_run();
    else if (L.wn) { place_word(L.wb, L.wn); L.wn = 0; }
}

static void text_ws(int c) {
    if (!S.pre) { flush_word(); L.pend = 1; return; }
    if (c == '\r' || c == '\f') return;
    if (c == '\n') {
        if (L.skip_nl) { L.skip_nl = 0; return; }
        pre_run(); line_flush(1); L.col = 0;
        return;
    }
    L.skip_nl = 0;
    if (c == '\t') { do pre_char(" ", 1); while (L.col & 7); return; }
    pre_char(" ", 1);
}

static void text_char(const char *u, int ul) {
    if (S.pre) { L.skip_nl = 0; pre_char(u, ul); return; }
    if (L.wn + ul >= (int)sizeof L.wb) flush_word();
    memcpy(L.wb + L.wn, u, ul);
    L.wn += ul;
}

/* ---------- entities ---------- */
static const struct { const char *n; uint32_t cp; } ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "deg", 0xB0 },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "plusmn", 0xB1 }, { "sect", 0xA7 }, { "para", 0xB6 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "harr", 0x2194 },
    { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "micro", 0xB5 },
    { "frac12", 0xBD }, { "frac14", 0xBC }, { "frac34", 0xBE }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "agrave", 0xE0 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF },
    { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "ne", 0x2260 },
    { "le", 0x2264 }, { "ge", 0x2265 }, { "infin", 0x221E },
};

static int utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
    return 4;
}

/* s[0] == '&'. Returns UTF-8 length written to o (0 = not an entity); *used = source bytes */
static int ent_decode(const char *s, int n, char *o, int *used) {
    int i = 1;
    uint32_t cp;
    if (i < n && s[i] == '#') {
        int hex = ++i < n && (s[i] == 'x' || s[i] == 'X'), dg = 0;
        if (hex) i++;
        unsigned long v = 0;
        for (; i < n && dg < 8 && (hex ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i])); i++, dg++)
            v = v * (hex ? 16 : 10) + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (s[i] | 32) - 'a' + 10);
        if (!dg) return 0;
        if (i < n && s[i] == ';') i++;
        cp = v;
        if (cp == 0xA0 || cp < 32) cp = ' ';
        else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp < 0xA0)) cp = 0xFFFD;
    } else {
        int j = i;
        while (j < n && j - i < 10 && isalnum((unsigned char)s[j])) j++;
        if (j >= n || s[j] != ';' || j == i) return 0;
        unsigned k;
        for (k = 0; k < sizeof ENT / sizeof *ENT; k++)
            if ((int)strlen(ENT[k].n) == j - i && !strncmp(ENT[k].n, s + i, j - i)) break;
        if (k == sizeof ENT / sizeof *ENT) return 0;
        cp = ENT[k].cp;
        i = j + 1;
    }
    *used = i;
    return utf8_put(cp, o);
}

static void feed_text(const char *s, int n) {
    for (int i = 0; i < n && !L.stop; ) {
        unsigned char c = s[i];
        if (c == '&') {
            char u[8];
            int used, ul = ent_decode(s + i, n - i, u, &used);
            if (ul) { text_char(u, ul); i += used; continue; }
        }
        if (c == ' ' || (c >= 9 && c <= 13)) { text_ws(c); i++; continue; }
        if (c < 32) { i++; continue; }
        uint32_t cp;
        int l = c < 0x80 ? 1 : utf8_decode(s + i, n - i, &cp);
        text_char(s + i, l);
        i += l;
    }
}

static void feed_raw(const char *s, int n) {             /* text/plain: no markup, no entities */
    for (int i = 0; i < n && !L.stop; ) {
        unsigned char c = s[i];
        if (c == ' ' || (c >= 9 && c <= 13)) { text_ws(c); i++; continue; }
        if (c < 32) { i++; continue; }
        uint32_t cp;
        int l = c < 0x80 ? 1 : utf8_decode(s + i, n - i, &cp);
        text_char(s + i, l);
        i += l;
    }
}

/* ---------- blocks and frames ---------- */
static void block(int m) {
    flush_word();
    line_flush(0);
    L.pend = 0;
    L.hgap = 0;
    if (L.ovr >= 0) { m = L.ovr; L.ovr = -1; }
    if (m > L.gap) { L.y += m - L.gap; L.gap = m; }
}

/* an inline box (image, form control): w x h with its bottom edge `bel` below the baseline */
static struct item *place_box(int w, int h, int bel, uint8_t flags) {
    static struct item sink;
    flush_word();
    int sw = gfx_text_width(FT[S.font], " "), sp = L.pend && L.nln;
    L.pend = 0;
    if (L.nln == LN_MAX) line_flush(0);
    if (L.nln && L.x + (sp ? sw : 0) + w + L.hgap > right()) { line_flush(0); sp = 0; }
    if (!L.nln) { L.x = L.lleft = left(); L.lcenter = S.center; sp = 0; }
    if (L.nln >= LN_MAX) return &sink;
    struct item *it = &L.ln[L.nln++];
    *it = (struct item){ .x = L.x + (sp ? sw : 0) + L.hgap, .w = w, .h = h, .bel = bel, .font = S.font, .flags = flags, .col = S.col, .link = S.link, .node = cur_node() };
    L.hgap = 0;
    L.x = it->x + w;
    if (h - bel > L.lasc) L.lasc = h - bel;
    if (bel > L.ldesc) L.ldesc = bel;
    return it;
}

static struct frame *push(int tag) {
    if (L.nfr >= MAXFR) { L.pushed++; L.lastpush = &L.dummy; return &L.dummy; }
    struct frame *f = &L.fr[L.nfr++];
    memset(f, 0, sizeof *f);
    f->tag = tag;
    f->s = L.fr[L.nfr - 2].s;
    f->elidx = L.nel - 1;
    L.pushed++; L.lastpush = f;
    return f;
}

static void end_frame(struct frame *f) {
    flush_word();
    if (f->kind == 2) {
        line_flush(0);
        L.y += 6;
        int x = L.ml + f->s.indent - 8;
        add_decor(x, f->deco_y, L.R + 8 - x, L.y - f->deco_y, mix(L.d->bg, L.d->fg, 20));
        L.gap = 0;
    } else if (f->kind == 1) {
        line_flush(0);
        add_decor(L.ml + f->s.indent - 16, f->deco_y, 3, L.y - f->deco_y, mix(L.d->bg, L.d->fg, 90));
    }
    if (f->box) {                                      /* CSS box: padding, bottom border, then size the background */
        line_flush(0);
        L.y += f->pb;
        if (f->bbw) { add_decor(f->bx0, L.y, f->bx1 - f->bx0, f->bbw, f->bbc); L.y += f->bbw; }
        for (int i = 0; i < 3; i++) if (f->dv[i] > 0) L.d->dc[f->dv[i] - 1].h = L.y - f->y0;
        if (f->pb || f->bbw) L.gap = 0;
    }
    L.hgap += f->rgap;
    if (f->after) block(f->after);
}

static int pop_to(int tag) {
    int i = L.nfr - 1;
    while (i >= 1 && L.fr[i].tag != tag) i--;
    if (i < 1) return 0;
    while (L.nfr > i) { end_frame(&L.fr[L.nfr - 1]); L.nfr--; }
    return 1;
}

static void pop_while(int a, int b) {
    while (L.nfr > 1 && (L.fr[L.nfr - 1].tag == a || L.fr[L.nfr - 1].tag == b)) { end_frame(&L.fr[L.nfr - 1]); L.nfr--; }
}

/* ---------- tags ---------- */
struct attr { char *n, *v; };
static struct attr at[16];
static int nat;

static const char *A(const char *name) {
    for (int i = 0; i < nat; i++) if (!strcmp(at[i].n, name)) return at[i].v;
    return 0;
}

static void ent_inplace(char *s) {
    char *o = s;
    while (*s) {
        char u[8];
        int used, ul;
        if (*s == '&' && (ul = ent_decode(s, strlen(s), u, &used))) { memcpy(o, u, ul); o += ul; s += used; }
        else *o++ = *s++;
    }
    *o = 0;
}
void html_unescape(char *s) { ent_inplace(s); }

static void parse_attrs(char *p) {
    nat = 0;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (nat < 16) {
        while (*p && (isspace((unsigned char)*p) || *p == '/')) p++;
        if (!*p) break;
        char *n = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '/') { *p = tolower((unsigned char)*p); p++; }
        char *v = "";
        int more = *p != 0;
        char c = *p;
        *p = 0;
        if (more) p++;
        if (c && isspace((unsigned char)c)) { while (*p && isspace((unsigned char)*p)) p++; c = *p == '=' ? '=' : 0; if (c) p++; }
        if (c == '=') {
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '"' || *p == '\'') { char q = *p++; v = p; while (*p && *p != q) p++; }
            else { v = p; while (*p && !isspace((unsigned char)*p)) p++; }
            if (*p) *p++ = 0;
            ent_inplace(v);
        }
        if (*n) { at[nat].n = n; at[nat].v = v; nat++; }
    }
}

static int parse_color(const char *s, color_t *out) { return css_color(s, out, 1) && *out; }

static void align(struct frame *f) {
    const char *a = A("align");
    if (a && !strcasecmp(a, "center")) f->s.center = 1;
    else if (a && !strcasecmp(a, "right")) f->s.center = 2;
}

static void set_font(struct frame *f, int id) { f->s.font = id; }

/* ---------- form controls ---------- */
static int tw(const struct font *f, const char *s) { return gfx_text_width(f, s); }

/* <option>s inside a <select> body */
static void parse_options(const char *s, int n, struct ctl *c) {
    struct doc *d = L.d;
    c->o0 = d->nop;
    for (int i = 0; i < n; i++) {
        if (s[i] != '<' || i + 7 > n || strncasecmp(s + i + 1, "option", 6) || isalnum((unsigned char)s[i + 7])) continue;
        char tb[512];
        int j = i + 1, tl = 0;
        for (; j < n && s[j] != '>'; j++) if (tl < (int)sizeof tb - 1) tb[tl++] = s[j];
        tb[tl] = 0;
        parse_attrs(tb);
        const char *val = A("value");
        int sel = A("selected") != 0;
        char vbuf[200];
        vbuf[0] = 0;
        if (val) { strlcpy(vbuf, val, sizeof vbuf); }
        int ls = j + 1, le = ls;
        while (le < n && s[le] != '<') le++;
        char lab[200];
        int k = 0, sp = 0;
        for (int q = ls; q < le && k < (int)sizeof lab - 1; q++) {
            if (isspace((unsigned char)s[q])) { if (k && !sp) lab[k++] = ' '; sp = 1; }
            else { lab[k++] = s[q]; sp = 0; }
        }
        while (k && lab[k - 1] == ' ') k--;
        lab[k] = 0;
        ent_inplace(lab);
        GROW(d->op, d->nop, d->opcap);
        if (d->nop >= d->opcap) return;
        d->op[d->nop++] = (struct opt){ astr(lab), astr(val ? vbuf : lab) };
        if (sel) c->rows = d->nop - 1 - c->o0;
        i = le - 1;
    }
    c->no = d->nop - c->o0;
}

static void open_ctl(int t) {
    struct doc *d = L.d;
    const char *ty = A("type"), *v;
    struct ctl c = { .form = L.curform, .ord = L.nord++, .item = -1, .node = cur_node() };
    char name[120], val[400], lab[200];
    int avail = right() - left(), w = 0, h = 26, bel = 9, cols;
    strlcpy(name, (v = A("name")) ? v : "", sizeof name);
    strlcpy(val, (v = A("value")) ? v : "", sizeof val);
    cols = (v = A("size")) && atoi(v) > 0 ? atoi(v) : 20;
    lab[0] = 0;
    c.chk = A("checked") != 0;
    if (t == T_INPUT) {
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
        if (c.kind == C_BTN) strlcpy(lab, val[0] ? val : c.sub == 1 ? "Submit" : c.sub == 2 ? "Reset" : "Button", sizeof lab);
    } else if (t == T_BUTTON) {
        c.kind = C_BTN;
        c.sub = ty && !strcasecmp(ty, "reset") ? 2 : ty && !strcasecmp(ty, "button") ? 0 : 1;
        int k = 0, sp = 0;                                  /* label: the text of the body, tags dropped */
        for (int i = 0; i < L.clen && k < (int)sizeof lab - 1; i++) {
            if (L.content[i] == '<') { while (i < L.clen && L.content[i] != '>') i++; continue; }
            if (isspace((unsigned char)L.content[i])) { if (k && !sp) lab[k++] = ' '; sp = 1; }
            else { lab[k++] = L.content[i]; sp = 0; }
        }
        while (k && lab[k - 1] == ' ') k--;
        lab[k] = 0;
        ent_inplace(lab);
        if (!lab[0]) strcpy(lab, "Button");
    } else if (t == T_TEXTAREA) {
        c.kind = C_AREA;
        c.cols = (v = A("cols")) && atoi(v) > 0 ? atoi(v) : 30;
        c.rows = (v = A("rows")) && atoi(v) > 0 ? atoi(v) : 4;
        if (c.cols > 120) c.cols = 120;
        if (c.rows > 30) c.rows = 30;
    } else c.kind = C_SELECT;
    c.name = astr(name);
    if (t == T_TEXTAREA) {
        char *tv = malloc(L.clen + 1);                      /* initial text: raw body, entities decoded, first newline dropped */
        if (tv) {
            int k = 0, i = 0;
            if (i < L.clen && L.content[i] == '\r') i++;
            if (i < L.clen && L.content[i] == '\n') i++;
            for (; i < L.clen; i++) tv[k++] = L.content[i];
            tv[k] = 0;
            ent_inplace(tv);
            c.val = astr(tv);
            free(tv);
        } else c.val = astr("");
    } else c.val = astr((c.kind == C_CHECK || c.kind == C_RADIO) && !val[0] ? "on" : val);
    if (c.kind == C_BTN) c.o0 = astr(lab);
    if (c.kind == C_SELECT) {
        c.rows = 0;
        parse_options(L.content, L.clen, &c);               /* clobbers the attribute table: done with it */
        int mw = 40;
        for (int i = 0; i < c.no; i++) { int x = tw(&font_ui, d->txt + d->op[c.o0 + i].lab); if (x > mw) mw = x; }
        w = mw + 34;
    }
    switch (c.kind) {
    case C_HIDDEN: break;
    case C_TEXT: case C_PASS: w = cols * 8 + 14; break;
    case C_BTN: w = tw(&font_ui, lab) + 28; break;
    case C_CHECK: case C_RADIO: w = h = 16; bel = 2; break;
    case C_AREA: w = c.cols * 8 + 14; h = c.rows * 18 + 10; bel = 4; break;
    }
    if (w > avail && avail > 40) w = avail;
    GROW(d->ct, d->nct, d->ctcap);
    if (d->nct >= d->ctcap) return;
    int ci = d->nct++;
    d->ct[ci] = c;
    if (c.kind == C_HIDDEN) return;
    struct item *it = place_box(w, h, bel, FL_CTL);
    it->ref = ci;
}

static void open_tag(int t) {
    flush_word();
    struct frame *f;
    const char *v;
    if ((v = A("id")) || (t == T_A && (v = A("name")))) anchor_add(v);
    switch (t) {
    case T_BODY:
        if ((v = A("bgcolor"))) parse_color(v, &L.d->bg);
        if ((v = A("text"))) { parse_color(v, &L.d->fg); L.fr[0].s.col = L.d->fg; }
        if ((v = A("link"))) parse_color(v, &L.d->linkc);
        else if (dark(L.d->bg)) { L.d->linkc = RGB(0x8C, 0xB4, 0xFF); L.d->hoverc = RGB(0xFF, 0x9A, 0xD0); }
        if (dark(L.d->bg) && !A("text")) { L.d->fg = RGB(0xE8, 0xE6, 0xF5); L.fr[0].s.col = L.d->fg; }
        L.em = dark(L.d->bg) ? RGB(0xFF, 0xA0, 0xD8) : RGB(0x8A, 0x1C, 0x8A);
        L.dim = mix(L.d->fg, L.d->bg, 130);
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
        static const uint8_t font[] = { F_TITLE, F_TITLE, F_UIB, F_UIB, F_UIB, F_UIB };
        static const uint8_t mb[] = { 18, 16, 14, 12, 10, 10 }, ma[] = { 10, 8, 6, 5, 4, 4 };
        int k = t - T_H1;
        block(mb[k]);
        f = push(t); set_font(f, font[k]); f->after = ma[k]; align(f);
        if (t == T_H6) f->s.col = L.dim;
        break;
    }
    case T_P:
        pop_while(T_P, T_P);
        block(10);
        f = push(t); f->after = 10; align(f);
        break;
    case T_DIV: block(0); f = push(t); align(f); break;
    case T_CENTER: block(0); f = push(t); f->s.center = 1; break;
    case T_UL: case T_OL: {
        int nested = 0;
        for (int i = 1; i < L.nfr; i++) if (L.fr[i].tag == T_UL || L.fr[i].tag == T_OL) nested = 1;
        block(nested ? 3 : 8);
        f = push(t); f->s.indent += 28; f->after = nested ? 3 : 8; f->ordered = t == T_OL;
        if ((v = A("start")) && f->ordered) f->n = atoi(v) - 1;
        break;
    }
    case T_LI: {
        block(3);
        struct frame *lf = 0;
        for (int i = L.nfr - 1; i >= 1 && !lf; i--) if (L.fr[i].tag == T_UL || L.fr[i].tag == T_OL) lf = &L.fr[i];
        char m[16] = "\xE2\x80\xA2";
        if (lf && lf->ordered) snprintf(m, sizeof m, "%d.", ++lf->n);
        const struct font *mf = FT[S.font];
        int w = gfx_text_width(mf, m);
        if (S.nomark) break;
        struct item *it = doc_item();
        *it = (struct item){ .x = left() - 8 - w, .y = L.y, .w = w, .h = mf->height, .font = S.font, .len = strlen(m), .col = L.dim, .link = -1, .off = arena(m, strlen(m)) };
        if (it->x < 2) it->x = 2;
        break;
    }
    case T_DL: block(8); f = push(t); f->after = 8; break;
    case T_DT: pop_while(T_DT, T_DD); block(4); f = push(t); set_font(f, F_UIB); break;
    case T_DD: pop_while(T_DT, T_DD); block(2); f = push(t); f->s.indent += 32; break;
    case T_BQ:
        block(10);
        f = push(t); f->s.indent += 28; f->after = 10; f->kind = 1; f->deco_y = L.y;
        break;
    case T_PRE:
        block(10);
        f = push(t); set_font(f, F_MONO); f->s.pre = 1; f->s.center = 0; f->after = 10; f->kind = 2; f->deco_y = L.y;
        L.y += 6; L.skip_nl = 1; L.col = 0;
        break;
    case T_CODE: f = push(t); set_font(f, f->s.font == F_UIB ? F_MONOB : F_MONO); break;
    case T_B: f = push(t); set_font(f, f->s.font == F_UI ? F_UIB : f->s.font == F_MONO ? F_MONOB : f->s.font); break;
    case T_I: f = push(t); f->s.col = L.em; break;
    case T_U: f = push(t); f->s.flags |= FL_UL; break;
    case T_S: f = push(t); f->s.flags |= FL_STRIKE; break;
    case T_A:
        f = push(t);
        if ((v = A("href")) && strncasecmp(v, "javascript:", 11)) { f->s.link = link_add(v); if (f->s.link >= 0) f->s.col = L.d->linkc; }
        break;
    case T_FONT:
        f = push(t);
        if ((v = A("color"))) parse_color(v, &f->s.col);
        break;
    case T_BR: line_flush(L.nln == 0); break;
    case T_HR:
        block(8);
        add_decor(left(), L.y, right() - left(), 2, mix(L.d->bg, L.d->fg, 70));
        L.y += 2; L.gap = 0;
        block(8);
        break;
    case T_IMG: {
        const char *src = A("src"), *alt = A("alt");
        char u[URL_MAX];
        int idx = -1, iw = 0, ih = 0, st = IMG_FAILED;
        if (src && *src && strncasecmp(src, "data:", 5)) {
            url_resolve(L.base, src, u, sizeof u);
            idx = img_ref(u);
            st = img_info(idx, &iw, &ih);
        }
        int avail = right() - left(), W = 0, H = 0;
        const struct cprops *cp = L.cp;
        const char *aw = A("width"), *ah = A("height");
        if (aw && atoi(aw) > 0) W = strchr(aw, '%') ? avail * atoi(aw) / 100 : atoi(aw);
        if (ah && atoi(ah) > 0 && !strchr(ah, '%')) H = atoi(ah);
        if (cp && (cp->set & 1u << P_WIDTH) && cp->k[P_WIDTH] != K_AUTO && cp->v[P_WIDTH] > 0) W = cp->k[P_WIDTH] == K_PCT ? avail * cp->v[P_WIDTH] / 100 : cp->v[P_WIDTH];
        if (cp && (cp->set & 1u << P_HEIGHT) && cp->k[P_HEIGHT] == K_PX && cp->v[P_HEIGHT] > 0) H = cp->v[P_HEIGHT];
        if (st == IMG_READY) {
            if (!W && !H) { W = iw; H = ih; }
            else if (!H) H = (int)((long)ih * W / iw);
            else if (!W) W = (int)((long)iw * H / ih);
        } else if (st == IMG_FAILED && !(W && H)) {         /* broken and unsized: just the alt text, like a text browser */
            char buf[200];
            snprintf(buf, sizeof buf, "[image%s%s]", alt && *alt ? ": " : "", alt && *alt ? alt : "");
            color_t keep = S.col;
            S.col = L.dim;
            feed_text(buf, strlen(buf));
            flush_word();
            S.col = keep;
            break;
        } else {                                            /* still loading: reserve a box */
            if (!W && !H) { W = 96; H = 64; }
            else if (!H) H = W * 3 / 4;
            else if (!W) W = H * 4 / 3;
        }
        if (cp && (cp->set & 1u << P_MAXW) && cp->k[P_MAXW] != K_AUTO) {
            int m = cp->k[P_MAXW] == K_PCT ? avail * cp->v[P_MAXW] / 100 : cp->v[P_MAXW];
            if (m > 0 && W > m) { H = (int)((long)H * m / W); W = m; }
        }
        if (W > avail && avail > 8) { H = (int)((long)H * avail / W); W = avail; }
        if (W < 1) W = 1;
        if (H < 1) H = 1;
        if (W > 8192 || H > 8192) { W = W > 8192 ? 8192 : W; H = H > 8192 ? 8192 : H; }
        struct item *it = place_box(W, H, 0, FL_IMG);
        it->ref = idx;
        if (alt && *alt) { it->len = strlen(alt) > 200 ? 200 : strlen(alt); it->off = arena(alt, it->len); }
        break;
    }
    case T_FORM: {
        block(0);
        f = push(t); align(f);
        GROW(L.d->fm, L.d->nfm, L.d->fmcap);
        if (L.d->nfm < L.d->fmcap) {
            const char *m = A("method");
            L.d->fm[L.d->nfm] = (struct form){ .action = astr(A("action") ? A("action") : ""), .post = m && !strcasecmp(m, "post"), .node = cur_node() };
            L.curform = L.d->nfm++;
        }
        break;
    }
    case T_INPUT: case T_TEXTAREA: case T_SELECT: case T_BUTTON: open_ctl(t); break;
    case T_TABLE: block(8); f = push(t); f->after = 8; L.cell_n = 0; break;
    case T_TR: pop_while(T_TH, T_TH); block(2); L.cell_n = 0; break;
    case T_TD: case T_TH:
        pop_while(T_TH, T_TH);
        if (L.cell_n++) {
            color_t keep = S.col;
            L.pend = 1; S.col = L.dim;
            place_word("|", 1);
            S.col = keep; L.pend = 1;
        }
        if (t == T_TH) { f = push(t); set_font(f, f->s.font == F_UI ? F_UIB : f->s.font); }
        break;
    case T_CAP: block(4); f = push(t); set_font(f, F_UIB); f->s.center = 1; f->after = 4; break;
    }
}

static void close_tag(int t) {
    switch (t) {
    case T_BR: open_tag(T_BR); break;
    case T_P: if (!pop_to(T_P)) block(10); break;
    case T_LI: block(0); break;
    case T_TD: case T_TH: pop_while(T_TH, T_TH); break;
    case T_TR: pop_while(T_TH, T_TH); block(0); break;
    case T_DT: case T_DD: pop_while(T_DT, T_DD); break;
    case T_DL: pop_while(T_DT, T_DD); pop_to(T_DL); break;
    case T_TABLE: pop_while(T_TH, T_TH); pop_to(T_TABLE); break;
    case T_FORM: pop_to(T_FORM); L.curform = -1; break;
    case T_INPUT: case T_TEXTAREA: case T_SELECT: case T_BUTTON: case T_CSS: break;
    case T_NONE: case T_HTML: case T_BODY: case T_HR: case T_IMG: case T_RAW: case T_TITLE: break;
    default: pop_to(t);
    }
}

/* ---------- elements: CSS cascade + frames ---------- */
#define PM(p) (1u << (p))
static int is_void(const char *n) {
    static const char *const V[] = { "br", "hr", "img", "input", "meta", "link", "area", "base", "col", "param", "source", "track", "wbr", "embed", 0 };
    for (int i = 0; V[i]; i++) if (!strcmp(n, V[i])) return 1;
    return 0;
}

static int is_block_tag(int t) {
    switch (t) {
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_P: case T_DIV: case T_CENTER: case T_UL: case T_OL:
    case T_LI: case T_DL: case T_DT: case T_DD: case T_BQ: case T_PRE: case T_TABLE: case T_TR: case T_CAP: case T_FORM: case T_BODY: return 1;
    }
    return 0;
}

/* close element e and everything inside it (frames included) */
static void el_close_to(int e) {
    while (L.nfr > 1 && L.fr[L.nfr - 1].elidx >= e) { end_frame(&L.fr[L.nfr - 1]); L.nfr--; }
    if (e < L.nel) L.nel = e;
}

/* <li> after <li>, <td> after <td>...: the old element ends implicitly */
static void el_implicit(const char *n) {
    const char *set = !strcmp(n, "li") ? "li" : !strcmp(n, "p") ? "p" : !strcmp(n, "td") || !strcmp(n, "th") ? "td th" :
                      !strcmp(n, "tr") ? "td th tr" : !strcmp(n, "dt") || !strcmp(n, "dd") ? "dt dd" : 0;
    while (set && L.nel) {
        const char *t = L.el[L.nel - 1].name;
        int l = strlen(t), hit = 0;
        for (const char *q = set; *q && !hit; q += strcspn(q, " ") + (q[strcspn(q, " ")] != 0)) hit = (int)strcspn(q, " ") == l && !strncmp(q, t, l);
        if (!hit) break;
        el_close_to(L.nel - 1);
    }
}

static int len_px(const struct cprops *p, int prop, int avail, int dflt) {      /* px, % of avail; `dflt` when unset/auto */
    if (!(p->set & PM(prop)) || p->k[prop] == K_AUTO) return dflt;
    return p->k[prop] == K_PCT ? avail * p->v[prop] / 100 : p->v[prop];
}

/* apply the cascaded properties to the element's frame */
static void css_apply(struct frame *f, const struct cprops *p, int blk, int isbody, int ishtml, int top_done) {
    uint32_t set = p->set;
    struct style *st = &f->s;
    struct doc *d = L.d;
    if (set & PM(P_COLOR)) st->col = p->v[P_COLOR];
    else if ((set & PM(P_ITALIC)) && p->v[P_ITALIC]) st->col = L.em;
    if (set & PM(P_BOLD)) {
        if (p->v[P_BOLD]) st->font = st->font == F_UI ? F_UIB : st->font == F_MONO ? F_MONOB : st->font;
        else st->font = st->font == F_UIB ? F_UI : st->font == F_MONOB ? F_MONO : st->font;
    }
    if ((set & PM(P_MONO)) && p->v[P_MONO]) st->font = st->font == F_UIB || st->font == F_MONOB ? F_MONOB : F_MONO;
    if ((set & PM(P_FSIZE)) && p->v[P_FSIZE] >= 20 && (st->font == F_UI || st->font == F_UIB)) st->font = F_TITLE;
    if (set & PM(P_ALIGN)) st->center = p->v[P_ALIGN];
    if (set & PM(P_DECO)) {
        int m = p->v[P_DECO];
        st->flags = (st->flags & ~(FL_UL | FL_STRIKE)) | (m & 1 ? FL_UL : 0) | (m & 2 ? FL_STRIKE : 0);
        st->nul = !(m & 1);
    }
    if ((set & PM(P_LIST)) && p->v[P_LIST]) st->nomark = 1;
    if (isbody || ishtml) {
        if ((set & PM(P_BG)) && p->v[P_BG]) {
            d->bg = p->v[P_BG];
            if (dark(d->bg)) {
                d->linkc = RGB(0x8C, 0xB4, 0xFF); d->hoverc = RGB(0xFF, 0x9A, 0xD0);
                if (!(set & PM(P_COLOR))) { d->fg = RGB(0xE8, 0xE6, 0xF5); L.fr[0].s.col = st->col = d->fg; }
                L.em = RGB(0xFF, 0xA0, 0xD8);
            }
            L.dim = mix(d->fg, d->bg, 130);
        }
        if (set & PM(P_COLOR)) { d->fg = p->v[P_COLOR]; L.dim = mix(d->fg, d->bg, 130); }
    }
    if (!blk && f != &L.dummy) {                                // inline: side margins and padding become gaps
        int ml = len_px(p, P_ML, 0, 0) + len_px(p, P_PL, 0, 0), mr = len_px(p, P_MR, 0, 0) + len_px(p, P_PR, 0, 0);
        if (ml > 0) L.hgap += ml;
        if (mr > 0) f->rgap = mr;
    }
    if (!blk || f == &L.dummy) return;
    /* ---- box model ---- */
    int idx = f - L.fr;
    const struct style *par = &L.fr[idx - 1].s;
    int extra = st->indent - par->indent;                   /* indent the builtin tag added (lists, quotes) */
    int pw = L.R - L.ml - par->rin - par->indent;           /* width of the parent's content box */
    int ml = len_px(p, P_ML, pw, 0), mr = len_px(p, P_MR, pw, 0);
    int pl = (set & PM(P_PL)) ? len_px(p, P_PL, pw, 0) : extra, pr = len_px(p, P_PR, pw, 0);
    int bt = (set & PM(P_BWT)) ? p->v[P_BWT] : 0, bb = (set & PM(P_BWB)) ? p->v[P_BWB] : 0;
    int bl = (set & PM(P_BWL)) ? p->v[P_BWL] : 0, br = (set & PM(P_BWR)) ? p->v[P_BWR] : 0;
    int pt = len_px(p, P_PT, pw, 0), pb = len_px(p, P_PB, pw, 0);
    int mt = len_px(p, P_MT, pw, 0), mb = len_px(p, P_MB, pw, 0);
    if (ml < 0) ml = 0;
    if (mr < 0) mr = 0;
    if (mt < 0) mt = 0;
    if (pl < 0) pl = 0;
    if (!top_done && !isbody) block(mt);
    int x0 = L.ml + par->indent + ml, x1 = L.R - par->rin - mr;
    int avail = x1 - x0, wbox = -1;
    int edge = pl + pr + bl + br;
    if ((set & PM(P_WIDTH)) && p->k[P_WIDTH] != K_AUTO) wbox = len_px(p, P_WIDTH, avail, 0) + edge;
    if ((set & PM(P_MAXW)) && p->k[P_MAXW] != K_AUTO) {
        int m = len_px(p, P_MAXW, avail, 0) + edge;
        if (wbox < 0 || wbox > m) wbox = m;
    }
    if (wbox >= 0 && wbox < avail) {
        int ex = avail - wbox, al = (set & PM(P_ML)) && p->k[P_ML] == K_AUTO, ar = (set & PM(P_MR)) && p->k[P_MR] == K_AUTO;
        if (al && ar) { x0 += ex / 2; x1 -= ex - ex / 2; }
        else if (al) x0 += ex;
        else x1 -= ex;
    }
    if (x1 - x0 < 40) x1 = x0 + 40;
    st->indent = x0 - L.ml + bl + pl;
    st->rin = L.R - x1 + br + pr;
    if (set & PM(P_MB)) f->after = mb;
    f->box = 1; f->y0 = L.y; f->bx0 = x0; f->bx1 = x1; f->pb = pb;
    if (isbody) {                                           /* the body box is the page: margins and padding just move the text */
        L.y += (mt > 0 ? mt : 0) + pt;
        if (mt || pt) L.gap = 0;
        f->box = 0;
        return;
    }
    color_t fg = st->col;
    if ((set & PM(P_BG)) && p->v[P_BG]) f->dv[0] = add_decor(x0, L.y, x1 - x0, 0, p->v[P_BG]) + 1;
    if (bt) add_decor(x0, L.y, x1 - x0, bt, (set & PM(P_BCT)) ? (color_t)p->v[P_BCT] : mix(d->bg, fg, 90));
    if (bl) f->dv[1] = add_decor(x0, L.y, bl, 0, (set & PM(P_BCL)) ? (color_t)p->v[P_BCL] : mix(d->bg, fg, 90)) + 1;
    if (br) f->dv[2] = add_decor(x1 - br, L.y, br, 0, (set & PM(P_BCR)) ? (color_t)p->v[P_BCR] : mix(d->bg, fg, 90)) + 1;
    f->bbw = bb;
    f->bbc = (set & PM(P_BCB)) ? (color_t)p->v[P_BCB] : mix(d->bg, fg, 90);
    for (int i = 0; i < 3; i++) if (f->dv[i] < 0) f->dv[i] = 0;
    if (bt || pt) { L.y += bt + pt; L.gap = 0; }
}

/* open an element: cascade, display:none, frames. Returns 1 when the caller should skip its whole subtree. */
static int start_el(int t, const char *name, const char *content, int clen) {
    int vd = is_void(name), pushed = 0;
    const char *v;
    el_implicit(name);
    const char *id = A("id"), *cls = A("class"), *sty = A("style");
    if (L.nel < 64) {
        struct el *e = &L.el[L.nel++];
        strlcpy(e->name, name, sizeof e->name);
        strlcpy(e->id, id ? id : "", sizeof e->id);
        strlcpy(e->cls, cls ? cls : "", sizeof e->cls);
        e->js = (v = A("data-vjs")) ? atoi(v) : 0;
        pushed = 1;
    }
    struct cprops cp;
    memset(&cp, 0, sizeof cp);
    if (pushed && (css_any() || (sty && *sty))) css_cascade(L.el, L.nel, sty, &cp);
    int disp = (cp.set & PM(P_DISPLAY)) ? cp.v[P_DISPLAY] : 0;
    if (A("hidden")) disp = D_NONE;
    if (disp == D_NONE) {
        L.nel -= pushed;
        return !vd && !content;
    }
    if (disp == D_INLINE && is_block_tag(t) && t != T_BODY) t = T_NONE;
    if (pushed) L.el[L.nel - 1].tag = t;
    int blk = disp == D_BLOCK || (is_block_tag(t) && disp != D_INLINE);
    int isbody = !strcmp(name, "body"), ishtml = !strcmp(name, "html");
    L.ovr = (cp.set & PM(P_MT)) && cp.k[P_MT] == K_PX && cp.v[P_MT] >= 0 && is_block_tag(t) && !isbody ? cp.v[P_MT] : -1;
    L.cp = &cp; L.pushed = 0; L.content = content; L.clen = clen;
    open_tag(t);
    L.ovr = -1; L.cp = 0;
    struct frame *f = L.pushed ? L.lastpush : 0;
    int top_done = f != 0 || (is_block_tag(t) && !isbody);
    if (!f && !vd && cp.set && pushed) f = push(T_CSS);     /* no frame of its own (span, li, td, body): give it one */
    if (f && cp.set) css_apply(f, &cp, blk, isbody, ishtml, top_done);
    if (vd) L.nel -= pushed;
    return 0;
}

static int tag_id(const char *n) {
    for (unsigned i = 0; i < sizeof TAGS / sizeof *TAGS; i++) if (!strcmp(TAGS[i].n, n)) return TAGS[i].t;
    return T_NONE;
}

/* offset just past "</name ...>" searching from p, or n */
static int skip_raw(const char *src, int p, int n, const char *name, int *content_end) {
    int nl = strlen(name);
    for (int i = p; i < n; i++) {
        if (src[i] != '<' || i + 2 + nl > n || src[i + 1] != '/' || strncasecmp(src + i + 2, name, nl)) continue;
        *content_end = i;
        while (i < n && src[i] != '>') i++;
        return i < n ? i + 1 : n;
    }
    *content_end = n;
    return n;
}

static void capture_title(const char *s, int n) {
    char *o = L.d->title;
    int k = 0, sp = 0;
    for (int i = 0; i < n && k < (int)sizeof L.d->title - 5; ) {
        unsigned char c = s[i];
        char u[8];
        int used, ul;
        if (c == '&' && (ul = ent_decode(s + i, n - i, u, &used))) { memcpy(o + k, u, ul); k += ul; i += used; sp = 0; continue; }
        if (isspace(c)) { if (k && !sp) o[k++] = ' '; sp = 1; i++; continue; }
        sp = 0;
        o[k++] = c; i++;
    }
    while (k && o[k - 1] == ' ') k--;
    o[k] = 0;
}

/* offset just past the </name> that closes the element whose start tag ended at p (nesting of the same name counted) */
static int skip_subtree(const char *src, int p, int n, const char *name) {
    int nl = strlen(name), depth = 1;
    int implicit = !strcmp(name, "li") || !strcmp(name, "p") || !strcmp(name, "td") || !strcmp(name, "th") || !strcmp(name, "tr") ||
                   !strcmp(name, "dt") || !strcmp(name, "dd");
    for (int i = p; i < n; i++) {
        if (src[i] != '<') continue;
        int cl = i + 1 < n && src[i + 1] == '/', st = i + 1 + cl;
        if (st + nl > n || strncasecmp(src + st, name, nl) || isalnum((unsigned char)src[st + nl])) continue;
        int j = st + nl;
        while (j < n && src[j] != '>') j++;
        if (cl) { if (--depth == 0) return j < n ? j + 1 : n; }
        else if (implicit) return i;                         /* <li> after <li> closes the first */
        else if (j > 0 && src[j - 1] != '/') depth++;
        i = j;
    }
    return n;
}

void doc_layout(struct doc *d, const char *src, size_t n, int is_html, int width, const char *base) {
    char title[sizeof d->title];
    strlcpy(title, d->title, sizeof title);          /* a title survives relayout */
    doc_free(d);
    memset(&L, 0, sizeof L);
    L.d = d;
    L.base = base;
    L.curform = -1;
    L.ovr = -1;
    css_reset();
    d->bg = RGB(0xFF, 0xFF, 0xFF); d->fg = RGB(0x1A, 0x1A, 0x22);
    d->linkc = RGB(0x0B, 0x57, 0xD0); d->hoverc = RGB(0xC0, 0x1F, 0x7A);
    L.em = RGB(0x8A, 0x1C, 0x8A);
    L.dim = mix(d->fg, d->bg, 130);
    int colw = width > 920 ? 880 : width - 40;
    if (colw < 60) colw = 60;
    L.ml = (width - colw) / 2;
    L.R = L.ml + colw;
    L.cw = font_mono.g['M' - 32].adv;
    L.y = 14;
    L.gap = 9999;
    L.nfr = 1;
    L.fr[0].s = (struct style){ .font = F_UI, .link = -1, .col = d->fg };
    if (!is_html) {
        L.fr[0].s.font = F_MONO;
        L.fr[0].s.pre = 1;
        feed_raw(src, n);
    } else {
        char tb[1024];
        for (size_t i = 0; i < n && !L.stop; ) {
            if (src[i] != '<') {
                const char *e = memchr(src + i, '<', n - i);
                size_t j = e ? (size_t)(e - src) : n;
                feed_text(src + i, j - i);
                i = j;
                continue;
            }
            if (i + 4 <= n && !strncmp(src + i, "<!--", 4)) {
                size_t j = i + 4;
                while (j + 3 <= n && strncmp(src + j, "-->", 3)) j++;
                i = j + 3 <= n ? j + 3 : n;
                continue;
            }
            if (i + 1 < n && (src[i + 1] == '!' || src[i + 1] == '?')) {
                const char *e = memchr(src + i, '>', n - i);
                i = e ? (size_t)(e - src) + 1 : n;
                continue;
            }
            size_t k = i + 1;
            int closing = k < n && src[k] == '/';
            k += closing;
            if (k >= n || !isalpha((unsigned char)src[k])) { feed_text("<", 1); i++; continue; }
            int tl = 0;
            char q = 0, prev = 0;
            size_t j = k;
            for (; j < n; j++) {
                char c = src[j];
                if (q) { if (c == q) q = 0; }
                else if ((c == '"' || c == '\'') && prev == '=') q = c;
                else if (c == '>') break;
                if (!isspace((unsigned char)c)) prev = c;
                if (tl < (int)sizeof tb - 1) tb[tl++] = c;
            }
            tb[tl] = 0;
            i = j < n ? j + 1 : n;
            char name[16];
            int nl = 0;
            while (nl < 15 && tb[nl] && !isspace((unsigned char)tb[nl]) && tb[nl] != '/') { name[nl] = tolower((unsigned char)tb[nl]); nl++; }
            name[nl] = 0;
            int t = tag_id(name);
            if (closing) {
                int e = L.nel - 1;
                while (e >= 0 && strcmp(L.el[e].name, name)) e--;
                close_tag(e >= 0 ? L.el[e].tag : t);              /* the tag it was opened as (display:inline turns li into a span) */
                if (e >= 0) el_close_to(e);
                continue;
            }
            parse_attrs(tb);
            if (t == T_RAW || t == T_TITLE) {
                int ce;
                int next = skip_raw(src, i, n, name, &ce);
                if (t == T_TITLE) capture_title(src + i, ce - i);
                else if (!strcmp(name, "style") && !(A("media") && strstr(A("media"), "print"))) css_add(src + i, ce - i);
                i = next;
                continue;
            }
            const char *content = 0;
            int clen = 0;
            if (t == T_TEXTAREA || t == T_SELECT || t == T_BUTTON) {
                int ce, next = skip_raw(src, i, n, name, &ce);
                content = src + i; clen = ce - i;
                i = next;
            }
            if (start_el(t, name, content, clen)) i = skip_subtree(src, i, n, name);
        }
    }
    flush_word();
    while (L.nfr > 1) { end_frame(&L.fr[L.nfr - 1]); L.nfr--; }
    line_flush(0);
    d->height = L.y + 24;
    if (!d->title[0]) strlcpy(d->title, title, sizeof d->title);
}

/* ---------- drawing and hit testing ---------- */
static int first_item(const struct doc *d, int ytop) {    /* first item with y >= ytop - 40 (lines are roughly sorted) */
    int lo = 0, hi = d->nit;
    ytop -= d->maxh > 40 ? d->maxh : 40;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->it[mid].y < ytop) lo = mid + 1; else hi = mid;
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
    for (int i = first_item(d, y - 40); i < d->nit && d->it[i].y <= y + 40; i++) {
        const struct item *it = &d->it[i];
        if (!is_text(it)) continue;
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
    for (int i = first_item(d, y); i < d->nit && d->it[i].y <= y; i++) {
        const struct item *it = &d->it[i];
        if ((it->flags & FL_CTL) && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->ref;
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

void doc_draw(struct ui *ui, const struct doc *d, int vx, int vy, int vw, int vh, int scroll, int hover,
              const struct docmark *mk, int focus_ord, int caret) {
    struct canvas cc = *ui->c;
    canvas_clip(&cc, vx, vy, vw, vh);
    gfx_fill(&cc, vx, vy, vw, vh, d->bg);
    for (int i = 0; i < d->ndc; i++) {
        const struct decor *r = &d->dc[i];
        int y = vy + r->y - scroll;
        if (y < vy + vh && y + r->h > vy) gfx_fill(&cc, vx + r->x, y, r->w, r->h, r->col);
    }
    int nl = mk && mk->find ? strlen(mk->find) : 0;
    for (int i = first_item(d, scroll); i < d->nit; i++) {
        const struct item *it = &d->it[i];
        int y = vy + it->y - scroll;
        if (y >= vy + vh) break;
        if (y + it->h < vy) continue;
        if (it->flags & FL_IMG) { draw_box(&cc, d, it, vx + it->x, y); continue; }
        if (it->flags & FL_CTL) {
            const struct ctl *t = &d->ct[it->ref];
            ctl_draw(&cc, d, t, vx + it->x, y, it->w, it->h, t->ord == focus_ord, caret);
            continue;
        }
        const struct font *f = FT[it->font];
        const char *txt = d->txt + it->off;
        if (mk && mk->from_it >= 0 && i >= mk->from_it && i <= mk->to_it) {      /* selection */
            int s = i == mk->from_it ? mk->from_ch : 0, e = i == mk->to_it ? mk->to_ch : it->len;
            if (e > s) { int x0 = run_x(d, it, s), x1 = run_x(d, it, e); gfx_fill(&cc, vx + it->x + x0, y, x1 - x0 + (e == it->len ? 3 : 0), it->h, RGBA(0x4C, 0x8D, 0xFF, 120)); }
        }
        for (int k = 0; nl && k + nl <= it->len; k++) {                          /* find matches */
            if (!ci_at(txt, it->len, k, mk->find, nl)) continue;
            int x0 = run_x(d, it, k), x1 = run_x(d, it, k + nl), act = i == mk->find_it && k == mk->find_off;
            gfx_fill(&cc, vx + it->x + x0, y, x1 - x0, it->h, act ? RGBA(0xFF, 0x96, 0x1E, 200) : RGBA(0xFF, 0xE0, 0x3C, 150));
            k += nl - 1;
        }
        color_t col = it->link >= 0 && it->link == hover ? d->hoverc : it->col;
        gfx_text_n(&cc, f, vx + it->x, y, txt, it->len, col);
        if (it->flags & FL_UL) gfx_fill(&cc, vx + it->x, y + f->ascent + 2, it->w, 1, col);
        if (it->flags & FL_STRIKE) gfx_fill(&cc, vx + it->x, y + f->height * 55 / 100, it->w, 1, col);
    }
}

int doc_hit(const struct doc *d, int x, int y) {
    for (int i = first_item(d, y); i < d->nit && d->it[i].y <= y; i++) {
        const struct item *it = &d->it[i];
        if (it->link >= 0 && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->link;
    }
    return -1;
}

int doc_node_at(const struct doc *d, int x, int y) {
    int node = 0;
    for (int i = first_item(d, y); i < d->nit && d->it[i].y <= y; i++) {
        const struct item *it = &d->it[i];
        if (it->node && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) node = it->node;
    }
    return node;
}

int doc_anchor(const struct doc *d, const char *name) {
    for (int i = 0; i < d->nan; i++) if (!strcmp(d->an[i].name, name)) return d->an[i].y;
    return -1;
}
