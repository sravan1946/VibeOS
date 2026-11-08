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
static const struct { const char *n; uint32_t cp; } ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "deg", 0xB0 },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "plusmn", 0xB1 }, { "sect", 0xA7 }, { "para", 0xB6 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "harr", 0x2194 },
    { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "micro", 0xB5 },
    { "frac12", 0xBD }, { "frac14", 0xBC }, { "frac34", 0xBE }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    { "agrave", 0xE0 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF },
    { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "ne", 0x2260 },
    { "le", 0x2264 }, { "ge", 0x2265 }, { "infin", 0x221E }, { "ensp", ' ' }, { "emsp", ' ' }, { "thinsp", ' ' },
    { "zwnj", 0x200C }, { "zwj", 0x200D }, { "shy", 0xAD }, { "minus", 0x2212 }, { "check", 0x2713 },
};

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
        if (cp < 32 && cp != 10 && cp != 9) cp = ' ';
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
        char name[16];
        int nl = 0;
        while (nl < 15 && tb[nl] && !isspace((unsigned char)tb[nl]) && tb[nl] != '/') { name[nl] = tolower((unsigned char)tb[nl]); nl++; }
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
        if (!strcmp(name, "script") || !strcmp(name, "svg") || !strcmp(name, "template") || !strcmp(name, "style") || !strcmp(name, "title")) {
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
