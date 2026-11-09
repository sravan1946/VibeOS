/* dom.c - HTML text -> node tree. Tolerant tokenizer plus the implied-end-tag rules that matter for structure (<p> closed by
 * blocks, <li>, <td>, <tr>, <option>...). <style> and <link rel=stylesheet> are handed to css.c as they stream past, in
 * document order; <title> goes to the doc. Text keeps its whitespace (layout collapses it); entities are decoded here, with
 * &nbsp; kept as U+00A0 so layout can tell it from a breakable space. <script>/<svg>/<template> are dropped.
 * ponytail: no foster parenting, no adoption agency (misnested <b><i></b></i> just closes both), depth capped at 200. */
#include "browser.h"
#include "layout.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct dom HD;

static const struct { const char *n; int t; } TAGS[] = {
    { "html", T_HTML }, { "head", T_HEAD }, { "body", T_BODY },
    { "h1", T_H1 }, { "h2", T_H2 }, { "h3", T_H3 }, { "h4", T_H4 }, { "h5", T_H5 }, { "h6", T_H6 },
    { "p", T_P }, { "div", T_DIV }, { "section", T_DIV }, { "article", T_DIV }, { "header", T_DIV }, { "footer", T_DIV },
    { "nav", T_DIV }, { "main", T_DIV }, { "aside", T_DIV }, { "figure", T_DIV }, { "figcaption", T_DIV }, { "address", T_DIV },
    { "details", T_DIV }, { "summary", T_DIV }, { "fieldset", T_DIV }, { "hgroup", T_DIV }, { "dialog", T_DIV }, { "noscript", T_DIV },
    { "legend", T_LEGEND }, { "form", T_FORM }, { "input", T_INPUT }, { "textarea", T_TEXTAREA }, { "select", T_SELECT },
    { "option", T_OPTION }, { "optgroup", T_OPTION }, { "button", T_BUTTON }, { "center", T_CENTER },
    { "ul", T_UL }, { "menu", T_UL }, { "dir", T_UL }, { "ol", T_OL }, { "li", T_LI }, { "dl", T_DL }, { "dt", T_DT }, { "dd", T_DD },
    { "blockquote", T_BQ }, { "pre", T_PRE }, { "listing", T_PRE }, { "xmp", T_PRE },
    { "code", T_CODE }, { "tt", T_CODE }, { "kbd", T_CODE }, { "samp", T_CODE },
    { "b", T_B }, { "strong", T_B }, { "i", T_I }, { "em", T_I }, { "cite", T_I }, { "var", T_I }, { "dfn", T_I },
    { "u", T_U }, { "ins", T_U }, { "s", T_S }, { "strike", T_S }, { "del", T_S },
    { "sup", T_SUP }, { "sub", T_SUB }, { "small", T_SMALL }, { "big", T_BIG },
    { "a", T_A }, { "font", T_FONT }, { "br", T_BR }, { "hr", T_HR }, { "img", T_IMG },
    { "table", T_TABLE }, { "tr", T_TR }, { "td", T_TD }, { "th", T_TH }, { "caption", T_CAP },
    { "thead", T_TGROUP }, { "tbody", T_TGROUP }, { "tfoot", T_TGROUP }, { "col", T_COL }, { "colgroup", T_COL },
    { "iframe", T_SKIP }, { "object", T_SKIP }, { "canvas", T_SKIP }, { "video", T_SKIP }, { "audio", T_SKIP }, { "map", T_SKIP },
    { "meta", T_SKIP }, { "link", T_SKIP }, { "base", T_SKIP }, { "param", T_SKIP }, { "source", T_SKIP }, { "track", T_SKIP },
    { "datalist", T_SKIP }, { "embed", T_SKIP }, { "area", T_SKIP }, { "wbr", T_SKIP },
};

/* ---------- storage ---------- */
void hd_free(void) {
    free(HD.n); free(HD.a); free(HD.pool);
    memset(&HD, 0, sizeof HD);
}

static uint32_t pool_add(const char *s, size_t n) {
    if (HD.np + n + 1 > HD.pcap) {
        size_t nc = HD.pcap ? HD.pcap * 2 : 65536;
        while (nc < HD.np + n + 1) nc *= 2;
        char *p = realloc(HD.pool, nc);
        if (!p) return 0;
        HD.pool = p; HD.pcap = nc;
    }
    uint32_t off = HD.np;
    memcpy(HD.pool + off, s, n);
    HD.pool[off + n] = 0;
    HD.np += n + 1;
    return off;
}

#define MAXDOM 250000
static int node_new(int par, int type, int tag, uint32_t nm, int tn) {
    if (HD.nn >= MAXDOM) return -1;
    if (HD.nn == HD.cap) {
        int nc = HD.cap ? HD.cap * 2 : 1024;
        struct dnode *p = realloc(HD.n, nc * sizeof *p);
        if (!p) return -1;
        HD.n = p; HD.cap = nc;
    }
    int id = HD.nn++;
    HD.n[id] = (struct dnode){ .par = par, .first = -1, .last = -1, .next = -1, .prev = par >= 0 ? HD.n[par].last : -1, .type = type, .tag = tag, .nm = nm, .tn = tn };
    if (par >= 0) {
        if (HD.n[par].last >= 0) HD.n[HD.n[par].last].next = id; else HD.n[par].first = id;
        HD.n[par].last = id;
    }
    return id;
}

const char *hd_attr(int n, const char *name) {
    const struct dnode *d = &HD.n[n];
    for (int i = 0; i < d->na; i++) if (!strcmp(HD.pool + HD.a[d->a0 + i].n, name)) return HD.pool + HD.a[d->a0 + i].v;
    return 0;
}

/* ---------- entities ---------- */
uint32_t ent_lookup(const char *s, int n);   /* entities.c: the full HTML table (nbsp comes back as U+00A0) */

static int utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
    return 4;
}

int hd_ent(const char *s, int n, char *o, int *used) {
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
        static const uint16_t W1252[32] = { 0x20AC, 0xFFFD, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0xFFFD, 0x17D, 0xFFFD,
            0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0xFFFD, 0x17E, 0x178 };
        if (cp >= 0x80 && cp < 0xA0) cp = W1252[cp - 0x80];     /* numeric references in 0x80..0x9F are Windows-1252, as browsers do */
        if (cp < 32 && cp != 10 && cp != 9) cp = ' ';
        else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    } else {
        int j = i;
        while (j < n && j - i < 33 && isalnum((unsigned char)s[j])) j++;
        if (j >= n || s[j] != ';' || j == i) return 0;
        if (!(cp = ent_lookup(s + i, j - i))) return 0;
        i = j + 1;
    }
    *used = i;
    return utf8_put(cp, o);
}

/* decode in place; U+00A0 becomes a plain space (attribute values and labels have no use for the distinction) */
void html_unescape(char *s) {
    char *o = s;
    while (*s) {
        char u[8];
        int used, ul;
        if (*s == '&' && (ul = hd_ent(s, strlen(s), u, &used))) {
            if (ul == 2 && (unsigned char)u[0] == 0xC2 && (unsigned char)u[1] == 0xA0) { *o++ = ' '; s += used; continue; }
            memcpy(o, u, ul); o += ul; s += used;
        } else *o++ = *s++;
    }
    *o = 0;
}

/* ---------- tree building ---------- */
static int tag_id(const char *n) {
    for (unsigned i = 0; i < sizeof TAGS / sizeof *TAGS; i++) if (!strcmp(TAGS[i].n, n)) return TAGS[i].t;
    return T_NONE;
}

static int is_void(const char *n) {
    static const char *const V[] = { "br", "hr", "img", "input", "meta", "link", "area", "base", "col", "param", "source", "track", "wbr", "embed", 0 };
    for (int i = 0; V[i]; i++) if (!strcmp(n, V[i])) return 1;
    return 0;
}

#define MAXSTK 200
static int stk[MAXSTK], nstk;
static const char *cur_base;
static struct doc *cur_doc;

static int top(void) { return nstk ? stk[nstk - 1] : 0; }
static const char *nm(int n) { return HD.pool + HD.n[n].nm; }

/* pop the stack down to and including index i */
static void pop_to(int i) { nstk = i; }

/* nearest open element called `name` above any of the `stop` names (space-separated); its stack index or -1 */
static int find_open(const char *name, const char *stop) {
    for (int i = nstk - 1; i >= 0; i--) {
        const char *t = nm(stk[i]);
        if (!strcmp(t, name)) return i;
        int l = strlen(t);
        for (const char *q = stop; *q; ) {
            int ql = strcspn(q, " ");
            if (ql == l && !strncmp(q, t, l)) return -1;
            q += ql + (q[ql] != 0);
        }
    }
    return -1;
}

static int in_set(const char *n, const char *set) {
    int l = strlen(n);
    for (const char *q = set; *q; ) {
        int ql = strcspn(q, " ");
        if (ql == l && !strncmp(q, n, l)) return 1;
        q += ql + (q[ql] != 0);
    }
    return 0;
}

#define SCOPE "html table td th caption button object"
static void implied_end(const char *n) {
    int i;
    if (in_set(n, "address article aside blockquote center details dialog dir div dl fieldset figcaption figure footer form h1 h2 h3 h4 h5 h6 header hgroup hr li main menu nav ol p pre section summary table ul dd dt listing xmp")
        && (i = find_open("p", SCOPE)) >= 0) pop_to(i);
    if (!strcmp(n, "li") && (i = find_open("li", "ul ol menu dir table")) >= 0) pop_to(i);
    else if ((!strcmp(n, "dd") || !strcmp(n, "dt"))) {
        int a = find_open("dd", "dl table"), b = find_open("dt", "dl table");
        i = a > b ? a : b;
        if (i >= 0) pop_to(i);
    } else if (!strcmp(n, "td") || !strcmp(n, "th")) {
        int a = find_open("td", "table"), b = find_open("th", "table");
        i = a > b ? a : b;
        if (i >= 0) pop_to(i);
    } else if (!strcmp(n, "tr")) {
        if ((i = find_open("tr", "table")) >= 0) pop_to(i);
    } else if (!strcmp(n, "thead") || !strcmp(n, "tbody") || !strcmp(n, "tfoot")) {
        int a = find_open("tbody", "table"), b = find_open("thead", "table"), c = find_open("tfoot", "table");
        i = a > b ? a : b;
        if (c > i) i = c;
        int r = find_open("tr", "table");
        if (r > i) i = r;
        if (i >= 0) pop_to(i);
    } else if (!strcmp(n, "option") || !strcmp(n, "optgroup")) {
        if ((i = find_open("option", "select")) >= 0) pop_to(i);
    } else if (!strcmp(n, "a") && (i = find_open("a", "table td th")) >= 0) pop_to(i);
    if (nstk && !strcmp(nm(top()), "head") && !in_set(n, "title meta link style script base noscript template head"))
        pop_to(nstk - 1);                                 /* body content without a <body>: the head is over */
    if (!strcmp(n, "body") && (i = find_open("head", "")) >= 0) pop_to(i);
}

static void add_text(const char *s, int n) {
    if (n <= 0) return;
    char *o = malloc(n * 4 / 3 + 8), *p;
    if (!o) return;
    p = o;
    for (int i = 0; i < n; ) {
        unsigned char c = s[i];
        if (c == '&') {
            char u[8];
            int used, ul = hd_ent(s + i, n - i, u, &used);
            if (ul) { memcpy(p, u, ul); p += ul; i += used; continue; }
        }
        if (c == '\r') { i++; continue; }
        if (c < 32 && c != '\n' && c != '\t' && c != '\f') { i++; continue; }
        *p++ = c == '\f' ? ' ' : c; i++;
    }
    int len = p - o;
    if (len) {
        int par = top(), last = HD.n[par].last;
        if (last >= 0 && HD.n[last].type == 3 && HD.n[last].nm + HD.n[last].tn + 1 == HD.np) {   /* continue the previous text node (after a comment) */
            HD.np--;
            pool_add(o, len);
            HD.n[last].tn += len;
        } else {
            uint32_t off = pool_add(o, len);
            node_new(par, 3, 0, off, len);
        }
    }
    free(o);
}

/* attributes of the tag text tb (after the name) into the attribute table; returns the first index */
static int add_attrs(char *p, int *cnt) {
    int a0 = HD.na;
    *cnt = 0;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*cnt < 40) {
        while (*p && (isspace((unsigned char)*p) || *p == '/')) p++;
        if (!*p) break;
        char *n = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '/') { *p = tolower((unsigned char)*p); p++; }
        char *v = "";
        char c = *p;
        if (*p) *p++ = 0;
        if (c && isspace((unsigned char)c)) { while (*p && isspace((unsigned char)*p)) p++; c = *p == '=' ? '=' : 0; if (c) p++; }
        if (c == '=') {
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '"' || *p == '\'') { char q = *p++; v = p; while (*p && *p != q) p++; }
            else { v = p; while (*p && !isspace((unsigned char)*p)) p++; }
            if (*p) *p++ = 0;
            html_unescape(v);
        }
        if (!*n) continue;
        int dup = 0;
        for (int i = a0; i < HD.na; i++) if (!strcmp(HD.pool + HD.a[i].n, n)) dup = 1;
        if (dup) continue;
        if (HD.na == HD.acap) {
            int nc = HD.acap ? HD.acap * 2 : 1024;
            struct dattr *na = realloc(HD.a, nc * sizeof *na);
            if (!na) break;
            HD.a = na; HD.acap = nc;
        }
        HD.a[HD.na].n = pool_add(n, strlen(n));
        HD.a[HD.na].v = pool_add(v, strlen(v));
        HD.na++; (*cnt)++;
    }
    return a0;
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

static void capture_title(struct doc *d, const char *s, int n) {
    char *o = d->title;
    int k = 0, sp = 0;
    for (int i = 0; i < n && k < (int)sizeof d->title - 5; ) {
        unsigned char c = s[i];
        char u[8];
        int used, ul;
        if (c == '&' && (ul = hd_ent(s + i, n - i, u, &used))) { memcpy(o + k, u, ul); k += ul; i += used; sp = 0; continue; }
        if (isspace(c)) { if (k && !sp) o[k++] = ' '; sp = 1; i++; continue; }
        sp = 0;
        o[k++] = c; i++;
    }
    while (k && o[k - 1] == ' ') k--;
    o[k] = 0;
}

static int word_in(const char *list, const char *w) {           /* w is a space-separated word of list (any case) */
    int l = strlen(w);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e - p == l && !strncasecmp(p, w, l)) return 1;
        p = e;
    }
    return 0;
}

/* <link rel=stylesheet href media>: the sheet joins the cascade here, in document order (nothing yet while it loads) */
static void link_sheet(int n) {
    const char *rel = hd_attr(n, "rel"), *href = hd_attr(n, "href"), *media = hd_attr(n, "media");
    if (!rel || !href || !*href || !word_in(rel, "stylesheet") || word_in(rel, "alternate") || hd_attr(n, "disabled") || !css_media(media)) return;
    char u[URL_MAX];
    size_t len;
    url_resolve(cur_base, href, u, sizeof u);
    const char *t = sheet_get(u, &len);
    if (t) css_add_from(t, len, u);
}

int hd_parse(const char *src, size_t n, const char *base, struct doc *d) {
    hd_free();
    cur_base = base;
    node_new(-1, 9, 0, pool_add("#root", 5), 0);
    nstk = 0;
    stk[nstk++] = 0;
    size_t i = 0;
    { size_t k = 0; while (k < n && isspace((unsigned char)src[k])) k++; HD.quirks = !(k + 9 <= n && !strncasecmp(src + k, "<!doctype", 9)); }   /* no doctype: quirks mode (tables don't inherit text-align) */
    char tb[2048];
    while (i < n) {
        if (src[i] != '<') {
            const char *e = memchr(src + i, '<', n - i);
            size_t j = e ? (size_t)(e - src) : n;
            add_text(src + i, j - i);
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
        if (k >= n || !isalpha((unsigned char)src[k])) { add_text("<", 1); i++; continue; }
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
        char name[64];                                        /* custom elements have long names (devsite-language-selector) */
        int nl = 0;
        while (nl < 63 && tb[nl] && !isspace((unsigned char)tb[nl]) && tb[nl] != '/') { name[nl] = tolower((unsigned char)tb[nl]); nl++; }
        name[nl] = 0;
        if (closing) {
            if (!strcmp(name, "br")) strcpy(name, "br"), closing = 0;
            else {
                int e = nstk - 1;
                while (e > 0 && strcmp(nm(stk[e]), name)) e--;
                if (e > 0 && strcmp(name, "body") && strcmp(name, "html")) pop_to(e);
                continue;
            }
        }
        if (!strcmp(name, "svg")) {                           /* inline SVG: the raw markup in a text child, rasterised by layout */
            int ce, next = skip_raw(src, i, n, name, &ce);
            int a0, na;
            a0 = add_attrs(tb, &na);
            int id = node_new(top(), 1, T_SVG, pool_add("svg", 3), 0);
            if (id >= 0) {
                HD.n[id].a0 = a0; HD.n[id].na = na;
                size_t s0 = k - 1, len = next - s0;
                node_new(id, 3, 0, pool_add(src + s0, len), len);
            }
            i = next;
            continue;
        }
        if (!strcmp(name, "script") || !strcmp(name, "template") || !strcmp(name, "style") || !strcmp(name, "title")) {
            int ce, next = skip_raw(src, i, n, name, &ce);
            if (!strcmp(name, "title")) { if (!d->title[0]) capture_title(d, src + i, ce - i); }
            else if (!strcmp(name, "style")) {
                int a0, na;
                const char *media = 0;
                a0 = add_attrs(tb, &na);
                for (int q2 = 0; q2 < na; q2++) if (!strcmp(HD.pool + HD.a[a0 + q2].n, "media")) media = HD.pool + HD.a[a0 + q2].v;
                if (css_media(media)) css_add_from(src + i, ce - i, cur_base);
                HD.na = a0;                                  /* only needed to read media */
            }
            i = next;
            continue;
        }
        int a0, na;
        a0 = add_attrs(tb, &na);
        int tag = tag_id(name);
        implied_end(name);
        int id = node_new(top(), 1, tag, pool_add(name, strlen(name)), 0);
        if (id < 0) break;
        HD.n[id].a0 = a0; HD.n[id].na = na;
        if (!strcmp(name, "link")) link_sheet(id);
        if (is_void(name)) continue;
        if (tag == T_TEXTAREA) {                              /* raw text up to </textarea> */
            int ce, next = skip_raw(src, i, n, name, &ce);
            if (ce > (int)i) {
                int save = nstk;
                stk[nstk++] = id;
                int a = ce > (int)i + 1 && src[i] == '\r' && src[i + 1] == '\n' ? 2 : src[i] == '\n' ? 1 : 0;   /* the first newline is dropped */
                add_text(src + i + a, ce - i - a);
                nstk = save;
            }
            i = next;
            continue;
        }
        if (nstk < MAXSTK) stk[nstk++] = id;
    }
    return HD.nn;
}

uint32_t hd_pool_add(const char *s, size_t n) { return pool_add(s, n); }

/* text/plain: html > body > pre */
void hd_plain(const char *src, size_t n) {
    hd_free();
    node_new(-1, 9, 0, pool_add("#root", 5), 0);
    int h = node_new(0, 1, T_HTML, pool_add("html", 4), 0), b = node_new(h, 1, T_BODY, pool_add("body", 4), 0), p = node_new(b, 1, T_PRE, pool_add("pre", 3), 0);
    char *o = malloc(n + 1);
    if (!o) return;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (src[i] != '\r' && ((unsigned char)src[i] >= 32 || src[i] == '\n' || src[i] == '\t')) o[k++] = src[i];
    node_new(p, 3, 0, pool_add(o, k), k);
    free(o);
}

/* ---------- the script engine's tree (js.c accessors) -> HD ----------
 * One tree per page: for a page with scripts the layout reads js.c's composed tree directly (no serialise/parse). Nodes get dense
 * ids of our own; `js` keeps the js.c id, which items, controls, forms and js_set_rect() use. Scripts, templates and comments are
 * dropped here; <style>/<link> are fed to css.c in document order; inline <svg> becomes an element with its markup in a text child,
 * as the parser does; live control state (value, checked, selected option) replaces the attributes. */
struct sbuf { char *s; size_t n, cap; };
static void sb_put(struct sbuf *b, const char *t, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + n + 1) nc *= 2;
        char *p = realloc(b->s, nc);
        if (!p) return;
        b->s = p; b->cap = nc;
    }
    memcpy(b->s + b->n, t, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void sb_esc(struct sbuf *b, const char *t, int attr) {
    for (; *t; t++) {
        if (*t == '&') sb_put(b, "&amp;", 5);
        else if (*t == '<') sb_put(b, "&lt;", 4);
        else if (*t == '>') sb_put(b, "&gt;", 4);
        else if (attr && *t == '"') sb_put(b, "&quot;", 6);
        else sb_put(b, t, 1);
    }
}
static void svg_ser(struct sbuf *b, int j, int depth) {
    int ty = dom_type(j);
    if (ty == 3) { sb_esc(b, dom_text(j), 0); return; }
    if (ty != 1 || depth > 40) return;
    const char *tag = dom_tag(j);
    sb_put(b, "<", 1); sb_put(b, tag, strlen(tag));
    for (int i = 0, n = dom_nattr(j); i < n; i++) {
        const char *an, *av;
        if (!dom_attr_at(j, i, &an, &av)) continue;
        static const char *const cs[] = { "viewBox", "preserveAspectRatio", "gradientUnits", "gradientTransform", "spreadMethod", "patternUnits", "patternTransform", "clipPathUnits" };
        for (size_t k = 0; k < sizeof cs / sizeof *cs; k++) if (!strcasecmp(an, cs[k])) { an = cs[k]; break; }   /* the JS tree lowercases attribute names; nanosvg wants SVG's camelCase */
        sb_put(b, " ", 1); sb_put(b, an, strlen(an)); sb_put(b, "=\"", 2); sb_esc(b, av, 1); sb_put(b, "\"", 1);
    }
    sb_put(b, ">", 1);
    for (int c = dom_first(j); c; c = dom_next(c)) svg_ser(b, c, depth + 1);
    sb_put(b, "</", 2); sb_put(b, tag, strlen(tag)); sb_put(b, ">", 1);
}

static int sel_want = -1, sel_i;                              /* the open <select>'s selected index and the option counter */

static void js_copy(int j, int par, struct doc *d, int depth) {
    int ty = dom_type(j);
    if (ty == 3) {
        const char *t = dom_text(j);
        size_t n = strlen(t);
        if (!n) return;
        int id = node_new(par, 3, 0, pool_add(t, n), n);
        if (id >= 0) HD.n[id].js = j;
        return;
    }
    if (ty != 1 || depth > 190) return;
    const char *name = dom_tag(j);
    if (!strcmp(name, "script") || !strcmp(name, "template")) return;
    if (!strcmp(name, "title")) {
        for (int c = dom_first(j); c; c = dom_next(c)) if (dom_type(c) == 3 && !cur_doc->title[0]) capture_title(cur_doc, dom_text(c), strlen(dom_text(c)));
        return;
    }
    if (!strcmp(name, "style")) {
        struct sbuf b = { 0 };
        for (int c = dom_first(j); c; c = dom_next(c)) if (dom_type(c) == 3) sb_put(&b, dom_text(c), strlen(dom_text(c)));
        const char *media = dom_attr(j, "media");
        if (b.s && css_media(media)) css_add_from(b.s, b.n, cur_base);
        free(b.s);
        return;
    }
    /* attributes, with the live control state folded in */
    const char *an[64], *av[64];
    int na = 0;
    int tag = tag_id(name), isinput = !strcmp(name, "input"), isarea = !strcmp(name, "textarea"), issel = !strcmp(name, "select"), isopt = !strcmp(name, "option");
    const char *ctlv = 0;
    int checked = -1;
    if (isinput || isarea || issel) {
        const char *ty2 = dom_attr(j, "type");
        int textual = isinput && (!ty2 || !(!strcasecmp(ty2, "checkbox") || !strcasecmp(ty2, "radio") || !strcasecmp(ty2, "hidden") || !strcasecmp(ty2, "submit") ||
                                           !strcasecmp(ty2, "button") || !strcasecmp(ty2, "image") || !strcasecmp(ty2, "reset")));
        if (textual || isarea) ctlv = dom_ctl_value(j);
        if (isinput && !textual && ty2 && (!strcasecmp(ty2, "checkbox") || !strcasecmp(ty2, "radio"))) checked = dom_ctl_checked(j);
    }
    char *ctlcopy = ctlv ? strdup(ctlv) : 0;
    for (int i = 0, n = dom_nattr(j); i < n && na < 62; i++) {
        const char *a, *v;
        if (!dom_attr_at(j, i, &a, &v)) continue;
        if (!strcmp(a, "data-vjs")) continue;
        if (isinput && ctlcopy && !strcmp(a, "value")) continue;
        if (checked >= 0 && !strcmp(a, "checked")) continue;
        if (isopt && sel_want >= 0 && !strcmp(a, "selected")) continue;
        an[na] = a; av[na] = v; na++;
    }
    if (isinput && ctlcopy) { an[na] = "value"; av[na] = ctlcopy; na++; }
    if (checked > 0) { an[na] = "checked"; av[na] = ""; na++; }
    if (isopt && sel_want >= 0) { if (sel_i == sel_want) { an[na] = "selected"; av[na] = ""; na++; } sel_i++; }
    int a0 = HD.na;
    for (int i = 0; i < na; i++) {
        if (HD.na == HD.acap) {
            int nc = HD.acap ? HD.acap * 2 : 1024;
            struct dattr *nat = realloc(HD.a, nc * sizeof *nat);
            if (!nat) break;
            HD.a = nat; HD.acap = nc;
        }
        HD.a[HD.na].n = pool_add(an[i], strlen(an[i]));
        HD.a[HD.na].v = pool_add(av[i], strlen(av[i]));
        HD.na++;
    }
    int id = node_new(par, 1, !strcmp(name, "svg") ? T_SVG : tag, pool_add(name, strlen(name)), 0);
    if (id < 0) { free(ctlcopy); return; }
    HD.n[id].a0 = a0; HD.n[id].na = HD.na - a0; HD.n[id].js = j;
    if (!strcmp(name, "link")) link_sheet(id);
    if (tag == T_SVG || !strcmp(name, "svg")) {
        struct sbuf b = { 0 };
        svg_ser(&b, j, 0);
        if (b.s) node_new(id, 3, 0, pool_add(b.s, b.n), b.n);
        free(b.s);
    } else if (isarea) {
        if (ctlcopy && *ctlcopy) node_new(id, 3, 0, pool_add(ctlcopy, strlen(ctlcopy)), strlen(ctlcopy));
    } else if (!is_void(name)) {
        int save_want = sel_want, save_i = sel_i;
        if (issel) { sel_want = dom_ctl_sel(j); sel_i = 0; }
        for (int c = dom_first(j); c; c = dom_next(c)) {
            js_copy(c, id, d, depth + 1);
        }
        sel_want = save_want; sel_i = save_i;
    }
    free(ctlcopy);
}

int hd_from_js(struct doc *d, const char *base) {
    hd_free();
    cur_base = base; cur_doc = d;
    node_new(-1, 9, 0, pool_add("#root", 5), 0);
    HD.js = 1;
    for (int c = dom_first(1); c; c = dom_next(c)) js_copy(c, 0, d, 0);
    return HD.nn;
}
