/* Host test of the browser's CSS engine (user/bin/browser/css.c + cssel.c): colours, values, selectors, cascade, var(), calc(),
 * @media/@supports/@layer, inheritance, pseudo-elements, UA defaults, plus a timing run on a large generated page. */
#include "browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- stubs for the rest of the browser ---- */
void url_resolve(const char *base, const char *ref, char *out, int cap) {
    if (strstr(ref, "://") || !*base) snprintf(out, cap, "%s", ref);
    else snprintf(out, cap, "%s%s", base, ref);
}
const char *sheet_get(const char *url, size_t *len) {
    if (!strcmp(url, "http://x/imp.css")) { static const char t[] = "p.imp{color:#00ff00}"; *len = sizeof t - 1; return t; }
    return 0;
}

/* ---- a tiny DOM ---- */
#define MAXN 60000
static struct node { const char *tag; char id[32], cls[96], style[160]; char at[4][2][40]; int nat; int par, prev, next, first, last, state, text; } N[MAXN];
static int nn = 1;
static int d_parent(int n) { return N[n].par; }
static int d_prev(int n) { return N[n].prev; }
static int d_next(int n) { return N[n].next; }
static int d_first(int n) { return N[n].first; }
static const char *d_tag(int n) { return N[n].tag; }
static const char *d_attr(int n, const char *a) {
    if (!strcmp(a, "id")) return N[n].id[0] ? N[n].id : 0;
    if (!strcmp(a, "class")) return N[n].cls[0] ? N[n].cls : 0;
    if (!strcmp(a, "style")) return N[n].style[0] ? N[n].style : 0;
    for (int i = 0; i < N[n].nat; i++) if (!strcmp(N[n].at[i][0], a)) return N[n].at[i][1];
    return 0;
}
static int d_state(int n) { return N[n].state; }
static int d_text(int n) { return N[n].text; }
static const struct css_dom DOMT = { d_parent, d_prev, d_next, d_first, d_tag, d_attr, d_state, d_text };

/* el(parent, "div", "id", "a b"): returns node; extra attrs through at() */
static int el(int par, const char *tag, const char *id, const char *cls) {
    int n = nn++;
    memset(&N[n], 0, sizeof N[n]);
    N[n].tag = tag; N[n].par = par;
    if (id) strcpy(N[n].id, id);
    if (cls) strcpy(N[n].cls, cls);
    if (par) {
        if (N[par].last) { N[N[par].last].next = n; N[n].prev = N[par].last; } else N[par].first = n;
        N[par].last = n;
    }
    return n;
}
static void at(int n, const char *k, const char *v) { strcpy(N[n].at[N[n].nat][0], k); strcpy(N[n].at[N[n].nat][1], v); N[n].nat++; }
static void reset_dom(void) { nn = 1; }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct cprops compute(int n) {       /* computes the whole ancestor chain, like layout does */
    struct cprops chain[64];
    int path[64], d = 0;
    for (int x = n; x; x = N[x].par) path[d++] = x;
    for (int i = d - 1; i >= 0; i--) css_compute(path[i], 0, i == d - 1 ? 0 : &chain[i + 1], &chain[i]);
    return chain[0];
}
static int len(const struct cprops *c, int p) { return css_len(c, p, 1000); }
static int col(const char *s) { color_t c = 0; return css_color(s, &c, 0) ? (int)c : -1; }

static void new_page(const char *css) { css_reset(); css_dom_set(&DOMT); css_viewport_size(1000, 600); css_base("http://x/"); if (css) css_add_from(css, strlen(css), "http://x/"); }

static void test_colors(void) {
    CHECK(col("#f00") == (int)0xFFFF0000u);
    CHECK(col("#00ff0080") == (int)0x8000FF00u);
    CHECK(col("#abcd") == (int)0xDDAABBCCu);
    CHECK(col("rgb(1, 2, 3)") == (int)0xFF010203u);
    CHECK(col("rgb(1 2 3 / 50%)") == (int)0x80010203u);
    CHECK(col("rgba(255,0,0,.5)") == (int)0x80FF0000u);
    CHECK(col("rgb(100%, 0%, 0%)") == (int)0xFFFF0000u);
    CHECK(col("hsl(120, 100%, 50%)") == (int)0xFF00FF00u);
    CHECK(col("hsl(0 100% 50% / 0)") == 0);
    CHECK(col("hsla(240,100%,50%,1)") == (int)0xFF0000FFu);
    CHECK(col("RebeccaPurple") == (int)0xFF663399u);
    CHECK(col("transparent") == 0);
    CHECK(col("lightgoldenrodyellow") == (int)0xFFFAFAD2u);
    CHECK(col("notacolor") == -1);
    CHECK(col("#12") == -1);
}

static void test_selectors(void) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, "page"), div = el(body, "div", "main", "a b");
    int ul = el(div, "ul", 0, 0), li1 = el(ul, "li", 0, "x"), li2 = el(ul, "li", 0, "y"), li3 = el(ul, "li", 0, "x last");
    int p = el(div, "p", 0, 0), a = el(p, "a", 0, 0), em = el(p, "em", 0, 0), span = el(div, "span", 0, 0);
    at(a, "href", "http://example.com/x.pdf"); at(a, "data-k", "Hello World"); at(a, "lang", "en-US");
    N[p].text = 1;
    css_reset(); css_dom_set(&DOMT);
    #define M(sel, n) css_match_node(sel, n)
    CHECK(M("div", div) == 1 && M("DIV", div) == 1 && M("p", div) == 0);
    CHECK(M("#main", div) == 1 && M("div#main.a.b", div) == 1 && M(".c", div) == 0);
    CHECK(M("body div", div) == 1 && M("html div", div) == 1 && M("body > div", div) == 1 && M("html > div", div) == 0);
    CHECK(M("li + li", li2) == 1 && M("li + li", li1) == 0 && M("li ~ li", li3) == 1 && M("li ~ li", li1) == 0);
    CHECK(M("ul > li.y ~ li.x", li1) == 0 && M("ul > li.x + li.y", li2) == 1 && M("ul > li.x ~ li.y", li2) == 1);
    CHECK(M("li:first-child", li1) == 1 && M("li:first-child", li2) == 0 && M("li:last-child", li3) == 1 && M("li:only-child", li1) == 0);
    CHECK(M("li:nth-child(2)", li2) == 1 && M("li:nth-child(odd)", li3) == 1 && M("li:nth-child(even)", li3) == 0 && M("li:nth-child(2n+1)", li1) == 1);
    CHECK(M("li:nth-child(-n+2)", li2) == 1 && M("li:nth-child(-n+2)", li3) == 0 && M("li:nth-last-child(1)", li3) == 1);
    CHECK(M("p:first-of-type", p) == 1 && M("span:last-of-type", span) == 1 && M("li:nth-of-type(3)", li3) == 1);
    CHECK(M("span:empty", span) == 1 && M("p:empty", p) == 0 && M("ul:empty", ul) == 0);
    CHECK(M("html:root", html) == 1 && M(":root", body) == 0);
    CHECK(M("a[href]", a) == 1 && M("a[href^='http://']", a) == 1 && M("a[href$='.pdf']", a) == 1 && M("a[href*='ample']", a) == 1);
    CHECK(M("a[data-k='hello world' i]", a) == 1 && M("a[data-k='hello world']", a) == 0 && M("a[data-k~='World']", a) == 1);
    CHECK(M("a[lang|=en]", a) == 1 && M("a[href=\"x\"]", a) == 0 && M("a:link", a) == 1);
    CHECK(M("li:not(.x)", li2) == 1 && M("li:not(.x)", li1) == 0 && M("li:not(.x, .y)", li2) == 0);
    CHECK(M(":is(ul, ol) > li", li1) == 1 && M(":where(ul, ol) > li", li1) == 1 && M(":is(.nope)", li1) == 0);
    CHECK(M("div:has(> ul)", div) == 1 && M("div:has(> a)", div) == 0 && M("div:has(a)", div) == 1 && M("ul:has(+ p)", ul) == 1);
    CHECK(M("li:has(~ li.last)", li1) == 1 && M("p:has(em):not(:empty)", p) == 1);
    CHECK(M("div, span", span) == 1 && M("nope, span", span) == 1 && M("nope, ::bad", span) == 0);
    CHECK(M("li[", li1) == -1 && M("", li1) == -1 && M("li:unknownpseudo", li1) == -1 && M("a::before", a) == -1);
    N[a].state = CS_HOVER;
    CHECK(M("a:hover", a) == 1 && M("a:focus", a) == 0 && M("p:hover", p) == 0);
    CHECK(M(".md\\:flex", div) == 0);
    strcpy(N[div].cls, "md:flex"); CHECK(M(".md\\:flex", div) == 1);
    CHECK(M("a:lang(en)", a) == 1);
}

static void test_cascade(void) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, 0), div = el(body, "div", "d", "c1 c2");
    int p = el(div, "p", 0, "t"), sp = el(p, "span", 0, 0);
    struct cprops c;
    new_page("p{color:red} .t{color:blue} div p{color:green}"); c = compute(p);
    CHECK(c.v[P_COLOR] == (int)0xFF0000FFu);                       /* class beats two tags */
    new_page("#d p{color:red} p.t.t{color:blue}"); c = compute(p);
    CHECK(c.v[P_COLOR] == (int)0xFFFF0000u);                       /* id beats classes */
    new_page("p{color:red !important} p.t{color:blue}"); c = compute(p);
    CHECK(c.v[P_COLOR] == (int)0xFFFF0000u);
    strcpy(N[p].style, "color:lime");
    new_page("p{color:red} p.t{color:blue}"); c = compute(p);
    CHECK(c.v[P_COLOR] == (int)0xFF00FF00u);                       /* inline beats sheets */
    new_page("p{color:red !important}"); c = compute(p);
    CHECK(c.v[P_COLOR] == (int)0xFFFF0000u);                       /* important beats inline */
    N[p].style[0] = 0;
    new_page("div{color:#123456;font-size:20px;border:2px solid;padding:1em;margin:0 auto;text-align:center}"); c = compute(sp);
    CHECK(c.v[P_COLOR] == (int)0xFF123456u && c.v[P_FSIZE] == 20 && c.v[P_ALIGN] == TA_CENTER);       /* inherited */
    c = compute(div);
    CHECK(len(&c, P_BWT) == 2 && c.v[P_BSTT] == BS_SOLID && c.v[P_BCT] == (int)0xFF123456u);          /* currentcolor border */
    CHECK(len(&c, P_PL) == 20 && c.k[P_ML] == K_AUTO && len(&c, P_MT) == 0);                            /* em = own font-size */
    c = compute(sp);
    CHECK(len(&c, P_BWT) == 0 && len(&c, P_PL) == 0);                                                   /* not inherited */
    new_page("div{font-size:20px} span{font-size:150%;margin-left:2em;width:50%} p{font-size:larger}"); c = compute(sp);
    CHECK(c.v[P_FSIZE] == 36 && len(&c, P_ML) == 72 && css_len(&c, P_WIDTH, 800) == 400);              /* 20 * 1.2 (p larger) = 24, *1.5 = 36 */
    new_page("html{font-size:10px} div{font-size:2rem;width:calc(100% - 20px);height:calc(2em + 5px);padding:min(10px,5%)}"); c = compute(div);
    CHECK(c.v[P_FSIZE] == 20 && css_len(&c, P_WIDTH, 500) == 480 && len(&c, P_HEIGHT) == 45 && css_len(&c, P_PT, 1000) == 10);
    new_page("div{width:clamp(100px, 50%, 400px);max-width:min(100%, 600px);min-width:max(10px, 2vw);height:calc(50vh - 10px)}"); c = compute(div);
    CHECK(css_len(&c, P_WIDTH, 500) == 250 && css_len(&c, P_WIDTH, 100) == 100 && css_len(&c, P_WIDTH, 2000) == 400);
    CHECK(css_len(&c, P_MAXW, 1000) == 600 && css_len(&c, P_MAXW, 300) == 300 && css_len(&c, P_MINW, 0) == 20 && len(&c, P_HEIGHT) == 290);
    new_page("div{margin:calc(1px + 2px) calc(10px * 2) calc(100px / 4) -3px;line-height:1.5;font-size:10px}"); c = compute(div);
    CHECK(len(&c, P_MT) == 3 && len(&c, P_MR) == 20 && len(&c, P_MB) == 25 && len(&c, P_ML) == -3 && c.k[P_LH] == K_NUM && c.v[P_LH] == 150);
    new_page("span{margin:1vw 2vh 3vmin 4vmax}"); c = compute(sp);
    CHECK(len(&c, P_MT) == 10 && len(&c, P_MR) == 12 && len(&c, P_MB) == 18 && len(&c, P_ML) == 40);
    /* shorthand/longhand and inherit/initial/unset */
    new_page("div{color:red;margin:5px} p{color:inherit;margin:inherit;background:inherit} span{color:initial;margin:unset;background:unset}");
    c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFFFF0000u && len(&c, P_MT) == 5);
    c = compute(sp); CHECK(c.v[P_COLOR] == (int)0xFF000000u && len(&c, P_MT) == 0);
    new_page("div{border-top:3px dashed red;border-bottom-width:0;border-width:1px 2px;border-style:solid dotted}");
    c = compute(div); CHECK(len(&c, P_BWT) == 1 && len(&c, P_BWR) == 2 && c.v[P_BSTR] == BS_DOTTED && c.v[P_BSTT] == BS_SOLID && c.v[P_BCT] == (int)0xFFFF0000u);
    new_page("div{border:1px solid red;border-style:none} p{border:0}"); c = compute(div);
    CHECK(len(&c, P_BWT) == 0);
    new_page("div{background:#fff url(a.png) no-repeat center / cover, red}"); c = compute(div);
    CHECK(c.v[P_BG] == (int)0xFFFF0000u && c.k[P_BGIMG] == K_STR && !strcmp(css_str(c.v[P_BGIMG]), "http://x/a.png") && c.v[P_BGREP] == REP_NONE && c.k[P_BGSW] == K_COVER);
    new_page("div{background:url('b.png') right 10px top 20px repeat-x}"); c = compute(div);
    CHECK(c.v[P_BGREP] == REP_X && c.v[P_BGPX] == 10000 && c.v[P_BGPY] == 20 && c.k[P_BGPY] == K_PX);
    new_page("div{background-image:linear-gradient(red,blue);background-color:blue}"); c = compute(div);
    CHECK(c.k[P_BGIMG] == K_GRAD && c.v[P_BG] == (int)0xFF0000FFu);
    /* font shorthand */
    new_page("div{font:italic bold 14px/1.4 'Helvetica Neue', Arial, sans-serif}"); c = compute(div);
    CHECK(c.v[P_ITALIC] == 1 && c.v[P_FWEIGHT] == 700 && c.v[P_BOLD] == 1 && c.v[P_FSIZE] == 14 && c.v[P_LH] == 140 && c.v[P_FAMILY] == FAM_SANS);
    new_page("div{font-family: Georgia, serif} p{font-family:'SF Mono', Menlo, monospace} span{font-weight:bolder}"); c = compute(div);
    CHECK(c.v[P_FAMILY] == FAM_SERIF); c = compute(p); CHECK(c.v[P_FAMILY] == FAM_MONO && c.v[P_MONO] == 1); c = compute(sp); CHECK(c.v[P_FAMILY] == FAM_MONO && c.v[P_FWEIGHT] == 700);
    /* flex, grid, positioning */
    new_page("div{display:flex;flex:1 0 100px;gap:4px 8px;justify-content:space-between;align-items:center;flex-flow:column wrap;position:sticky;top:0;z-index:5;float:left}");
    c = compute(div);
    CHECK(c.v[P_DISPLAY] == D_FLEX);                                 /* floated flex container is still a flex container */
    new_page("div{display:flex;flex:1 0 100px;gap:4px 8px;justify-content:space-between;align-items:center;flex-flow:column wrap;position:sticky;top:0;z-index:5}");
    c = compute(div);
    CHECK(c.v[P_DISPLAY] == D_FLEX && c.v[P_FGROW] == 100 && c.v[P_FSHRINK] == 0 && len(&c, P_FBASIS) == 100 && len(&c, P_RGAP) == 4 && len(&c, P_CGAP) == 8);
    CHECK(c.v[P_JUSTC] == AL_BETWEEN && c.v[P_ALIGNI] == AL_CENTER && c.v[P_FLEXDIR] == 2 && c.v[P_FLEXWRAP] == 1 && c.v[P_POSITION] == POS_STICKY && c.v[P_ZINDEX] == 5);
    new_page("div{flex:1} p{flex:auto} span{flex:none}"); c = compute(div); CHECK(c.v[P_FGROW] == 100 && c.v[P_FSHRINK] == 100 && c.k[P_FBASIS] == K_PX && c.v[P_FBASIS] == 0);
    c = compute(p); CHECK(c.k[P_FBASIS] == K_AUTO && c.v[P_FGROW] == 100); c = compute(sp); CHECK(c.v[P_FGROW] == 0 && c.v[P_FSHRINK] == 0);
    new_page("div{display:grid;grid-template-columns:repeat(3, 1fr);grid-template-rows:auto 1fr;gap:10px} p{grid-column:2 / span 2;grid-row:1/3} span{grid-area:hdr}");
    c = compute(div); CHECK(c.v[P_DISPLAY] == D_GRID && !strcmp(css_str(c.v[P_GCOLS]), "repeat(3, 1fr)") && !strcmp(css_str(c.v[P_GROWS]), "auto 1fr") && len(&c, P_RGAP) == 10);
    c = compute(p); CHECK(c.k[P_GCS] == K_PX && c.v[P_GCS] == 2 && c.k[P_GCE] == K_SPAN && c.v[P_GCE] == 2 && c.v[P_GRS] == 1 && c.v[P_GRE] == 3);
    c = compute(sp); CHECK(c.k[P_GAREA] == K_STR && !strcmp(css_str(c.v[P_GAREA]), "hdr"));
    new_page("div{position:absolute;display:inline-block;inset:0 auto 10px 5%}"); c = compute(div);
    CHECK(c.v[P_POSITION] == POS_ABSOLUTE && c.v[P_DISPLAY] == D_BLOCK && len(&c, P_TOP) == 0 && c.k[P_RIGHT] == K_AUTO && len(&c, P_BOTTOM) == 10 && css_len(&c, P_LEFT, 200) == 10);
    new_page("div{overflow:hidden auto;opacity:.5;visibility:hidden;box-sizing:border-box;border-radius:4px 8px;white-space:nowrap;text-transform:uppercase;letter-spacing:2px;text-indent:2em}");
    c = compute(div); CHECK(c.v[P_OVX] == OV_HIDDEN && c.v[P_OVY] == OV_AUTO && c.v[P_OPACITY] == 128 && c.v[P_VISIB] == 1 && c.v[P_BOXSZ] == 1);
    CHECK(len(&c, P_RTL) == 4 && len(&c, P_RTR) == 8 && len(&c, P_RBR) == 4 && c.v[P_WS] == WS_NOWRAP && c.v[P_TT] == 1 && len(&c, P_LS) == 2 && len(&c, P_TIND) == 32);
    c = compute(sp); CHECK(c.v[P_WS] == WS_NOWRAP && c.v[P_VISIB] == 1 && c.v[P_OPACITY] == 255 && c.v[P_OVX] == OV_VISIBLE);   /* inherited vs not */
    new_page("div{list-style:square inside} p{list-style:none} span{list-style-type:lower-roman}");
    c = compute(p); CHECK(c.v[P_LISTT] == LT_NONE && c.v[P_LIST] == 1 && c.v[P_LISTP] == 0);
    c = compute(sp); CHECK(c.v[P_LISTT] == LT_LROMAN && c.v[P_LISTP] == 0);
    new_page("div{margin-inline:auto 4px;padding-block:2px 3px;inset-inline-start:7px;border-inline-end:2px solid;width:fit-content;min-height:min-content}"); c = compute(div);
    CHECK(c.k[P_ML] == K_AUTO && len(&c, P_MR) == 4 && len(&c, P_PT) == 2 && len(&c, P_PB) == 3 && len(&c, P_LEFT) == 7 && len(&c, P_BWR) == 2 && c.k[P_WIDTH] == K_FIT && c.k[P_MINH] == K_MINC);
}

static void test_vars_media(void) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, 0), div = el(body, "div", 0, "box"), p = el(div, "p", 0, 0);
    struct cprops c;
    new_page(":root{--main:#336699;--gap:8px;--twice:calc(var(--gap) * 2);--font:Georgia, serif} .box{--gap:10px;color:var(--main);margin:var(--gap) var(--missing, 3px);padding:var(--twice);font-family:var(--font)}");
    c = compute(div);
    CHECK(c.v[P_COLOR] == (int)0xFF336699u && len(&c, P_MT) == 10 && len(&c, P_MR) == 3 && len(&c, P_PT) == 16 && c.v[P_FAMILY] == FAM_SERIF);
    c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFF336699u);
    new_page("p{--x:var(--y);--y:var(--x);color:var(--x, red);margin:var(--none)} div{--c:blue} p{background:var(--c)}"); c = compute(p);
    CHECK(c.v[P_COLOR] == (int)0xFFFF0000u && c.v[P_BG] == (int)0xFF0000FFu && len(&c, P_MT) == 16);   /* a cycle is invalid: fallback used; unset var -> not declared */
    new_page("div{--w:50%} p{width:var(--w);height:var(--h,calc(10px + 1em))}"); c = compute(p);
    CHECK(css_len(&c, P_WIDTH, 300) == 150 && len(&c, P_HEIGHT) == 26);
    /* @media */
    new_page("@media (min-width: 800px) and (max-width: 1200px){div{color:red}} @media (max-width:500px){div{color:blue}} @media print{div{color:lime}}"
             "@media (width >= 900px){p{color:red}} @media (400px <= width <= 700px){p{color:blue}} @media not (min-width: 2000px){p{margin-top:1px}}"
             "@media screen, print and (orientation: portrait){p{margin-left:2px}} @media (prefers-color-scheme: dark){p{margin-right:9px}}"
             "@media (prefers-reduced-motion: no-preference) and (hover: hover){p{margin-bottom:3px}}");
    c = compute(div); CHECK(c.v[P_COLOR] == (int)0xFFFF0000u);
    c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFFFF0000u && len(&c, P_MT) == 1 && len(&c, P_ML) == 2 && len(&c, P_MR) == 0 && len(&c, P_MB) == 3);
    css_reset(); css_viewport_size(600, 800);
    { const char *t = "@media (max-width:640px){div{color:blue}} @media (min-width:48em){div{color:red}} @media (orientation:portrait){p{margin-top:7px}}"; css_add(t, strlen(t)); }
    { const char *t = "@media (min-width:36em) and (max-width: 40em){div{margin-top:1px}}"; css_add(t, strlen(t)); }
    css_dom_set(&DOMT);
    c = compute(div); CHECK(c.v[P_COLOR] == (int)0xFF0000FFu && len(&c, P_MT) == 1); c = compute(p); CHECK(len(&c, P_MT) == 7);
    CHECK(css_media("screen") == 1 && css_media("print") == 0 && css_media("") == 1 && css_media("all and (min-width:1px)") == 1 && css_media("only screen and (max-width: 100px)") == 0);
    CHECK(css_media("(min-width: 100px), print") == 1 && css_media("not print") == 1 && css_media("not screen") == 0);
    CHECK(css_media("screen and (min-width:calc(500px + 50px))") == 1 && css_media("(max-width:calc(600px - 1px))") == 0);
    /* @supports, @layer, @import, ignored at-rules */
    new_page("@supports (display: grid){div{color:red}} @supports not (display: grid){div{color:blue}} @supports (backdrop-filter: blur(1px)){p{padding-top:9px}}"
             "@supports (display:flex) and (position:sticky){p{margin-top:1px}} @supports selector(:has(a)){p{margin-left:1px}} @supports (not (foo: bar)){p{margin-right:1px}}");
    c = compute(div); CHECK(c.v[P_COLOR] == (int)0xFFFF0000u);
    c = compute(p); CHECK(len(&c, P_PT) == 0 && len(&c, P_MT) == 1 && len(&c, P_ML) == 1 && len(&c, P_MR) == 1);
    new_page("@layer base, theme; @layer theme{p{color:red}} @layer base{p{color:blue;margin-top:5px}} p.none{margin-top:7px}"
             "@layer base{p{margin-left:2px !important}} p{margin-left:9px !important}");
    c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFFFF0000u && len(&c, P_MT) == 5 && len(&c, P_ML) == 2);       /* later layer wins; important reverses */
    new_page("p{color:blue} @layer x{p{color:red}}"); c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFF0000FFu);   /* unlayered beats layered */
    new_page("@import url(imp.css); @import 'imp.css' screen; @font-face{font-family:x;src:url(y)} @keyframes k{from{color:red}to{color:blue}} @page{margin:1cm}"
             "p{margin-top:4px} @media print{p{margin-top:9px}} @charset 'x';");
    strcpy(N[p].cls, "imp"); c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFF00FF00u && len(&c, P_MT) == 4); N[p].cls[0] = 0;
    new_page("p{color:red;;;} p{margin:bogus;color:blue;weird thing;width:10px} p{height:5px; x:y} p{ & a{color:red} margin-top:3px }");
    c = compute(p); CHECK(c.v[P_COLOR] == (int)0xFF0000FFu && len(&c, P_WIDTH) == 10 && len(&c, P_HEIGHT) == 5 && len(&c, P_MT) == 3 && len(&c, P_ML) == 0);
}

static void test_pseudo_ua(void) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, 0), h1 = el(body, "h1", 0, 0), pp = el(body, "p", 0, 0), a = el(pp, "a", 0, 0), ul = el(body, "ul", 0, 0);
    int li = el(ul, "li", 0, 0), ul2 = el(li, "ul", 0, 0), li2 = el(ul2, "li", 0, 0), tbl = el(body, "table", 0, 0), tr = el(tbl, "tr", 0, 0), td = el(tr, "td", 0, 0);
    int hid = el(body, "div", 0, 0), inp = el(body, "input", 0, 0), pre = el(body, "pre", 0, 0), b = el(pp, "b", 0, 0);
    at(a, "href", "/x"); at(hid, "hidden", ""); at(inp, "type", "hidden");
    struct cprops c;
    new_page(0);
    c = compute(body); CHECK(c.v[P_DISPLAY] == D_BLOCK && len(&c, P_MT) == 8 && len(&c, P_ML) == 8);
    c = compute(h1); CHECK(c.v[P_FSIZE] == 32 && c.v[P_BOLD] == 1 && len(&c, P_MT) == 21 && c.v[P_DISPLAY] == D_BLOCK);
    c = compute(pp); CHECK(len(&c, P_MT) == 16 && len(&c, P_MB) == 16);
    c = compute(a); CHECK(c.v[P_COLOR] == (int)0xFF0000EEu && c.v[P_DECO] == DECO_UNDERLINE && c.v[P_DISPLAY] == D_INLINE && c.v[P_CURSOR] == 2);
    c = compute(b); CHECK(c.v[P_BOLD] == 1);
    c = compute(ul); CHECK(c.v[P_LISTT] == LT_DISC && len(&c, P_PL) == 40);
    c = compute(li); CHECK(c.v[P_DISPLAY] == D_LIST_ITEM && c.v[P_LISTT] == LT_DISC);
    c = compute(li2); CHECK(c.v[P_LISTT] == LT_CIRCLE);
    c = compute(ul2); CHECK(len(&c, P_MT) == 0);
    c = compute(tbl); CHECK(c.v[P_DISPLAY] == D_TABLE && len(&c, P_SPACING) == 2 && c.v[P_BOXSZ] == 1);
    c = compute(td); CHECK(c.v[P_DISPLAY] == D_TABLE_CELL && len(&c, P_PL) == 1);
    c = compute(hid); CHECK(c.v[P_DISPLAY] == D_NONE);
    c = compute(inp); CHECK(c.v[P_DISPLAY] == D_NONE);
    c = compute(pre); CHECK(c.v[P_FAMILY] == FAM_MONO && c.v[P_WS] == WS_PRE);
    new_page("a{color:red;text-decoration:none} p{display:flex} [hidden]{display:block}");
    c = compute(a); CHECK(c.v[P_COLOR] == (int)0xFFFF0000u && c.v[P_DECO] == 0);     /* author beats UA, even with lower specificity */
    c = compute(hid); CHECK(c.v[P_DISPLAY] == D_BLOCK);
    /* ::before / ::after */
    new_page("p::before{content:'> ';color:red} p::after{content:attr(data-x) \"!\"} a::before{content:\"\\201C\"} b::after{content:none} h1:before{content:''}"
             "ul:hover::after{content:'hov'}");
    at(pp, "data-x", "hi");
    struct cprops own = compute(pp), pe;
    CHECK(css_compute(pp, PE_BEFORE, &own, &pe) == 1 && !strcmp(css_str(pe.v[P_CONTENT]), "> ") && pe.v[P_COLOR] == (int)0xFFFF0000u);
    CHECK(css_compute(pp, PE_AFTER, &own, &pe) == 1 && !strcmp(css_str(pe.v[P_CONTENT]), "hi!"));
    own = compute(a); CHECK(css_compute(a, PE_BEFORE, &own, &pe) == 1 && !strcmp(css_str(pe.v[P_CONTENT]), "\xE2\x80\x9C"));
    own = compute(b); CHECK(css_compute(b, PE_AFTER, &own, &pe) == 0);
    own = compute(h1); CHECK(css_compute(h1, PE_BEFORE, &own, &pe) == 1 && !*css_str(pe.v[P_CONTENT]));
    own = compute(ul); CHECK(css_compute(ul, PE_AFTER, &own, &pe) == 0); N[ul].state = CS_HOVER; CHECK(css_compute(ul, PE_AFTER, &own, &pe) == 1);
    own = compute(li); CHECK(css_compute(li, PE_BEFORE, &own, &pe) == 0);
    /* the legacy stack entry still works */
    struct el st[3];
    memset(st, 0, sizeof st);
    strcpy(st[0].name, "html"); strcpy(st[1].name, "body"); strcpy(st[2].name, "p"); strcpy(st[2].cls, "t"); st[2].first = 1;
    new_page("body p.t{color:#102030;margin:2em;border:1px solid red;display:inline-block;font-weight:bold} p:first-child{padding-left:3px} p:last-child{width:9px}");
    css_cascade(st, 3, "height:4px", &c);
    CHECK((c.set & 1u << P_COLOR) && c.v[P_COLOR] == (int)0xFF102030u && c.v[P_MT] == 32 && c.v[P_BWT] == 1 && c.v[P_BOLD] == 1 && c.v[P_DISPLAY] == D_INLINE);
    CHECK(c.v[P_PL] == 3 && c.v[P_HEIGHT] == 4 && !(c.set & 1u << P_WIDTH) && !(c.set & 1u << P_BG));
    CHECK(css_match("body p", st, 3) == 1 && css_match("html > p", st, 3) == 0 && css_match("::x", st, 3) == -1 && css_match("p.t, q", st, 3) == 1);
    (void)tbl; (void)td;
}

static void test_extras(void) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, 0), ol = el(body, "ol", 0, "toc"), li1 = el(ol, "li", 0, 0), li2 = el(ol, "li", 0, 0), sub = el(li2, "ol", 0, 0), s1 = el(sub, "li", 0, 0);
    int d = el(body, "div", 0, "fx");
    struct cprops c, pe, own;
    new_page(".fx{outline:2px dotted red;outline-offset:3px;box-shadow:inset 1px 2px 3px 4px rgba(0,0,0,.5), 0 0 5px red;object-position:left 10px;transform:rotate(3deg) translate(10px, 50%) scale(2);text-overflow:ellipsis}"
             "ol{counter-reset:item} li{counter-increment:item} li::before{content:counters(item, '.') ' '} li::marker{color:red;font-weight:bold}");
    c = compute(d);
    CHECK(len(&c, P_OUTW) == 2 && c.v[P_OUTS] == BS_DOTTED && c.v[P_OUTC] == (int)0xFFFF0000u && len(&c, P_OUTO) == 3);
    CHECK(c.k[P_SHC] == K_PX && c.v[P_SHC] == (int)0x80000000u && len(&c, P_SHX) == 1 && len(&c, P_SHY) == 2 && len(&c, P_SHB) == 3 && len(&c, P_SHS) == 4 && c.v[P_SHI] == 1);
    CHECK(c.v[P_OBJPX] == 0 && len(&c, P_OBJPY) == 10 && len(&c, P_TRX) == 10 && css_len(&c, P_TRY, 200) == 100 && c.v[P_TOVF] == 1);
    c = compute(body); CHECK(len(&c, P_OUTW) == 0 && c.k[P_SHC] == K_NONE && len(&c, P_TRX) == 0);
    css_cnt_reset_all();
    c = compute(ol); css_cnt_enter(&c, 1);
    c = compute(li1); css_cnt_enter(&c, 2); own = c;
    CHECK(css_compute(li1, PE_BEFORE, &own, &pe) == 1 && !strcmp(css_str(pe.v[P_CONTENT]), "1 "));
    css_cnt_leave(2);
    c = compute(li2); css_cnt_enter(&c, 2); own = c;
    CHECK(css_compute(li2, PE_BEFORE, &own, &pe) == 1 && !strcmp(css_str(pe.v[P_CONTENT]), "2 "));
    c = compute(sub); css_cnt_enter(&c, 3);
    c = compute(s1); css_cnt_enter(&c, 4); own = c;
    CHECK(css_compute(s1, PE_BEFORE, &own, &pe) == 1 && !strcmp(css_str(pe.v[P_CONTENT]), "2.1 "));
    css_cnt_leave(4); css_cnt_leave(3); css_cnt_leave(2);
    own = compute(li1);
    CHECK(css_compute(li1, PE_MARKER, &own, &pe) == 1 && pe.v[P_COLOR] == (int)0xFFFF0000u && pe.v[P_BOLD] == 1 && pe.k[P_CONTENT] == K_NONE);
    char b[16];
    css_counter_format(LT_LROMAN, 14, b, sizeof b); CHECK(!strcmp(b, "xiv"));
    css_counter_format(LT_UALPHA, 28, b, sizeof b); CHECK(!strcmp(b, "AB"));
    css_counter_format(LT_ZERO, 7, b, sizeof b); CHECK(!strcmp(b, "07"));
    css_counter_format(LT_DECIMAL, -3, b, sizeof b); CHECK(!strcmp(b, "-3"));
}

static void test_restyle(void) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, 0), ul = el(body, "ul", 0, "menu"), li = el(ul, "li", 0, "item"), a = el(li, "a", 0, 0);
    int p1 = el(body, "p", 0, 0), p2 = el(body, "p", 0, "note");
    int deep;
    new_page("a:hover{color:red} ul.open li{color:blue} .hl + p{margin:0} div:has(.z){color:red} [data-x]{color:red} li[data-y] a{color:red} :focus-within{color:red}");
    CHECK(css_restyle_for_state(a, CS_HOVER, &deep) == a && deep == 0);
    CHECK(css_restyle_for_state(a, CS_CHECKED, &deep) == 0);
    CHECK(css_restyle_for_state(li, CS_FOCUS, &deep) == li && deep == 0 || deep == 1);
    CHECK(css_restyle_for_class(ul, "menu", "menu open", &deep) == ul && deep == 1);
    CHECK(css_restyle_for_class(ul, "menu", "menu closed", &deep) == 0);
    CHECK(css_restyle_for_class(p1, 0, "hl", &deep) == body && deep == 1);
    CHECK(css_restyle_for_class(p1, "x y", "y x", &deep) == 0);
    CHECK(css_restyle_for_class(p2, 0, "z", &deep) == html && deep == 1);          /* :has(.z) can restyle any ancestor */
    CHECK(css_restyle_for_attr(a, "data-x", &deep) == a && deep == 0);
    CHECK(css_restyle_for_attr(li, "data-y", &deep) == li && deep == 1);
    CHECK(css_restyle_for_attr(li, "data-nothing", &deep) == 0);
    CHECK(css_restyle_for_attr(li, "style", &deep) == li && deep == 0);
    CHECK(css_restyle_for_insert(li, &deep) == ul);                                /* sibling-position selectors (+) exist: the parent restyles */
    struct cprops x = compute(a), y = x;
    CHECK(css_inherit_same(&x, &y));
    y.v[P_COLOR] ^= 1; CHECK(!css_inherit_same(&x, &y));
    y = x; y.v[P_BG] ^= 1; CHECK(css_inherit_same(&x, &y));
    (void)p2;
}

/* timing: a page of `n` elements and a sheet of `rules` rules */
static void bench(int n, int rules) {
    reset_dom();
    int html = el(0, "html", 0, 0), body = el(html, "body", 0, 0), div = el(body, "div", "content", "mw-body");
    char cls[40];
    static const char *tags[] = { "p", "span", "a", "li", "td", "div", "b", "i" };
    int cur = div;
    for (int i = 0; i < n && nn < MAXN - 2; i++) {
        snprintf(cls, sizeof cls, "c%d t%d", i % 97, i % 13);
        int e = el(i % 7 == 0 ? div : cur, tags[i % 8], 0, cls);
        if (i % 5 == 0) cur = e;
        if (i % 3 == 0) at(e, "href", "x");
    }
    size_t cap = (size_t)rules * 120 + 1000;
    char *css = malloc(cap), *w = css;
    for (int r = 0; r < rules; r++) {
        switch (r % 5) {
        case 0: w += sprintf(w, ".c%d{color:#%06x;margin:%dpx}\n", r % 97, r * 2654435u & 0xFFFFFF, r % 9); break;
        case 1: w += sprintf(w, ".mw-body .t%d a{text-decoration:none;padding:%dpx %dem}\n", r % 13, r % 5, r % 3); break;
        case 2: w += sprintf(w, "div > .c%d + span.t%d{font-size:%dpx;background:#abc}\n", r % 97, r % 13, 10 + r % 8); break;
        case 3: w += sprintf(w, "#content p:not(.c%d):nth-child(%dn+1){line-height:1.%d}\n", r % 97, r % 4 + 1, r % 9); break;
        default: w += sprintf(w, "td.c%d, li.t%d, a[href]{border:1px solid #ddd;display:block}\n", r % 97, r % 13); break;
        }
    }
    new_page(css);
    clock_t t0 = clock();
    static struct cprops cs[MAXN];
    for (int i = 1; i < nn; i++) css_compute(i, 0, N[i].par ? &cs[N[i].par] : 0, &cs[i]);
    double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("bench: %d elements, %d rules: css_compute %.1f ms total, %.2f us/element\n", nn - 1, rules, ms, ms * 1000 / (nn - 1));
    clock_t t1 = clock();
    new_page(css);
    printf("bench: parse %d rules (%zu bytes): %.1f ms\n", rules, (size_t)(w - css), (clock() - t1) * 1000.0 / CLOCKS_PER_SEC);
    free(css);
    CHECK(cs[nn - 1].v[P_FSIZE] > 0);
}

int main(int argc, char **argv) {
    test_colors();
    test_selectors();
    test_cascade();
    test_vars_media();
    test_pseudo_ua();
    test_extras();
    test_restyle();
    bench(argc > 1 ? atoi(argv[1]) : 20000, 3000);
    printf("csstest: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
