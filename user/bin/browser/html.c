/* html.c - tolerant HTML subset: tokenizer + inline/block layout into a flat display list.
 * Layout runs once per page/resize; doc_draw only touches the items that are on screen.
 * ponytail: no CSS, no real table grid (cells flow inline, separated by " | "), no images. */
#include "browser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

enum { F_UI, F_UIB, F_TITLE, F_MONO, F_MONOB };
static const struct font *const FT[] = { &font_ui, &font_ui_bold, &font_title, &font_mono, &font_mono_bold };
#define FL_UL 1
#define FL_STRIKE 2
#define LEAD 3
#define LN_MAX 256
#define MAXFR 128
#define MAX_ITEMS 250000

enum { T_NONE, T_HTML, T_BODY, T_TITLE, T_RAW, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_P, T_DIV, T_CENTER,
       T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_BQ, T_PRE, T_CODE, T_B, T_I, T_U, T_S, T_A, T_FONT,
       T_BR, T_HR, T_IMG, T_TABLE, T_TR, T_TD, T_TH, T_CAP };

static const struct { const char *n; int t; } TAGS[] = {
    { "html", T_HTML }, { "head", T_HTML }, { "body", T_BODY }, { "title", T_TITLE },
    { "script", T_RAW }, { "style", T_RAW }, { "svg", T_RAW }, { "template", T_RAW },
    { "h1", T_H1 }, { "h2", T_H2 }, { "h3", T_H3 }, { "h4", T_H4 }, { "h5", T_H5 }, { "h6", T_H6 },
    { "p", T_P }, { "div", T_DIV }, { "section", T_DIV }, { "article", T_DIV }, { "header", T_DIV },
    { "footer", T_DIV }, { "nav", T_DIV }, { "main", T_DIV }, { "aside", T_DIV }, { "form", T_DIV },
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

struct style { uint8_t font, flags, pre, center; int link; color_t col; int indent; };
struct frame { int tag; struct style s; int after, kind, deco_y, n, ordered; };   /* kind: 1 quote, 2 pre */

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
} L;
#define S (L.fr[L.nfr - 1].s)

static int dark(color_t c) { return (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10 < 110; }

/* ---------- document storage ---------- */
void doc_free(struct doc *d) {
    for (int i = 0; i < d->nlink; i++) free(d->link[i]);
    free(d->it); free(d->dc); free(d->txt); free(d->link); free(d->an);
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

static void add_decor(int x, int y, int w, int h, color_t col) {
    struct doc *d = L.d;
    if (d->ndc == d->dccap) {
        int nc = d->dccap ? d->dccap * 2 : 64;
        struct decor *nd = realloc(d->dc, nc * sizeof *nd);
        if (!nd) return;
        d->dc = nd; d->dccap = nc;
    }
    d->dc[d->ndc++] = (struct decor){ x, y, w, h, col };
}

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
        shift = (L.R - L.lleft - (L.ln[L.nln - 1].x + L.ln[L.nln - 1].w - L.lleft)) / 2;
        if (shift < 0) shift = 0;
    }
    for (int i = 0; i < L.nln; i++) {
        struct item *it = doc_item();
        *it = L.ln[i];
        it->x += shift;
        it->y = L.y + L.lasc - FT[it->font]->ascent;
    }
    L.y += L.lasc + L.ldesc + LEAD;
    L.gap = 0;
    L.nln = L.lasc = L.ldesc = 0;
}

static void emit(const char *s, int n, int w, int sp) {
    const struct font *f = FT[S.font];
    uint8_t flags = S.flags | (S.link >= 0 ? FL_UL : 0);
    int sw = gfx_text_width(f, " ");
    if (L.nln == LN_MAX) line_flush(0);
    if (!L.nln) { L.x = L.lleft = left(); L.lcenter = S.center; sp = 0; }
    struct item *last = L.nln ? &L.ln[L.nln - 1] : 0;
    if (last && last->font == S.font && last->flags == flags && last->col == S.col && last->link == S.link &&
        last->off + last->len == L.d->ntxt && last->len + n + 1 < 400) {   /* same style: one longer run */
        if (sp) arena(" ", 1);
        arena(s, n);
        last->len += n + sp;
        last->w += w + (sp ? sw : 0);
        L.x = last->x + last->w;
        return;
    }
    struct item *it = &L.ln[L.nln++];
    it->x = L.x + (sp ? sw : 0);
    it->w = w; it->h = f->height; it->font = S.font; it->flags = flags; it->col = S.col; it->link = S.link;
    it->len = n; it->off = arena(s, n);
    L.x = it->x + w;
    if (f->ascent > L.lasc) L.lasc = f->ascent;
    if (f->height - f->ascent > L.ldesc) L.ldesc = f->height - f->ascent;
}

static void place_word(const char *s, int n) {
    const struct font *f = FT[S.font];
    int sw = gfx_text_width(f, " "), w = gfx_text_width_n(f, s, n), sp = L.pend && L.nln;
    L.pend = 0;
    if (L.nln && L.x + (sp ? sw : 0) + w > L.R) { line_flush(0); sp = 0; }
    for (;;) {                                         /* a word wider than the line: break it */
        int fit = L.nln ? L.R - L.x - (sp ? sw : 0) : L.R - left();
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
    int maxcols = (L.R - left()) / L.cw;
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
    if (m > L.gap) { L.y += m - L.gap; L.gap = m; }
}

static struct frame *push(int tag) {
    if (L.nfr >= MAXFR) return &L.dummy;
    struct frame *f = &L.fr[L.nfr++];
    memset(f, 0, sizeof *f);
    f->tag = tag;
    f->s = L.fr[L.nfr - 2].s;
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

static int parse_color(const char *s, color_t *out) {
    static const struct { const char *n; uint32_t c; } N[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
        { "navy", 0x000080 }, { "purple", 0x800080 }, { "teal", 0x008080 }, { "olive", 0x808000 }, { "lime", 0x00FF00 },
        { "aqua", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "orange", 0xFFA500 },
    };
    if (*s == '#') s++;
    int n = strlen(s), hex = n == 3 || n == 6;
    for (int i = 0; i < n && hex; i++) hex = isxdigit((unsigned char)s[i]);
    if (hex) {
        uint32_t v = strtoul(s, 0, 16);
        if (n == 3) v = ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17;
        *out = 0xFF000000u | v;
        return 1;
    }
    for (unsigned i = 0; i < sizeof N / sizeof *N; i++)
        if (!strcasecmp(s, N[i].n)) { *out = 0xFF000000u | N[i].c; return 1; }
    return 0;
}

static void align(struct frame *f) {
    const char *a = A("align");
    if (a && !strcasecmp(a, "center")) f->s.center = 1;
}

static void set_font(struct frame *f, int id) { f->s.font = id; }

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
        struct item *it = doc_item();
        *it = (struct item){ left() - 8 - w, L.y, w, mf->height, S.font, 0, strlen(m), L.dim, -1, arena(m, strlen(m)) };
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
        add_decor(left(), L.y, L.R - left(), 2, mix(L.d->bg, L.d->fg, 70));
        L.y += 2; L.gap = 0;
        block(8);
        break;
    case T_IMG: {
        char buf[200];
        v = A("alt");
        snprintf(buf, sizeof buf, "[image%s%s]", v && *v ? ": " : "", v && *v ? v : "");
        color_t keep = S.col;
        S.col = L.dim;
        feed_text(buf, strlen(buf));
        flush_word();
        S.col = keep;
        break;
    }
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
    case T_NONE: case T_HTML: case T_BODY: case T_HR: case T_IMG: case T_RAW: case T_TITLE: break;
    default: pop_to(t);
    }
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

void doc_layout(struct doc *d, const char *src, size_t n, int is_html, int width) {
    char title[sizeof d->title];
    strlcpy(title, d->title, sizeof title);          /* a title survives relayout */
    doc_free(d);
    memset(&L, 0, sizeof L);
    L.d = d;
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
    L.fr[0].s = (struct style){ F_UI, 0, 0, 0, -1, d->fg, 0 };
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
            if (closing) { close_tag(t); continue; }
            parse_attrs(tb);
            if (t == T_RAW || t == T_TITLE) {
                int ce;
                int next = skip_raw(src, i, n, name, &ce);
                if (t == T_TITLE) capture_title(src + i, ce - i);
                i = next;
                continue;
            }
            open_tag(t);
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
    ytop -= 40;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (d->it[mid].y < ytop) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void doc_draw(struct ui *ui, const struct doc *d, int vx, int vy, int vw, int vh, int scroll, int hover) {
    struct canvas cc = *ui->c;
    canvas_clip(&cc, vx, vy, vw, vh);
    gfx_fill(&cc, vx, vy, vw, vh, d->bg);
    for (int i = 0; i < d->ndc; i++) {
        const struct decor *r = &d->dc[i];
        int y = vy + r->y - scroll;
        if (y < vy + vh && y + r->h > vy) gfx_fill(&cc, vx + r->x, y, r->w, r->h, r->col);
    }
    for (int i = first_item(d, scroll); i < d->nit; i++) {
        const struct item *it = &d->it[i];
        int y = vy + it->y - scroll;
        if (y >= vy + vh) break;
        if (y + it->h < vy) continue;
        const struct font *f = FT[it->font];
        color_t col = it->link >= 0 && it->link == hover ? d->hoverc : it->col;
        gfx_text_n(&cc, f, vx + it->x, y, d->txt + it->off, it->len, col);
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

int doc_anchor(const struct doc *d, const char *name) {
    for (int i = 0; i < d->nan; i++) if (!strcmp(d->an[i].name, name)) return d->an[i].y;
    return -1;
}
