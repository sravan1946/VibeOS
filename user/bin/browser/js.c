/* js.c - JavaScript in the browser: QuickJS (third_party/quickjs) plus a small DOM.
 * A page with scripts is parsed into a node tree here. Layout (html.c) runs on that tree serialised back to HTML with
 * data-vjs="ID" on every element, so display items, controls and forms know their node. Scripts change the tree through
 * the natives in this file (the object __d, wrapped into DOM classes by the prelude in dom_js.c); a change to the
 * document marks it dirty and main.c re-serialises and re-lays it out, at most once per frame. Events go the other
 * way: main.c finds the node under the pointer or the focused control and calls js_event -> __dispatch.
 * Every entry into JS (a script, a handler, a timer) gets RUN_MS: the interrupt handler aborts a runaway script, and
 * the page's scripts are then switched off. Form controls keep their live state in form.c (by ordinal); .value and
 * .checked read and write it there, and js_sync copies it into the tree before a relayout renumbers the controls.
 * ponytail: whole-page relayout per change, no capture phase, no document.write, scripts inserted later don't run,
 * alert() doesn't block, fetch() is GET only, detached nodes are only freed when the page goes (200k node ids). */
#include "browser.h"
#include <quickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscall.h>
#include <sys/stat.h>

#define RUN_MS 10000
#define MAXNODES 200000
#define MAXT 256
#define MAXF 6
#define MAXSCR 512
#define MAXDYN 64
#define NLOG 64

struct node {
    uint8_t type;                 /* 1 element, 3 text, 8 comment, 9 document, 11 fragment */
    uint8_t has_val, has_chk, chk, started;   /* started: a <script> that has run (or been taken) */   /* control state set by a script or synced from form.c */
    char tag[48];                 /* lower case */
    char *text;                   /* text node data */
    char **an, **av; int na;      /* attributes */
    char *val; int sel;           /* value / selectedIndex (select: has_val means sel is set) */
    int parent, first, last, next, prev;
};
static struct node *N;
static int nn, ncap;

static JSRuntime *rt;
static JSContext *ctx;
static JSValue fn_dispatch, fn_fire, fn_fetch;
static struct doc *live;
static int active, dirty, dead, killed, loaded, focus_req, pumps;
static uint32_t deadline;
static char page_url[URL_MAX], nav[URL_MAX];
static char *alert_text;

/* scripts: S = parser order (sync and defer/module entries, document.write'd ones spliced in after the running one),
 * D = async and dynamically inserted ones, which run whenever their download lands. mode 0 sync, 1 defer, 2 async;
 * state 0 idle, 1 downloading, 2 ready, 3 failed, 4 done */
struct scr { int node, mode, state, module, fid, rel; char *body, *url; };   /* rel: an async parser script waits until the parser (cur_scr) has got this far */
static struct scr S[MAXSCR], D[MAXDYN];
static int nscr, cur_scr, cur_def, cur_script, rstate, idle, strikes, url_changed, scroll_req_y = -1, vscroll, vw = 900, vh = 580;
static char anchor_req[128];
static int submit_req;
static char *sync_get(const char *url, size_t *len);
static void add_script(int id, int where);
static void scan_scripts(int id, int where);
static void exec_entry(struct scr *e);
static char *mod_norm(JSContext *c, const char *base, const char *name, void *op);
static JSModuleDef *mod_load(JSContext *c, const char *name, void *op);

static struct { int id, every; uint32_t due; } T[MAXT];
static int nt, next_tid = 1;

/* fetch() and <script src>: helper processes like page loads (fetch.c); file: URLs are read on the next pump */
static struct jf { int id, used, hops, script; struct fetch f; char url[URL_MAX]; int file; const char *fail; char *post; } F[MAXF];
static struct { int id; char url[URL_MAX]; int script; char *post; } FQ[64];   /* waiting for a slot */
static int nfq, next_fid = 1;

static struct { char t[200]; int kind; } LOG[NLOG];
static int nlog;

/* ---------- console ---------- */
static void jlog(int kind, const char *fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    for (char *p = b; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    if (nlog == NLOG) { memmove(LOG, LOG + 1, (NLOG - 1) * sizeof *LOG); nlog--; }
    strlcpy(LOG[nlog].t, b, sizeof LOG[0].t);
    LOG[nlog++].kind = kind;
    static int kfd = -2;                                   /* /dev/kmsg = serial: tests read the page's verdict there */
    if (kfd == -2) kfd = open("/dev/kmsg", O_WRONLY);
    if (kfd >= 0) { char l[240]; int k = snprintf(l, sizeof l, "[js%d] %s\n", kind, b); write(kfd, l, k); }
    browser_status(kind == 2, "%s%s", kind == 2 ? "JS: " : "console: ", b);
}
int js_log_count(void) { return nlog; }
const char *js_log_line(int i, int *kind) { *kind = LOG[i].kind; return LOG[i].t; }

/* ---------- tree ---------- */
static char *sdup(const char *s) { return strdup(s ? s : ""); }

static int new_node(int type, const char *tag) {
    if (nn >= MAXNODES) return 0;
    if (nn == ncap) {
        int nc = ncap ? ncap * 2 : 512;
        struct node *n2 = realloc(N, nc * sizeof *N);
        if (!n2) return 0;
        N = n2; ncap = nc;
    }
    memset(&N[nn], 0, sizeof *N);
    N[nn].type = type;
    if (tag) {
        strlcpy(N[nn].tag, tag, sizeof N[0].tag);
        for (char *p = N[nn].tag; *p; p++) *p = tolower((unsigned char)*p);
    }
    return nn++;
}

static int new_text(const char *s, int n) {
    int id = new_node(3, 0);
    if (id) N[id].text = strndup(s, n);
    return id;
}

static void unlink_node(int c) {
    struct node *x = &N[c];
    if (!x->parent) return;
    if (x->prev) N[x->prev].next = x->next; else N[x->parent].first = x->next;
    if (x->next) N[x->next].prev = x->prev; else N[x->parent].last = x->prev;
    x->parent = x->prev = x->next = 0;
}

static void insert_before(int p, int c, int ref) {       /* ref 0 = append */
    unlink_node(c);
    struct node *x = &N[c];
    x->parent = p;
    if (!ref) {
        x->prev = N[p].last;
        if (N[p].last) N[N[p].last].next = c; else N[p].first = c;
        N[p].last = c;
    } else {
        x->next = ref; x->prev = N[ref].prev;
        if (N[ref].prev) N[N[ref].prev].next = c; else N[p].first = c;
        N[ref].prev = c;
    }
}

static const char *attr(int id, const char *name) {
    struct node *x = &N[id];
    for (int i = 0; i < x->na; i++) if (!strcmp(x->an[i], name)) return x->av[i];
    return 0;
}

static int set_attr(int id, const char *name, const char *v) {    /* 1 = changed */
    struct node *x = &N[id];
    for (int i = 0; i < x->na; i++)
        if (!strcmp(x->an[i], name)) {
            if (!strcmp(x->av[i], v)) return 0;
            free(x->av[i]); x->av[i] = sdup(v);
            return 1;
        }
    if (x->na >= 64) return 0;
    char **an = realloc(x->an, (x->na + 1) * sizeof *an), **av = an ? realloc(x->av, (x->na + 1) * sizeof *av) : 0;
    if (an) x->an = an;
    if (!av) return 0;
    x->av = av;
    x->an[x->na] = sdup(name); x->av[x->na] = sdup(v); x->na++;
    return 1;
}

static int del_attr(int id, const char *name) {
    struct node *x = &N[id];
    for (int i = 0; i < x->na; i++)
        if (!strcmp(x->an[i], name)) {
            free(x->an[i]); free(x->av[i]);
            x->an[i] = x->an[x->na - 1]; x->av[i] = x->av[x->na - 1]; x->na--;
            return 1;
        }
    return 0;
}

static int connected(int id) { while (id && id != 1) id = N[id].parent; return id == 1; }
static void touch(int id) { if (connected(id)) dirty = 1; }

static int in_list(const char *t, const char *const *l) { for (; *l; l++) if (!strcmp(t, *l)) return 1; return 0; }
static const char *const VOIDS[] = { "br", "hr", "img", "input", "meta", "link", "area", "base", "col", "param", "source", "track", "wbr", "embed", 0 };
static const char *const RAWS[] = { "script", "style", "textarea", "title", "xmp", "noscript", 0 };   /* content up to </name> is text */
static const char *const HEADS[] = { "title", "style", "meta", "link", "script", "base", "noscript", 0 };
static const char *const PCLOSE[] = { "address", "article", "aside", "blockquote", "center", "details", "div", "dl", "fieldset",
    "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "menu", "nav", "ol",
    "p", "pre", "section", "table", "ul", "li", "dd", "dt", 0 };
static const char *const INLINES[] = { "a", "b", "i", "u", "s", "em", "strong", "span", "code", "small", "big", "font", "sub", "sup",
    "tt", "kbd", "samp", "var", "cite", "dfn", "abbr", "q", "mark", "strike", "del", "ins", "label", "img", "br", 0 };

/* ---------- parser: a tolerant tree builder (implied </p> </li> </td> ..., head/body inference) ---------- */
static struct { int st[256], n, frag, body; } P;
static int top(void) { return P.st[P.n - 1]; }

static void ensure_body(void) {
    if (P.frag || P.body) return;
    P.body = new_node(1, "body");
    if (!P.body) return;
    insert_before(2, P.body, 0);
    P.st[0] = 1; P.st[1] = 2; P.st[2] = P.body; P.n = 3;
}

/* text with entities decoded; &nbsp; stays U+00A0 so it survives the round trip */
static void add_text(int parent, const char *s, int n, int decode) {
    if (!n) return;
    char *b = malloc(n * 2 + 1);
    if (!b) return;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (decode && s[i] == '&' && i + 6 <= n && !strncmp(s + i, "&nbsp;", 6)) { b[k++] = '\xC2'; b[k++] = '\xA0'; i += 5; }
        else b[k++] = s[i];
    }
    b[k] = 0;
    if (decode) html_unescape(b);
    struct node *pp = &N[parent];
    if (pp->last && N[pp->last].type == 3) {                /* merge with the previous text node */
        struct node *t = &N[pp->last];
        size_t a = strlen(t->text), l = strlen(b);
        char *nt = realloc(t->text, a + l + 1);
        if (nt) { memcpy(nt + a, b, l + 1); t->text = nt; }
    } else {
        int id = new_text(b, strlen(b));
        if (id) insert_before(parent, id, 0);
    }
    free(b);
}

static void text_tok(const char *s, int n) {
    if (!P.frag && !P.body) {
        int ws = 1;
        for (int i = 0; i < n && ws; i++) ws = isspace((unsigned char)s[i]);
        if (ws) return;
        ensure_body();
    }
    add_text(top(), s, n, 1);
}

static void parse_attrs_into(int id, const char *p, const char *e) {
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == '/')) p++;
        if (p >= e) break;
        const char *ns = p;
        while (p < e && !isspace((unsigned char)*p) && *p != '=' && *p != '/') p++;
        char name[64];
        int nl = p - ns < 63 ? (int)(p - ns) : 63;
        for (int i = 0; i < nl; i++) name[i] = tolower((unsigned char)ns[i]);
        name[nl] = 0;
        while (p < e && isspace((unsigned char)*p)) p++;
        const char *vs = "", *ve = vs;
        if (p < e && *p == '=') {
            p++;
            while (p < e && isspace((unsigned char)*p)) p++;
            if (p < e && (*p == '"' || *p == '\'')) { char q = *p++; vs = p; while (p < e && *p != q) p++; ve = p; if (p < e) p++; }
            else { vs = p; while (p < e && !isspace((unsigned char)*p)) p++; ve = p; }
        }
        if (!nl || attr(id, name)) continue;                 /* first one wins */
        char *v = strndup(vs, ve - vs);
        if (!v) return;
        html_unescape(v);
        set_attr(id, name, v);
        free(v);
    }
}

/* close the innermost `names` element unless a `stops` element comes first */
static void pop_until(const char *const *names, const char *const *stops) {
    for (int i = P.n - 1; i >= 1; i--) {
        const char *t = N[P.st[i]].tag;
        if (in_list(t, names)) { P.n = i; return; }
        if (in_list(t, stops)) return;
    }
}

static void close_p(void) {
    for (int i = P.n - 1; i >= 1; i--) {
        const char *t = N[P.st[i]].tag;
        if (!strcmp(t, "p")) { P.n = i; return; }
        if (!in_list(t, INLINES)) return;
    }
}

/* the element for a start tag; returns its id (0 = handled or dropped) */
static int start_tag(const char *name, const char *ap, const char *ae) {
    static const char *const LI[] = { "li", 0 }, *const LIS[] = { "ul", "ol", "menu", 0 };
    static const char *const DD[] = { "dt", "dd", 0 }, *const DDS[] = { "dl", 0 };
    static const char *const TR[] = { "tr", 0 }, *const TRS[] = { "table", "tbody", "thead", "tfoot", 0 };
    static const char *const TD[] = { "td", "th", 0 }, *const TDS[] = { "tr", "table", 0 };
    static const char *const OP[] = { "option", 0 }, *const OPS[] = { "select", "optgroup", 0 };
    int parent;
    if (!strcmp(name, "html") || !strcmp(name, "head") || !strcmp(name, "body")) {
        if (P.frag) return 0;
        int id = !strcmp(name, "html") ? 2 : !strcmp(name, "head") ? 3 : (ensure_body(), P.body);
        if (id) parse_attrs_into(id, ap, ae);
        return 0;
    }
    if (!P.frag && !P.body && in_list(name, HEADS)) parent = 3;
    else {
        ensure_body();
        if (in_list(name, PCLOSE)) close_p();
        if (!strcmp(name, "li")) pop_until(LI, LIS);
        else if (!strcmp(name, "dt") || !strcmp(name, "dd")) pop_until(DD, DDS);
        else if (!strcmp(name, "tr")) pop_until(TR, TRS);
        else if (!strcmp(name, "td") || !strcmp(name, "th")) pop_until(TD, TDS);
        else if (!strcmp(name, "option")) pop_until(OP, OPS);
        parent = top();
    }
    int id = new_node(1, name);
    if (!id) return 0;
    parse_attrs_into(id, ap, ae);
    insert_before(parent, id, 0);
    if (!in_list(name, VOIDS) && !in_list(name, RAWS) && parent == top() && P.n < 256) P.st[P.n++] = id;
    return id;
}

static void end_tag(const char *name) {
    if (!strcmp(name, "html") || !strcmp(name, "body") || !strcmp(name, "head")) return;
    if (!strcmp(name, "br")) { start_tag("br", "", ""); return; }
    for (int i = P.n - 1; i >= 1; i--)
        if (!strcmp(N[P.st[i]].tag, name)) { P.n = i; return; }
}

static void parse_html(int root, const char *s, size_t n, int frag) {
    P.st[0] = root; P.n = 1; P.frag = frag;
    if (!frag) P.body = 0;
    for (size_t i = 0; i < n; ) {
        if (s[i] != '<') {
            const char *e = memchr(s + i, '<', n - i);
            size_t j = e ? (size_t)(e - s) : n;
            text_tok(s + i, j - i);
            i = j;
            continue;
        }
        if (i + 4 <= n && !strncmp(s + i, "<!--", 4)) {
            size_t j = i + 4;
            while (j + 3 <= n && strncmp(s + j, "-->", 3)) j++;
            int id = new_node(8, 0);                       /* comment node (kept for innerHTML/outerHTML and createComment users) */
            if (id) { N[id].text = strndup(s + i + 4, j > i + 4 ? j - i - 4 : 0); insert_before(top(), id, 0); }
            i = j + 3 <= n ? j + 3 : n;
            continue;
        }
        if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
            const char *e = memchr(s + i, '>', n - i);
            i = e ? (size_t)(e - s) + 1 : n;
            continue;
        }
        size_t k = i + 1;
        int closing = k < n && s[k] == '/';
        k += closing;
        if (k >= n || !isalpha((unsigned char)s[k])) { text_tok("<", 1); i++; continue; }
        size_t j = k;
        char q = 0, prev = 0;
        for (; j < n; j++) {                               /* the tag's end, quotes after '=' respected (as html.c) */
            char c = s[j];
            if (q) { if (c == q) q = 0; }
            else if ((c == '"' || c == '\'') && prev == '=') q = c;
            else if (c == '>') break;
            if (!isspace((unsigned char)c)) prev = c;
        }
        char name[48];
        int nl = 0;
        size_t p = k;
        while (p < j && !isspace((unsigned char)s[p]) && s[p] != '/' && s[p] != '>') { if (nl < 47) name[nl++] = tolower((unsigned char)s[p]); p++; }
        name[nl] = 0;
        i = j < n ? j + 1 : n;
        if (closing) { end_tag(name); continue; }
        int id = start_tag(name, s + p, s + j);
        if (in_list(name, RAWS)) {                         /* raw text up to </name> */
            size_t ce = i;
            while (ce < n && !(s[ce] == '<' && ce + 2 + nl <= n && s[ce + 1] == '/' && !strncasecmp(s + ce + 2, name, nl))) ce++;
            size_t cs = i;
            if (!strcmp(name, "textarea") && cs < ce && s[cs] == '\n') cs++;
            if (id) add_text(id, s + cs, ce - cs, !strcmp(name, "textarea") || !strcmp(name, "title"));
            const char *e = ce < n ? memchr(s + ce, '>', n - ce) : 0;
            i = e ? (size_t)(e - s) + 1 : n;
        } else if (id && !strcmp(name, "pre") && i < n && s[i] == '\n') i++;
    }
    if (!frag) ensure_body();
}

/* ---------- serialiser ---------- */
struct buf { char *p; size_t n, cap; };
static void put(struct buf *b, const char *s, size_t l) {
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + l + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, l);
    b->n += l;
    b->p[b->n] = 0;
}
static void puts_(struct buf *b, const char *s) { put(b, s, strlen(s)); }
static void esc(struct buf *b, const char *s, int quote) {
    const char *r = s;
    for (; *s; s++) {
        const char *e = 0;
        int skip = 0;
        if (*s == '&') e = "&amp;";
        else if (*s == '<') e = "&lt;";
        else if (*s == '>') e = "&gt;";
        else if (*s == '"' && quote) e = "&quot;";
        else if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0) { e = "&nbsp;"; skip = 1; }
        if (!e) continue;
        put(b, r, s - r);
        puts_(b, e);
        s += skip;
        r = s + 1;
    }
    put(b, r, s - r);
}

static void text_of(struct buf *b, int id) {
    if (N[id].type == 3) { puts_(b, N[id].text); return; }
    if (N[id].type == 8) return;
    for (int c = N[id].first; c; c = N[c].next) text_of(b, c);
}

static struct { int on, want, i; } SEL;                  /* serialising a <select> whose choice a script/the user made */

static void ser(struct buf *b, int id, int lay) {
    struct node *x = &N[id];
    if (x->type == 8) { if (!lay) { puts_(b, "<!--"); puts_(b, x->text); puts_(b, "-->"); } return; }
    if (x->type == 3) {
        int p = x->parent;
        if (p && N[p].type == 1 && (!strcmp(N[p].tag, "script") || !strcmp(N[p].tag, "style") || !strcmp(N[p].tag, "xmp") || !strcmp(N[p].tag, "noscript"))) puts_(b, x->text);
        else esc(b, x->text, 0);
        return;
    }
    if (x->type != 1) { for (int c = x->first; c; c = N[c].next) ser(b, c, lay); return; }
    if (lay && (!strcmp(x->tag, "script") || !strcmp(x->tag, "noscript"))) return;   /* scripting is on: <noscript> shows nothing */
    char t[24];
    int input = !strcmp(x->tag, "input"), opt = !strcmp(x->tag, "option"), area = !strcmp(x->tag, "textarea");
    puts_(b, "<"); puts_(b, x->tag);
    if (lay) { snprintf(t, sizeof t, " data-vjs=\"%d\"", id); puts_(b, t); }
    for (int i = 0; i < x->na; i++) {
        const char *n = x->an[i];
        if (lay && ((input && x->has_val && !strcmp(n, "value")) || (input && x->has_chk && !strcmp(n, "checked")) ||
                    (opt && SEL.on && !strcmp(n, "selected")) || !strcmp(n, "data-vjs"))) continue;
        puts_(b, " "); puts_(b, n);
        if (x->av[i][0]) { puts_(b, "=\""); esc(b, x->av[i], 1); puts_(b, "\""); }
    }
    if (lay && input && x->has_val) { puts_(b, " value=\""); esc(b, x->val ? x->val : "", 1); puts_(b, "\""); }
    if (lay && input && x->has_chk && x->chk) puts_(b, " checked");
    if (lay && opt && SEL.on && SEL.i++ == SEL.want) puts_(b, " selected");
    puts_(b, ">");
    if (in_list(x->tag, VOIDS)) return;
    if (lay && (area || !strcmp(x->tag, "pre"))) puts_(b, "\n");           /* the layout drops one leading newline */
    int sel = !strcmp(x->tag, "select") && x->has_val;
    if (sel) { SEL.on = 1; SEL.want = x->sel; SEL.i = 0; }
    if (lay && area && x->has_val) esc(b, x->val ? x->val : "", 0);
    else for (int c = x->first; c; c = N[c].next) ser(b, c, lay);
    if (sel) SEL.on = 0;
    puts_(b, "</"); puts_(b, x->tag); puts_(b, ">");
}

char *js_html(size_t *n) {
    struct buf b = { 0 };
    puts_(&b, "<!DOCTYPE html>");
    ser(&b, 1, 1);
    *n = b.n;
    return b.p ? b.p : strdup("");
}

static int clone(int id, int deep) {
    int c = new_node(N[id].type, N[id].tag);
    if (!c) return 0;
    struct node *x = &N[id];
    if (x->text) N[c].text = sdup(x->text);
    N[c].started = x->started;
    for (int i = 0; i < x->na; i++) set_attr(c, x->an[i], x->av[i]);
    if (deep) for (int k = N[id].first; k; k = N[k].next) { int cc = clone(k, 1); if (cc) insert_before(c, cc, 0); }
    return c;
}

static void mark_started(int id) {
    for (int c = N[id].first; c; c = N[c].next) if (N[c].type == 1) { if (!strcmp(N[c].tag, "script")) N[c].started = 1; else mark_started(c); }
}

static void clear_children(int id) { while (N[id].first) unlink_node(N[id].first); }

/* ---------- selectors and computed style: css.c walks this tree through the css_dom callbacks (elements only) ---------- */
static int cd_parent(int n) { int p = N[n].parent; return p && N[p].type == 1 ? p : 0; }
static int cd_prev(int n) { for (n = N[n].prev; n; n = N[n].prev) if (N[n].type == 1) return n; return 0; }
static int cd_next(int n) { for (n = N[n].next; n; n = N[n].next) if (N[n].type == 1) return n; return 0; }
static int cd_first(int n) { for (n = N[n].first; n; n = N[n].next) if (N[n].type == 1) return n; return 0; }
static const char *cd_tag(int n) { return N[n].tag; }
static const char *cd_attr(int n, const char *name) { return attr(n, name); }
static int cd_state(int n) {
    int st = 0;
    if (attr(n, "disabled")) st |= CS_DISABLED;
    if (!strcmp(N[n].tag, "a") && attr(n, "href")) st |= CS_LINK;
    if (N[n].has_chk ? N[n].chk : attr(n, "checked") != 0) st |= CS_CHECKED;
    return st;
}
static int cd_has_text(int n) { for (int c = N[n].first; c; c = N[c].next) if (N[c].type == 3 && N[c].text[0]) return 1; return 0; }
static const struct css_dom CDOM = { cd_parent, cd_prev, cd_next, cd_first, cd_tag, cd_attr, cd_state, cd_has_text };

static void qsa_walk(int id, const char *sel, JSValue arr, uint32_t *cnt, int first) {
    for (int c = N[id].first; c && !(first && *cnt); c = N[c].next) {
        if (N[c].type != 1) continue;
        if (css_match_node(sel, c) == 1) JS_SetPropertyUint32(ctx, arr, (*cnt)++, JS_NewInt32(ctx, c));
        qsa_walk(c, sel, arr, cnt, first);
    }
}

static int by_id(int id, const char *want) {
    for (int c = N[id].first; c; c = N[c].next) {
        if (N[c].type != 1) continue;
        const char *v = attr(c, "id");
        if (v && !strcmp(v, want)) return c;
        int r = by_id(c, want);
        if (r) return r;
    }
    return 0;
}

static int by_tag(int id, const char *tag) {
    for (int c = N[id].first; c; c = N[c].next) {
        if (N[c].type != 1) continue;
        if (!strcmp(N[c].tag, tag)) return c;
        int r = by_tag(c, tag);
        if (r) return r;
    }
    return 0;
}

/* ---------- live form state ---------- */
static int ctl_of(int id) {
    if (!live || !id) return -1;
    for (int i = 0; i < live->nct; i++) if (live->ct[i].node == id) return i;
    return -1;
}

static int options(int sel, int *ids, int max) {          /* <option> descendants of a <select>, in order */
    int n = 0;
    for (int c = N[sel].first; c && n < max; c = N[c].next) {
        if (N[c].type != 1) continue;
        if (!strcmp(N[c].tag, "option")) ids[n++] = c;
        else if (!strcmp(N[c].tag, "optgroup")) n += options(c, ids + n, max - n);
    }
    return n;
}

static int sel_index(int id) {
    int ci = ctl_of(id);
    if (ci >= 0) return fs_get(live, ci)->sel;
    if (N[id].has_val) return N[id].sel;
    int o[256], n = options(id, o, 256);
    for (int i = 0; i < n; i++) if (attr(o[i], "selected")) return i;
    return n ? 0 : -1;
}

static char *opt_value(int o) {
    const char *v = attr(o, "value");
    if (v) return sdup(v);
    struct buf b = { 0 };
    text_of(&b, o);
    return b.p ? b.p : sdup("");
}

static char *value_of(int id) {                          /* malloc'd */
    int ci = ctl_of(id);
    if (!strcmp(N[id].tag, "select")) {
        int o[256], n = options(id, o, 256), i = sel_index(id);
        return i >= 0 && i < n ? opt_value(o[i]) : sdup("");
    }
    if (ci >= 0) {
        int k = live->ct[ci].kind;
        if (k == C_TEXT || k == C_PASS || k == C_AREA || k == C_HIDDEN) { struct fstate *s = fs_get(live, ci); return sdup(s->val); }
    }
    if (N[id].has_val) return sdup(N[id].val);
    if (!strcmp(N[id].tag, "textarea")) { struct buf b = { 0 }; text_of(&b, id); return b.p ? b.p : sdup(""); }
    const char *v = attr(id, "value"), *ty = attr(id, "type");
    if (!v && ty && (!strcasecmp(ty, "checkbox") || !strcasecmp(ty, "radio"))) v = "on";
    return sdup(v);
}

void js_sync(void) {
    if (!live) return;
    for (int i = 0; i < live->nct; i++) {
        const struct ctl *c = &live->ct[i];
        struct fstate *s = fs_ord(c->ord);
        if (!c->node || c->node >= nn || !s || !s->init) continue;
        struct node *x = &N[c->node];
        if (c->kind == C_CHECK || c->kind == C_RADIO) { x->has_chk = 1; x->chk = s->on; }
        else if (c->kind == C_SELECT) { x->has_val = 1; x->sel = s->sel; }
        else if (c->kind != C_BTN) { free(x->val); x->val = sdup(s->val); x->has_val = 1; }
    }
}

void js_attach(struct doc *d) { live = d; }

/* ---------- natives ---------- */
#define ARGID(i) nid(argv[i])
static int nid(JSValueConst v) { int32_t id = 0; JS_ToInt32(ctx, &id, v); return id > 0 && id < nn ? id : 0; }
static JSValue str(const char *s) { return JS_NewString(ctx, s ? s : ""); }
static JSValue take(char *s) { JSValue v = str(s); free(s); return v; }
#define NATIVE(name) static JSValue name(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
#define CSTR(var, i) const char *var = JS_ToCString(c, argv[i]); if (!var) return JS_EXCEPTION

NATIVE(n_type) { int id = ARGID(0); return JS_NewInt32(c, id ? N[id].type : 0); }
NATIVE(n_tag) { int id = ARGID(0); return str(id ? (N[id].type == 3 ? "#text" : N[id].type == 8 ? "#comment" : N[id].type == 9 ? "#document" : N[id].type == 11 ? "#document-fragment" : N[id].tag) : ""); }
NATIVE(n_parent) { int id = ARGID(0); return JS_NewInt32(c, id ? N[id].parent : 0); }
NATIVE(n_first) { int id = ARGID(0); return JS_NewInt32(c, id ? N[id].first : 0); }
NATIVE(n_last) { int id = ARGID(0); return JS_NewInt32(c, id ? N[id].last : 0); }
NATIVE(n_next) { int id = ARGID(0); return JS_NewInt32(c, id ? N[id].next : 0); }
NATIVE(n_prev) { int id = ARGID(0); return JS_NewInt32(c, id ? N[id].prev : 0); }

NATIVE(n_get_attr) {
    int id = ARGID(0);
    CSTR(n, 1);
    const char *v = id ? attr(id, n) : 0;
    JS_FreeCString(c, n);
    return v ? str(v) : JS_NULL;
}
NATIVE(n_set_attr) {
    int id = ARGID(0);
    if (!id || N[id].type != 1) return JS_UNDEFINED;
    CSTR(n, 1);
    const char *v = JS_ToCString(c, argv[2]);
    if (v && set_attr(id, n, v)) touch(id);
    JS_FreeCString(c, n); JS_FreeCString(c, v);
    return JS_UNDEFINED;
}
NATIVE(n_del_attr) {
    int id = ARGID(0);
    CSTR(n, 1);
    if (id && del_attr(id, n)) touch(id);
    JS_FreeCString(c, n);
    return JS_UNDEFINED;
}
NATIVE(n_attrs) {
    int id = ARGID(0);
    JSValue a = JS_NewArray(c);
    for (int i = 0; id && i < N[id].na; i++) JS_SetPropertyUint32(c, a, i, str(N[id].an[i]));
    return a;
}
NATIVE(n_text) {
    int id = ARGID(0);
    if (!id) return str("");
    if (N[id].type == 8) return str(N[id].text);
    struct buf b = { 0 };
    text_of(&b, id);
    return b.p ? take(b.p) : str("");
}
NATIVE(n_set_text) {
    int id = ARGID(0);
    if (!id) return JS_UNDEFINED;
    CSTR(s, 1);
    int t1 = N[id].first && N[id].first == N[id].last && N[N[id].first].type == 3 && *s ? N[id].first : 0;
    if (N[id].type == 3 || N[id].type == 8 || t1) {                           /* reuse the text node: a ticking clock mustn't eat node ids */
        int t = t1 ? t1 : id;
        if (strcmp(N[t].text, s)) { free(N[t].text); N[t].text = sdup(s); touch(t); }
    } else {
        clear_children(id);
        int t = *s ? new_text(s, strlen(s)) : 0;
        if (t) insert_before(id, t, 0);
        touch(id);
    }
    JS_FreeCString(c, s);
    return JS_UNDEFINED;
}
NATIVE(n_html) {
    int id = ARGID(0), outer = JS_ToBool(c, argv[1]);
    if (!id) return str("");
    struct buf b = { 0 };
    if (outer || N[id].type != 1) ser(&b, id, 0);
    else for (int k = N[id].first; k; k = N[k].next) ser(&b, k, 0);
    return b.p ? take(b.p) : str("");
}
NATIVE(n_set_html) {
    int id = ARGID(0);
    if (!id || N[id].type == 3 || N[id].type == 8) return JS_UNDEFINED;
    size_t l;
    const char *s = JS_ToCStringLen(c, &l, argv[1]);
    if (!s) return JS_EXCEPTION;
    clear_children(id);
    int body = P.body;
    parse_html(id, s, l, 1);
    P.body = body;
    mark_started(id);                                      /* scripts from innerHTML never run */
    JS_FreeCString(c, s);
    touch(id);
    return JS_UNDEFINED;
}
NATIVE(n_create) {
    CSTR(t, 0);
    int id = new_node(1, t);
    JS_FreeCString(c, t);
    return id ? JS_NewInt32(c, id) : JS_ThrowInternalError(c, "too many DOM nodes");
}
NATIVE(n_create_text) {
    CSTR(s, 0);
    int id = new_text(s, strlen(s));
    JS_FreeCString(c, s);
    return id ? JS_NewInt32(c, id) : JS_ThrowInternalError(c, "too many DOM nodes");
}
NATIVE(n_create_frag) { int id = new_node(11, 0); return id ? JS_NewInt32(c, id) : JS_ThrowInternalError(c, "too many DOM nodes"); }
NATIVE(n_insert) {
    int p = ARGID(0), ch = ARGID(1), ref = ARGID(2);
    if (!p || !ch || N[p].type == 3 || N[p].type == 8) return JS_ThrowTypeError(c, "HierarchyRequestError: this node can't have children");
    for (int a = p; a; a = N[a].parent) if (a == ch) return JS_ThrowTypeError(c, "HierarchyRequestError: the new child contains the parent");
    if (ref && N[ref].parent != p) return JS_ThrowTypeError(c, "NotFoundError: the reference node is not a child of this node");
    if (ch == 1 || ch == ref) return JS_UNDEFINED;
    touch(ch);                                             /* leaving the document dirties it too */
    int live_p = connected(p);
    if (N[ch].type == 11) while (N[ch].first) { int k = N[ch].first; insert_before(p, k, ref); if (live_p) scan_scripts(k, 2); }
    else { insert_before(p, ch, ref); if (live_p) scan_scripts(ch, 2); }
    touch(p);
    return JS_UNDEFINED;
}
NATIVE(n_remove) {
    int p = ARGID(0), ch = ARGID(1);
    if (!ch || N[ch].parent != p) return JS_ThrowTypeError(c, "NotFoundError: not a child of this node");
    touch(ch);
    unlink_node(ch);
    return JS_UNDEFINED;
}
NATIVE(n_clone) { int id = ARGID(0); int r = id ? clone(id, JS_ToBool(c, argv[1])) : 0; return JS_NewInt32(c, r); }
NATIVE(n_qsa) {
    int root = ARGID(0), first = JS_ToBool(c, argv[2]);
    CSTR(sel, 1);
    if (css_match_node(sel, 2) < 0) { JSValue e = JS_ThrowSyntaxError(c, "'%s' is not a valid selector", sel); JS_FreeCString(c, sel); return e; }
    JSValue a = JS_NewArray(c);
    uint32_t cnt = 0;
    if (root) qsa_walk(root, sel, a, &cnt, first);
    JS_FreeCString(c, sel);
    return a;
}
NATIVE(n_matches) {
    int id = ARGID(0);
    CSTR(sel, 1);
    int r = id && N[id].type == 1 ? css_match_node(sel, id) : 0;
    JS_FreeCString(c, sel);
    return r < 0 ? JS_ThrowSyntaxError(c, "unsupported selector") : JS_NewBool(c, r);
}
NATIVE(n_by_id) { CSTR(s, 0); int r = by_id(1, s); JS_FreeCString(c, s); return JS_NewInt32(c, r); }
NATIVE(n_value) { int id = ARGID(0); return id ? take(value_of(id)) : str(""); }
NATIVE(n_set_value) {
    int id = ARGID(0);
    if (!id) return JS_UNDEFINED;
    CSTR(v, 1);
    struct node *x = &N[id];
    int ci = ctl_of(id);
    if (!strcmp(x->tag, "select")) {
        int o[256], n = options(id, o, 256), k = -1;
        for (int i = 0; i < n && k < 0; i++) { char *ov = opt_value(o[i]); if (!strcmp(ov, v)) k = i; free(ov); }
        x->has_val = 1; x->sel = k;
        if (ci >= 0 && k >= 0) fs_get(live, ci)->sel = k;
    } else {
        free(x->val); x->val = sdup(v); x->has_val = 1;
        if (ci >= 0 && live->ct[ci].kind != C_BTN && live->ct[ci].kind != C_CHECK && live->ct[ci].kind != C_RADIO) fs_set(fs_get(live, ci), v);
        else touch(id);                                    /* not on screen yet, or a button label */
    }
    JS_FreeCString(c, v);
    return JS_UNDEFINED;
}
NATIVE(n_checked) {
    int id = ARGID(0), ci = ctl_of(id);
    if (!id) return JS_FALSE;
    if (ci >= 0) return JS_NewBool(c, fs_get(live, ci)->on);
    return JS_NewBool(c, N[id].has_chk ? N[id].chk : attr(id, "checked") != 0);
}
NATIVE(n_set_checked) {
    int id = ARGID(0), on = JS_ToBool(c, argv[1]), ci = ctl_of(id);
    if (!id) return JS_UNDEFINED;
    N[id].has_chk = 1; N[id].chk = on;
    if (ci >= 0) {
        if (live->ct[ci].kind == C_RADIO && on) ctl_toggle(live, ci);
        else fs_get(live, ci)->on = on;
    } else touch(id);
    return JS_UNDEFINED;
}
NATIVE(n_sel_index) { int id = ARGID(0); return JS_NewInt32(c, id ? sel_index(id) : -1); }
NATIVE(n_set_sel_index) {
    int id = ARGID(0), ci = ctl_of(id);
    int32_t k = -1;
    JS_ToInt32(c, &k, argv[1]);
    if (!id) return JS_UNDEFINED;
    N[id].has_val = 1; N[id].sel = k;
    if (ci >= 0 && k >= 0 && k < live->ct[ci].no) fs_get(live, ci)->sel = k;
    else touch(id);
    return JS_UNDEFINED;
}
NATIVE(n_focus) { focus_req = ARGID(0); return JS_UNDEFINED; }
NATIVE(n_url) { return str(page_url); }
NATIVE(n_resolve) {
    char out[URL_MAX];
    CSTR(u, 0);
    const char *bs = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToCString(c, argv[1]) : 0;
    url_resolve(bs ? bs : page_url, u, out, sizeof out);
    JS_FreeCString(c, bs);
    JS_FreeCString(c, u);
    return str(out);
}
NATIVE(n_navigate) {
    CSTR(u, 0);
    url_resolve(page_url, u, nav, sizeof nav);
    JS_FreeCString(c, u);
    return JS_UNDEFINED;
}
NATIVE(n_alert) {
    CSTR(s, 0);
    free(alert_text);
    alert_text = sdup(s);
    JS_FreeCString(c, s);
    return JS_UNDEFINED;
}
NATIVE(n_log) {
    int32_t k = 0;
    JS_ToInt32(c, &k, argv[0]);
    CSTR(s, 1);
    jlog(k, "%s", s);
    JS_FreeCString(c, s);
    return JS_UNDEFINED;
}
NATIVE(n_timer) {
    double ms = 0;
    JS_ToFloat64(c, &ms, argv[0]);
    int rep = JS_ToBool(c, argv[1]);
    if (!(ms >= 0)) ms = 0;
    if (ms > 1e9) ms = 1e9;
    if (rep && ms < 10) ms = 10;
    if (nt == MAXT) return JS_ThrowRangeError(c, "too many timers");
    T[nt].id = next_tid++; T[nt].every = rep ? (int)ms : 0; T[nt].due = uptime_ms() + (uint32_t)ms;
    return JS_NewInt32(c, T[nt++].id);
}
NATIVE(n_clear) {
    int32_t id = 0;
    JS_ToInt32(c, &id, argv[0]);
    for (int i = 0; i < nt; i++) if (T[i].id == id) { T[i] = T[--nt]; break; }
    return JS_UNDEFINED;
}
NATIVE(n_now) { return JS_NewFloat64(c, uptime_ms()); }

static int fetch_queue(const char *url, int script, const char *blob, int has_blob);
NATIVE(n_fetch) {
    CSTR(u, 0);
    char abs[URL_MAX];
    url_resolve(page_url, u, abs, sizeof abs);
    url_strip_frag(abs);
    JS_FreeCString(c, u);
    /* the request travels as one blob "METHOD\0headers\0body" (NULL = a plain GET) */
    const char *body = JS_IsNull(argv[1]) || JS_IsUndefined(argv[1]) ? 0 : JS_ToCString(c, argv[1]);
    const char *meth = argc > 2 && !JS_IsUndefined(argv[2]) ? JS_ToCString(c, argv[2]) : 0;
    const char *hd = argc > 3 && !JS_IsUndefined(argv[3]) ? JS_ToCString(c, argv[3]) : 0;
    char *blob = 0;
    if (body || hd || (meth && strcmp(meth, "GET"))) {
        const char *m = meth ? meth : "POST";
        size_t lm = strlen(m), lh = hd ? strlen(hd) : 0, lb = body ? strlen(body) : 0;
        blob = malloc(lm + lh + lb + 3);
        if (blob) { memcpy(blob, m, lm + 1); memcpy(blob + lm + 1, hd ? hd : "", lh + 1); memcpy(blob + lm + lh + 2, body ? body : "", lb + 1); }
    }
    int id = fetch_queue(abs, 0, blob, blob ? 1 : 0);
    free(blob);
    JS_FreeCString(c, body); JS_FreeCString(c, meth); JS_FreeCString(c, hd);
    return id < 0 ? JS_ThrowRangeError(c, "too many fetches in flight") : JS_NewInt32(c, id);
}

NATIVE(n_create_comment) {
    CSTR(s, 0);
    int id = new_node(8, 0);
    if (id) N[id].text = sdup(s);
    JS_FreeCString(c, s);
    return id ? JS_NewInt32(c, id) : JS_ThrowInternalError(c, "too many DOM nodes");
}
/* document.write: while the parser "is" at a script, the text goes in right after it (and any <script> in it runs next);
 * afterwards it is appended to <body> (ponytail: no real document.open() reparse) */
NATIVE(n_docwrite) {
    size_t l;
    const char *src = JS_ToCStringLen(c, &l, argv[0]);
    if (!src) return JS_EXCEPTION;
    int frag = new_node(11, 0);
    if (frag) {
        int body = P.body;
        parse_html(frag, src, l, 1);
        P.body = body;
        int at = cur_script && !loaded && N[cur_script].parent ? cur_script : 0;
        int p = at ? N[at].parent : by_tag(1, "body"), ref = at ? N[at].next : 0;
        if (!p) p = 2;
        while (N[frag].first) { int k = N[frag].first; insert_before(p, k, ref); scan_scripts(k, 1); }
        touch(p);
    }
    JS_FreeCString(c, src);
    return JS_UNDEFINED;
}
NATIVE(n_hit) { int32_t x = 0, y = 0; JS_ToInt32(c, &x, argv[0]); JS_ToInt32(c, &y, argv[1]); return JS_NewInt32(c, live ? doc_node_at(live, x, y + vscroll) : 0); }
NATIVE(n_submit) { submit_req = ARGID(0); return JS_UNDEFINED; }
NATIVE(n_sync_get) {
    CSTR(u, 0);
    char abs[URL_MAX];
    url_resolve(page_url, u, abs, sizeof abs);
    url_strip_frag(abs);
    JS_FreeCString(c, u);
    size_t l = 0;
    char *b = sync_get(abs, &l);
    if (!b) return JS_NULL;
    JSValue v = JS_NewStringLen(c, b, l);
    free(b);
    return v;
}
NATIVE(n_ua) { return str(BROWSER_UA); }
NATIVE(n_cookie_get) { char b[4096]; cookie_get(page_url, b, sizeof b); return str(b); }
NATIVE(n_cookie_set) { CSTR(v, 0); cookie_set(page_url, v); JS_FreeCString(c, v); return JS_UNDEFINED; }
NATIVE(n_cur_script) { return JS_NewInt32(c, cur_script); }
NATIVE(n_ready) { return JS_NewInt32(c, rstate); }
NATIVE(n_set_url) {                                        /* pushState / replaceState / #hash: the page URL changes, no load */
    CSTR(u, 0);
    char out[URL_MAX];
    url_resolve(page_url, u, out, sizeof out);
    JS_FreeCString(c, u);
    strlcpy(page_url, out, sizeof page_url);
    url_changed = 1;
    const char *h = strchr(out, '#');
    if (JS_ToBool(c, argv[1])) strlcpy(anchor_req, h ? h + 1 : "", sizeof anchor_req);
    return JS_UNDEFINED;
}
NATIVE(n_scroll_to) { double y = 0; JS_ToFloat64(c, &y, argv[1]); scroll_req_y = y < 0 ? 0 : (int)y; return JS_UNDEFINED; }
NATIVE(n_view) { JSValue a = JS_NewArray(c); JS_SetPropertyUint32(c, a, 0, JS_NewInt32(c, vscroll)); JS_SetPropertyUint32(c, a, 1, JS_NewInt32(c, vw)); JS_SetPropertyUint32(c, a, 2, JS_NewInt32(c, vh)); return a; }
/* layout publishes border boxes here after each layout (js_set_rect); until it does (or for a node it skipped) a node's
 * box is the union of the display items inside it: text extents, and an empty element has none */
static int (*RC)[4], nrc, rc_set;                   /* RC[node] = {x, y, w, h}; w < 0 = unset */
void js_set_rect(int node, int x, int y, int w, int h) {
    if (node <= 0 || node >= nn) return;
    if (node >= nrc) {
        int n2 = nn + 256;
        int (*r)[4] = realloc(RC, n2 * sizeof *RC);
        if (!r) return;
        for (int i = nrc; i < n2; i++) r[i][2] = -1;
        RC = r; nrc = n2;
    }
    RC[node][0] = x; RC[node][1] = y; RC[node][2] = w; RC[node][3] = h; rc_set = 1;
}
NATIVE(n_rect) {
    int id = ARGID(0), x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    JSValue a = JS_NewArray(c);
    if (id && id < nrc && RC[id][2] >= 0) {
        for (int i = 0; i < 4; i++) JS_SetPropertyUint32(c, a, i, JS_NewInt32(c, RC[id][i]));
        return a;
    }
    if (id && live && N[id].type != 9)
        for (int i = 0; i < live->nit; i++) {
            const struct item *it = &live->it[i];
            int a_ = it->node;
            while (a_ && a_ != id) a_ = N[a_].parent;
            if (!a_ || !it->node) continue;
            if (it->x < x0) x0 = it->x;
            if (it->y < y0) y0 = it->y;
            if (it->x + it->w > x1) x1 = it->x + it->w;
            if (it->y + it->h > y1) y1 = it->y + it->h;
        }
    if (id && live && N[id].type == 9) { x0 = y0 = 0; x1 = vw; y1 = live->height; }
    if (x1 < 0) x0 = y0 = x1 = y1 = 0;
    int v[4] = { x0, y0, x1 - x0, y1 - y0 };
    for (int i = 0; i < 4; i++) JS_SetPropertyUint32(c, a, i, JS_NewInt32(c, v[i]));
    return a;
}
NATIVE(n_media) { CSTR(q, 0); css_viewport(vw); int r = css_media(q); JS_FreeCString(c, q); return JS_NewBool(c, r); }
/* computed style of an element, as an object of CSS strings (css_compute down the ancestor chain: UA sheet, author sheets, style="") */
static void put_len(JSContext *c, JSValue o, const char *name, const struct cprops *p, int prop) {
    char b[24];
    if (p->k[prop] == K_AUTO) strlcpy(b, "auto", sizeof b);
    else snprintf(b, sizeof b, "%dpx", css_len(p, prop, 0));
    JS_SetPropertyStr(c, o, name, str(b));
}
static void put_col(JSContext *c, JSValue o, const char *name, uint32_t col) {
    char b[40];
    int a = col >> 24;
    if (a == 255) snprintf(b, sizeof b, "rgb(%d, %d, %d)", (col >> 16) & 255, (col >> 8) & 255, col & 255);
    else snprintf(b, sizeof b, "rgba(%d, %d, %d, %d.%02d)", (col >> 16) & 255, (col >> 8) & 255, col & 255, a / 255, a * 100 / 255 % 100);
    JS_SetPropertyStr(c, o, name, str(b));
}
NATIVE(n_computed) {
    int id = ARGID(0), chain[128], n = 0;
    JSValue o = JS_NewObject(c);
    if (!id || N[id].type != 1) return o;
    for (int a = id; a && n < 128; a = cd_parent(a)) chain[n++] = a;
    css_dom_set(&CDOM);
    css_base(page_url);
    static struct cprops cp[2];
    int cur = 0;
    for (int i = n - 1; i >= 0; i--) { css_compute(chain[i], PE_NONE, i == n - 1 ? 0 : &cp[cur ^ 1], &cp[cur]); if (i) cur ^= 1; }
    const struct cprops *p = &cp[cur];
    static const char *const DN[] = { "", "none", "block", "inline", "inline-block", "flex", "inline-flex", "grid", "inline-grid", "table", "inline-table", "table-row",
        "table-cell", "table-row-group", "table-header-group", "table-footer-group", "table-column", "table-column-group", "table-caption", "list-item", "contents" };
    int dv = p->v[P_DISPLAY];
    JS_SetPropertyStr(c, o, "display", str(dv > 0 && dv < (int)(sizeof DN / sizeof *DN) ? DN[dv] : "inline"));
    static const char *const POS[] = { "static", "relative", "absolute", "fixed", "sticky" };
    JS_SetPropertyStr(c, o, "position", str(p->v[P_POSITION] >= 0 && p->v[P_POSITION] < 5 ? POS[p->v[P_POSITION]] : "static"));
    JS_SetPropertyStr(c, o, "float", str(p->v[P_FLOAT] == FLT_LEFT ? "left" : p->v[P_FLOAT] == FLT_RIGHT ? "right" : "none"));
    JS_SetPropertyStr(c, o, "visibility", str(p->v[P_VISIB] ? "hidden" : "visible"));
    static const char *const OVN[] = { "visible", "hidden", "scroll", "auto", "clip" };
    JS_SetPropertyStr(c, o, "overflow", str(p->v[P_OVX] >= 0 && p->v[P_OVX] < 5 ? OVN[p->v[P_OVX]] : "visible"));
    char b[24];
    snprintf(b, sizeof b, "%d.%02d", p->v[P_OPACITY] / 255, p->v[P_OPACITY] * 100 / 255 % 100);
    JS_SetPropertyStr(c, o, "opacity", str(p->v[P_OPACITY] >= 255 ? "1" : b));
    put_col(c, o, "color", p->v[P_COLOR]);
    put_col(c, o, "background-color", p->v[P_BG]);
    JS_SetPropertyStr(c, o, "text-align", str(p->v[P_ALIGN] == TA_CENTER ? "center" : p->v[P_ALIGN] == TA_RIGHT ? "right" : p->v[P_ALIGN] == TA_JUSTIFY ? "justify" : "left"));
    snprintf(b, sizeof b, "%d", p->v[P_FWEIGHT] ? p->v[P_FWEIGHT] : p->v[P_BOLD] ? 700 : 400);
    JS_SetPropertyStr(c, o, "font-weight", str(b));
    snprintf(b, sizeof b, "%dpx", css_len(p, P_FSIZE, 0));
    JS_SetPropertyStr(c, o, "font-size", str(b));
    static const struct { int p; const char *n; } LN[] = { { P_MT, "margin-top" }, { P_MR, "margin-right" }, { P_MB, "margin-bottom" }, { P_ML, "margin-left" },
        { P_PT, "padding-top" }, { P_PR, "padding-right" }, { P_PB, "padding-bottom" }, { P_PL, "padding-left" }, { P_BWT, "border-top-width" },
        { P_BWR, "border-right-width" }, { P_BWB, "border-bottom-width" }, { P_BWL, "border-left-width" }, { P_WIDTH, "width" }, { P_HEIGHT, "height" },
        { P_TOP, "top" }, { P_LEFT, "left" }, { P_RIGHT, "right" }, { P_BOTTOM, "bottom" } };
    for (unsigned i = 0; i < sizeof LN / sizeof *LN; i++) put_len(c, o, LN[i].n, p, LN[i].p);
    if (p->k[P_ZINDEX] == K_AUTO) JS_SetPropertyStr(c, o, "z-index", str("auto"));
    else { snprintf(b, sizeof b, "%d", p->v[P_ZINDEX]); JS_SetPropertyStr(c, o, "z-index", str(b)); }
    return o;
}
/* small per-origin files under ~/.config/browser (localStorage); name is sanitised */
static void store_path(const char *name, char *out, int cap, int mk) {
    const char *h = getenv("HOME");
    char dir[200], safe[100];
    int k = 0;
    for (; name[k] && k < 99; k++) safe[k] = isalnum((unsigned char)name[k]) || name[k] == '.' || name[k] == '-' ? name[k] : '_';
    safe[k] = 0;
    snprintf(dir, sizeof dir, "%s/.config", h && *h ? h : "/home");
    if (mk) { mkdir(dir, 0755); strlcat(dir, "/browser", sizeof dir); mkdir(dir, 0755); } else strlcat(dir, "/browser", sizeof dir);
    snprintf(out, cap, "%s/%s", dir, safe);
}
NATIVE(n_load) {
    CSTR(n, 0);
    char path[300];
    store_path(n, path, sizeof path, 0);
    JS_FreeCString(c, n);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0) return JS_NULL;
    char *b = fstat(fd, &st) == 0 && st.st_size < (1 << 20) ? malloc(st.st_size + 1) : 0;
    ssize_t got = b ? read(fd, b, st.st_size) : -1;
    close(fd);
    if (!b) return JS_NULL;
    if (got < 0) got = 0;
    b[got] = 0;
    return take(b);
}
NATIVE(n_save) {
    CSTR(n, 0);
    CSTR(v, 1);
    char path[300];
    store_path(n, path, sizeof path, 1);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, v, strlen(v)); close(fd); }
    JS_FreeCString(c, n); JS_FreeCString(c, v);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry NATIVES[] = {
    JS_CFUNC_DEF("type", 1, n_type), JS_CFUNC_DEF("tag", 1, n_tag), JS_CFUNC_DEF("parent", 1, n_parent),
    JS_CFUNC_DEF("first", 1, n_first), JS_CFUNC_DEF("last", 1, n_last), JS_CFUNC_DEF("next", 1, n_next),
    JS_CFUNC_DEF("prev", 1, n_prev), JS_CFUNC_DEF("getAttr", 2, n_get_attr), JS_CFUNC_DEF("setAttr", 3, n_set_attr),
    JS_CFUNC_DEF("delAttr", 2, n_del_attr), JS_CFUNC_DEF("attrs", 1, n_attrs), JS_CFUNC_DEF("text", 1, n_text),
    JS_CFUNC_DEF("setText", 2, n_set_text), JS_CFUNC_DEF("html", 2, n_html), JS_CFUNC_DEF("setHTML", 2, n_set_html),
    JS_CFUNC_DEF("create", 1, n_create), JS_CFUNC_DEF("createText", 1, n_create_text), JS_CFUNC_DEF("createFrag", 0, n_create_frag),
    JS_CFUNC_DEF("insert", 3, n_insert), JS_CFUNC_DEF("remove", 2, n_remove), JS_CFUNC_DEF("clone", 2, n_clone),
    JS_CFUNC_DEF("qsa", 3, n_qsa), JS_CFUNC_DEF("matches", 2, n_matches), JS_CFUNC_DEF("byId", 1, n_by_id),
    JS_CFUNC_DEF("value", 1, n_value), JS_CFUNC_DEF("setValue", 2, n_set_value), JS_CFUNC_DEF("checked", 1, n_checked),
    JS_CFUNC_DEF("setChecked", 2, n_set_checked), JS_CFUNC_DEF("selIndex", 1, n_sel_index), JS_CFUNC_DEF("setSelIndex", 2, n_set_sel_index),
    JS_CFUNC_DEF("focus", 1, n_focus), JS_CFUNC_DEF("url", 0, n_url), JS_CFUNC_DEF("navigate", 1, n_navigate), JS_CFUNC_DEF("resolve", 1, n_resolve),
    JS_CFUNC_DEF("alert", 1, n_alert), JS_CFUNC_DEF("log", 2, n_log), JS_CFUNC_DEF("timer", 2, n_timer),
    JS_CFUNC_DEF("clear", 1, n_clear), JS_CFUNC_DEF("now", 0, n_now), JS_CFUNC_DEF("fetch", 4, n_fetch),
    JS_CFUNC_DEF("createComment", 1, n_create_comment), JS_CFUNC_DEF("docwrite", 1, n_docwrite), JS_CFUNC_DEF("curScript", 0, n_cur_script),
    JS_CFUNC_DEF("ready", 0, n_ready), JS_CFUNC_DEF("setUrl", 2, n_set_url), JS_CFUNC_DEF("scrollTo", 2, n_scroll_to),
    JS_CFUNC_DEF("view", 0, n_view), JS_CFUNC_DEF("rect", 1, n_rect), JS_CFUNC_DEF("media", 1, n_media),
    JS_CFUNC_DEF("computed", 1, n_computed), JS_CFUNC_DEF("hit", 2, n_hit), JS_CFUNC_DEF("cookieGet", 0, n_cookie_get), JS_CFUNC_DEF("cookieSet", 1, n_cookie_set), JS_CFUNC_DEF("ua", 0, n_ua), JS_CFUNC_DEF("submit", 1, n_submit), JS_CFUNC_DEF("syncGet", 1, n_sync_get), JS_CFUNC_DEF("load", 1, n_load), JS_CFUNC_DEF("save", 2, n_save),
};

/* ---------- running JS ---------- */
static int on_interrupt(JSRuntime *r, void *op) {
    if (killed || uptime_ms() > deadline) { killed = 1; return 1; }
    return 0;
}

static int depth;                                          /* nested entries (a script inserted by a script) share the outer budget */
static void enter(void) { if (depth++ == 0) { deadline = uptime_ms() + RUN_MS; if (!dead) killed = 0; } }

static void report(JSContext *c) {
    JSValue e = JS_GetException(c);
    if (killed) {
        if (++strikes >= 3) { jlog(2, "scripts on this page ran too long three times and are now off"); dead = 1; nt = 0; }
        else jlog(2, "a script ran for more than %d s and was stopped", RUN_MS / 1000);
        if (JS_IsError(c, e)) {                              /* where it was when the budget ran out */
            JSValue st = JS_GetPropertyStr(c, e, "stack");
            const char *sk = JS_IsUndefined(st) ? 0 : JS_ToCString(c, st);
            for (const char *p = sk; p && *p; ) { const char *nl = strchr(p, '\n'); int l = nl ? (int)(nl - p) : (int)strlen(p); if (l > 0) jlog(2, "%.*s", l, p); p += l + (nl != 0); }
            JS_FreeCString(c, sk); JS_FreeValue(c, st);
        }
        JS_FreeValue(c, e);
        return;
    }
    const char *m = JS_ToCString(c, e);
    jlog(2, "Uncaught %s", m ? m : "exception");
    JS_FreeCString(c, m);
    if (JS_IsError(c, e)) {
        JSValue st = JS_GetPropertyStr(c, e, "stack");
        const char *s = JS_IsUndefined(st) ? 0 : JS_ToCString(c, st);
        for (const char *p = s; p && *p; ) {             /* the stack's lines, one console line each */
            const char *nl = strchr(p, '\n');
            int l = nl ? (int)(nl - p) : (int)strlen(p);
            if (l > 0) jlog(2, "%.*s", l, p);
            p += l + (nl != 0);
        }
        JS_FreeCString(c, s);
        JS_FreeValue(c, st);
    }
    JS_FreeValue(c, e);
}

static void flush_rejections(void);
static void leave(JSValue r) {
    if (JS_IsException(r)) report(ctx);
    JS_FreeValue(ctx, r);
    JSContext *c;
    if (--depth > 0) return;
    depth = 0;
    for (int k = 0; !dead && k < 100000; k++) {          /* promise jobs, same budget */
        int ret = JS_ExecutePendingJob(rt, &c);
        if (ret <= 0) { if (ret < 0) report(c); if (ret == 0) break; }
    }
    flush_rejections();
}

/* a rejection nobody handled by the end of the microtask checkpoint (a handler attached right after is fine, as in browsers) */
static struct { JSValue p, reason; } RJ[16];
static int nrj;
static void on_reject(JSContext *c, JSValueConst p, JSValueConst reason, JS_BOOL handled, void *op) {
    if (killed) return;
    if (handled) {
        for (int i = 0; i < nrj; i++)
            if (JS_VALUE_GET_PTR(RJ[i].p) == JS_VALUE_GET_PTR(p)) { JS_FreeValue(c, RJ[i].p); JS_FreeValue(c, RJ[i].reason); RJ[i] = RJ[--nrj]; break; }
        return;
    }
    if (nrj < 16) { RJ[nrj].p = JS_DupValue(c, p); RJ[nrj].reason = JS_DupValue(c, reason); nrj++; }
}
static void flush_rejections(void) {
    while (nrj) {
        nrj--;
        JSValue rs = RJ[nrj].reason;
        const char *m = JS_ToCString(ctx, rs);
        JSValue st = JS_IsError(ctx, rs) ? JS_GetPropertyStr(ctx, rs, "stack") : JS_UNDEFINED;
        const char *sk = JS_IsUndefined(st) ? 0 : JS_ToCString(ctx, st);
        jlog(2, "Uncaught (in promise) %s%s%.60s", m ? m : "", sk ? " " : "", sk ? sk : "");
        JS_FreeCString(ctx, m); JS_FreeCString(ctx, sk); JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, RJ[nrj].p); JS_FreeValue(ctx, rs);
    }
}

static void call(JSValue fn, int argc, JSValue *argv) {
    enter();
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    leave(r);
}

static void run_script(const char *src, size_t n, const char *name, int module) {
    enter();
    leave(JS_Eval(ctx, src, n, name, module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL));
}

int js_event(int node, const char *type, const struct gui_event *k, int x, int y) {
    if (!active || dead || !ctx) return 0;
    JSValue o = JS_NewObject(ctx);
    if (k) {
        static const char *const NAMES[] = { "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home", "End", "PageUp", "PageDown", "Insert" };
        static const int CODES[] = { 38, 40, 37, 39, 36, 35, 33, 34, 45 };
        char ch[8] = "";
        const char *key = ch;
        int code = 0;
        if (k->key == KEY_ENTER) { key = "Enter"; code = 13; }
        else if (k->key == KEY_ESC) { key = "Escape"; code = 27; }
        else if (k->key == KEY_BACKSPACE) { key = "Backspace"; code = 8; }
        else if (k->key == KEY_TAB) { key = "Tab"; code = 9; }
        else if (k->key == KEY_DELETE) { key = "Delete"; code = 46; }
        else if (k->key >= KEY_UP && k->key <= KEY_INSERT) { key = NAMES[k->key - KEY_UP]; code = CODES[k->key - KEY_UP]; }
        else if (k->key >= KEY_F1 && k->key <= KEY_F12) { snprintf(ch, sizeof ch, "F%d", k->key - KEY_F1 + 1); code = 112 + k->key - KEY_F1; }
        else if (k->ch >= 32 && k->ch < 127) { ch[0] = k->ch; ch[1] = 0; code = toupper(k->ch); }
        else if (k->key > 0 && k->key < 127) { ch[0] = k->key; ch[1] = 0; code = toupper(k->key); }
        char kcode[16];
        if (key[0] && !key[1] && isalpha((unsigned char)key[0])) snprintf(kcode, sizeof kcode, "Key%c", toupper((unsigned char)key[0]));
        else if (key[0] && !key[1] && isdigit((unsigned char)key[0])) snprintf(kcode, sizeof kcode, "Digit%c", key[0]);
        else if (key[0] == ' ') strlcpy(kcode, "Space", sizeof kcode);
        else strlcpy(kcode, key, sizeof kcode);
        JS_SetPropertyStr(ctx, o, "code", str(kcode));
        JS_SetPropertyStr(ctx, o, "key", str(key));
        JS_SetPropertyStr(ctx, o, "keyCode", JS_NewInt32(ctx, code));
        JS_SetPropertyStr(ctx, o, "which", JS_NewInt32(ctx, code));
        JS_SetPropertyStr(ctx, o, "ctrlKey", JS_NewBool(ctx, k->mods & MOD_CTRL));
        JS_SetPropertyStr(ctx, o, "shiftKey", JS_NewBool(ctx, k->mods & MOD_SHIFT));
        JS_SetPropertyStr(ctx, o, "altKey", JS_NewBool(ctx, k->mods & MOD_ALT));
        JS_SetPropertyStr(ctx, o, "metaKey", JS_NewBool(ctx, k->mods & MOD_SUPER));
    }
    JS_SetPropertyStr(ctx, o, "clientX", JS_NewInt32(ctx, x));         /* x, y are document coordinates */
    JS_SetPropertyStr(ctx, o, "clientY", JS_NewInt32(ctx, y - vscroll));
    JS_SetPropertyStr(ctx, o, "pageX", JS_NewInt32(ctx, x));
    JS_SetPropertyStr(ctx, o, "pageY", JS_NewInt32(ctx, y));
    JSValue argv[3] = { JS_NewInt32(ctx, node), str(type), o };
    enter();
    JSValue r = JS_Call(ctx, fn_dispatch, JS_UNDEFINED, 3, argv);
    int prevented = !JS_IsException(r) && JS_ToBool(ctx, r) > 0;
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, argv[i]);
    leave(r);
    return prevented;
}

/* ---------- fetches ---------- */
static struct scr *scr_by_fid(int id) {
    for (int i = 0; i < nscr; i++) if (S[i].fid == id && S[i].state == 1) return &S[i];
    for (int i = 0; i < MAXDYN; i++) if (D[i].fid == id && D[i].state == 1) return &D[i];
    return 0;
}
static void deliver(int id, int script, int status, const char *text, const char *ctype, const char *body, size_t blen, const char *err, const char *url, const char *hdrs) {
    if (script) {
        struct scr *e = scr_by_fid(id);
        if (!e || e->state != 1) return;
        if (err || status >= 400) { jlog(2, "failed to load script %s: %s", url, err ? err : text); e->state = 3; return; }
        e->body = strndup(body ? body : "", blen);
        e->state = e->body ? 2 : 3;
        return;
    }
    if (dead) return;
    JSValue argv[8] = { JS_NewInt32(ctx, id), JS_NewInt32(ctx, status), str(text), str(ctype),
                        body ? JS_NewStringLen(ctx, body, blen) : str(""), err ? str(err) : JS_NULL, str(url), str(hdrs) };
    call(fn_fetch, 8, argv);
}

static int slot_start(struct jf *f, int id, const char *url, int script, char *post) {   /* takes post */
    memset(f, 0, sizeof *f);
    f->id = id; f->script = script; f->post = post;
    strlcpy(f->url, url, sizeof f->url);
    if (!url_is_web(url)) { f->used = 1; f->file = 1; return 0; }       /* read on the next pump */
    f->used = 1;
    const char *meth = post, *hdrs = post ? post + strlen(post) + 1 : 0, *body = hdrs ? hdrs + strlen(hdrs) + 1 : 0;
    int started = post ? fetch_start_req(&f->f, url, strcmp(meth, "GET") ? meth : 0, *hdrs ? hdrs : 0,
                                         strcmp(meth, "GET") && strcmp(meth, "HEAD") ? body : 0, strlen(body))
                       : fetch_start(&f->f, url, 0);
    if (started < 0) f->fail = "the network helper didn't start";
    return 0;
}

static int fetch_queue(const char *url, int script, const char *blob, int has_blob) {
    int id = next_fid++;
    char *pd = 0;
    if (has_blob) {
        size_t l1 = strlen(blob) + 1, l2 = strlen(blob + l1) + 1, l3 = strlen(blob + l1 + l2) + 1;
        pd = malloc(l1 + l2 + l3);
        if (pd) memcpy(pd, blob, l1 + l2 + l3);
    }
    for (int i = 0; i < MAXF; i++) if (!F[i].used) { slot_start(&F[i], id, url, script, pd); return id; }
    if (nfq == 64) { free(pd); return -1; }
    FQ[nfq].id = id; FQ[nfq].script = script; FQ[nfq].post = pd; strlcpy(FQ[nfq].url, url, sizeof FQ[0].url); nfq++;
    return id;
}

static void read_file(struct jf *f) {
    char path[PATH_MAX];
    url_file_path(f->url, path, sizeof path);
    int fd = open(path, O_RDONLY);
    struct stat st;
    char *b = 0;
    size_t got = 0;
    if (fd >= 0 && fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode) && (b = malloc(st.st_size + 1))) {
        ssize_t r;
        while (got < st.st_size && (r = read(fd, b + got, st.st_size - got)) > 0) got += r;
        b[got] = 0;
    }
    if (fd >= 0) close(fd);
    f->used = 0;
    if (b) deliver(f->id, f->script, 200, "OK", "", b, got, 0, f->url, "");
    else deliver(f->id, f->script, 0, "", "", 0, 0, "file not found", f->url, "");
    free(b);
}

static void pump_fetches(void) {
    for (int i = 0; i < MAXF && active; i++) {
        struct jf *f = &F[i];
        if (!f->used) continue;
        if (f->fail) { f->used = 0; deliver(f->id, f->script, 0, "", "", 0, 0, f->fail, f->url, ""); continue; }
        if (f->file) { read_file(f); continue; }
        int r = fetch_pump(&f->f);
        if (r == 0 && uptime_ms() - f->f.last < 20000) continue;
        if (r <= 0) { fetch_cancel(&f->f); f->used = 0; deliver(f->id, f->script, 0, "", "", 0, 0, r ? "read error" : "timeout", f->url, ""); continue; }
        struct response rs;
        fetch_finish(&f->f, &rs);
        f->used = 0;
        if (!rs.err[0] && rs.status >= 300 && rs.status < 400 && rs.loc[0] && f->hops < 5) {
            char next[URL_MAX];
            url_resolve(f->url, rs.loc, next, sizeof next);
            int hops = f->hops + 1;
            free(rs.body);
            char *pd = rs.status == 307 || rs.status == 308 ? f->post : (free(f->post), (char *)0);
            slot_start(f, f->id, next, f->script, pd);
            f->hops = hops;
            continue;
        }
        deliver(f->id, f->script, rs.status, rs.text, rs.ctype, rs.body, rs.blen, rs.err[0] ? rs.err : 0, f->url, rs.hdrs);
        free(rs.body); free(f->post); f->post = 0;
    }
    for (int i = 0; i < MAXF && nfq && active; i++)
        if (!F[i].used) { slot_start(&F[i], FQ[0].id, FQ[0].url, FQ[0].script, FQ[0].post); memmove(FQ, FQ + 1, --nfq * sizeof *FQ); }
}

int js_busy(void) {
    if (!active) return 0;
    for (int i = 0; i < MAXF; i++) if (F[i].used) return 1;
    return nfq > 0;
}

/* ---------- page lifecycle ---------- */
static int uses_js(const char *s, size_t n) {
    for (size_t i = 0; i + 8 < n; i++) {
        if (s[i] == '<' && !strncasecmp(s + i + 1, "script", 6)) return 1;
        if (isspace((unsigned char)s[i]) && (s[i + 1] | 32) == 'o' && (s[i + 2] | 32) == 'n' && isalpha((unsigned char)s[i + 3])) {
            size_t j = i + 3;
            while (j < n && isalpha((unsigned char)s[j])) j++;
            while (j < n && s[j] == ' ') j++;
            if (j < n && s[j] == '=' && j - i > 5) return 1;   /* an onclick= style attribute */
        }
    }
    return 0;
}

static void scan_scripts(int id, int where) {
    if (N[id].type == 1 && !strcmp(N[id].tag, "script")) { if (!N[id].started) add_script(id, where); return; }
    for (int c = N[id].first; c; c = N[c].next) if (N[c].type == 1) scan_scripts(c, where);
}

static JSValue global_fn(const char *name) {
    JSValue g = JS_GetGlobalObject(ctx), f = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    return f;
}

int js_page(const char *src, size_t n, const char *url) {
    js_reset();
    if (!uses_js(src, n)) return 0;
    rt = JS_NewRuntime();
    if (!rt) return 0;
    JS_SetMemoryLimit(rt, 64u << 20);
    JS_SetMaxStackSize(rt, 1u << 20);
    JS_SetInterruptHandler(rt, on_interrupt, 0);
    JS_SetModuleLoaderFunc(rt, mod_norm, mod_load, 0);
    JS_SetHostPromiseRejectionTracker(rt, on_reject, 0);
    ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); rt = 0; return 0; }
    strlcpy(page_url, url, sizeof page_url);
    css_dom_set(&CDOM);
    new_node(0, 0);                                        /* id 0 = null */
    new_node(9, 0);                                        /* 1 document */
    insert_before(1, new_node(1, "html"), 0);              /* 2 */
    insert_before(2, new_node(1, "head"), 0);              /* 3 */
    parse_html(1, src, n, 0);
    JSValue g = JS_GetGlobalObject(ctx), d = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, d, NATIVES, sizeof NATIVES / sizeof *NATIVES);
    JS_SetPropertyStr(ctx, g, "__d", d);
    JS_FreeValue(ctx, g);
    active = 1;
    scan_scripts(1, 0);
    enter();
    JSValue r = JS_Eval(ctx, js_prelude, strlen(js_prelude), "<dom>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { report(ctx); dead = 1; }
    JS_FreeValue(ctx, r);
    if (!dead) {
        r = JS_Eval(ctx, js_prelude2, strlen(js_prelude2), "<web>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) { report(ctx); dead = 1; }
        JS_FreeValue(ctx, r);
    }
    depth = 0;
    fn_dispatch = global_fn("__dispatch"); fn_fire = global_fn("__fire"); fn_fetch = global_fn("__fetchDone");
    return 1;
}

static int ci_has(const char *h, const char *nd) {
    for (size_t l = strlen(nd); *h; h++) if (!strncasecmp(h, nd, l)) return 1;
    return 0;
}

static void fire(int node, const char *type) {            /* script load/error events */
    JSValue a[3] = { JS_NewInt32(ctx, node), str(type), JS_UNDEFINED };
    call(fn_dispatch, 3, a);
}

/* blocking GET for module imports (QuickJS wants the source back from the loader); ponytail: freezes the page for the download */
static char *sync_get(const char *url, size_t *len) {
    char cur[URL_MAX];
    strlcpy(cur, url, sizeof cur);
    for (int hop = 0; hop < 6; hop++) {
        if (!url_is_web(cur)) {
            struct jf f;
            memset(&f, 0, sizeof f);
            strlcpy(f.url, cur, sizeof f.url);
            char path[PATH_MAX];
            url_file_path(cur, path, sizeof path);
            int fd = open(path, O_RDONLY);
            struct stat st;
            char *b = 0;
            size_t got = 0;
            if (fd >= 0 && fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode) && (b = malloc(st.st_size + 1))) {
                ssize_t r;
                while (got < (size_t)st.st_size && (r = read(fd, b + got, st.st_size - got)) > 0) got += r;
                b[got] = 0;
            }
            if (fd >= 0) close(fd);
            *len = got;
            return b;
        }
        struct fetch f;
        if (fetch_start(&f, cur, 0) < 0) return 0;
        uint32_t t0 = uptime_ms();
        int r;
        while ((r = fetch_pump(&f)) == 0 && uptime_ms() - t0 < 20000) usleep(4000);
        if (r <= 0) { fetch_cancel(&f); return 0; }
        struct response rs;
        fetch_finish(&f, &rs);
        if (!rs.err[0] && rs.status >= 300 && rs.status < 400 && rs.loc[0]) { url_resolve(cur, rs.loc, cur, sizeof cur); free(rs.body); continue; }
        if (rs.err[0] || rs.status >= 400) { free(rs.body); return 0; }
        *len = rs.blen;
        return rs.body;
    }
    return 0;
}

static char *mod_norm(JSContext *c, const char *base, const char *name, void *op) {
    char out[URL_MAX];
    url_resolve(base, name, out, sizeof out);
    url_strip_frag(out);
    return js_strdup(c, out);
}

static JSModuleDef *mod_load(JSContext *c, const char *name, void *op) {
    size_t l;
    char *b = sync_get(name, &l);
    if (!b) { JS_ThrowReferenceError(c, "could not load module %s", name); return 0; }
    JSValue v = JS_Eval(c, b, l, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(b);
    if (JS_IsException(v)) return 0;
    JSModuleDef *m = JS_VALUE_GET_PTR(v);
    JS_FreeValue(c, v);
    return m;
}

/* queue (or, for an inline dynamic script, run) a <script> element once */
static void add_script(int id, int where) {                /* where: 0 parser, 1 document.write, 2 inserted by a script */
    N[id].started = 1;
    const char *ty = attr(id, "type"), *src = attr(id, "src");
    int module = ty && !strcasecmp(ty, "module");
    if (ty && *ty && !module && !ci_has(ty, "javascript") && !ci_has(ty, "ecmascript")) return;
    struct buf b = { 0 };
    if (!(src && *src)) { text_of(&b, id); if (!b.n) { free(b.p); return; } free(b.p); }
    struct scr e = { id, 0, src && *src ? 1 : 2, module, 0, where == 0 ? nscr : 0, 0, 0 };
    if (src && *src) { char u[URL_MAX]; url_resolve(page_url, src, u, sizeof u); e.url = strdup(u); }
    if (where == 2 || (e.url && attr(id, "async"))) e.mode = 2;
    else if (module || (e.url && attr(id, "defer"))) e.mode = 1;
    struct scr *slot;
    if (e.mode == 2) {
        int i = 0;
        while (i < MAXDYN && D[i].state && D[i].state != 4) i++;
        if (i == MAXDYN) { free(e.url); jlog(2, "too many scripts at once"); return; }
        slot = &D[i];
    } else {
        if (nscr == MAXSCR) { free(e.url); return; }
        int at = where == 1 ? cur_scr + 1 : nscr;
        memmove(S + at + 1, S + at, (nscr - at) * sizeof *S);
        nscr++;
        slot = &S[at];
    }
    *slot = e;
    if (e.url) { slot->fid = fetch_queue(e.url, 1, 0, 0); if (slot->fid < 0) slot->state = 3; }
    idle = 0;
    if (where == 2 && !e.url && ctx && !dead) exec_entry(slot);       /* inline: runs at once, as jQuery's globalEval etc. expect */
}

static void exec_entry(struct scr *e) {                    /* state 2 or 3: run it / report it, then done */
    struct scr c = *e;
    e->state = 4; e->body = 0; e->url = 0;
    if (c.state == 2) {
        struct buf b = { 0 };
        const char *code = c.body;
        size_t n = c.body ? strlen(c.body) : 0;
        if (!code) { text_of(&b, c.node); code = b.p; n = b.n; }
        char name[URL_MAX];
        if (c.url) strlcpy(name, c.url, sizeof name);
        else if (c.module) strlcpy(name, page_url, sizeof name);
        else { const char *sl = strrchr(page_url, '/'); snprintf(name, sizeof name, "%s (inline script)", sl && sl[1] ? sl + 1 : page_url); }
        cur_script = c.module ? 0 : c.node;
        if (code) run_script(code, n, name, c.module);
        cur_script = 0;
        free(b.p);
        if (c.url) fire(c.node, "load");
    } else fire(c.node, "error");
    free(c.body); free(c.url);
}

static void set_ready(int st, const char *ev) {
    rstate = st;
    if (ev) { JSValue a[3] = { JS_NewInt32(ctx, 1), str(ev), JS_UNDEFINED }; call(fn_dispatch, 3, a); }
}

static void run_scripts(void) {
    if (!active || dead) return;
    for (int i = 0; i < MAXDYN && !dead; i++) if ((D[i].state == 2 || D[i].state == 3) && cur_scr >= D[i].rel) exec_entry(&D[i]);
    idle = 0;
    while (!dead && active && cur_scr < nscr) {                /* parser-blocking scripts, in order */
        struct scr *e = &S[cur_scr];
        if (e->mode == 1 || e->state == 4) { cur_scr++; continue; }
        if (e->state == 1) { idle = 1; return; }               /* still loading: later scripts wait */
        if (e->state == 0) e->state = 2;
        exec_entry(e);
        cur_scr++;
    }
    if (dead || !active) return;
    if (rstate == 0) set_ready(1, "readystatechange");
    for (; cur_def < nscr && !dead; ) {                        /* defer / module scripts */
        struct scr *e = &S[cur_def];
        if (e->mode != 1 || e->state == 4) { cur_def++; continue; }
        if (e->state == 1) { idle = 1; return; }
        if (e->state == 0) e->state = 2;
        exec_entry(e);
        cur_def++;
    }
    if (!loaded && !dead) {
        loaded = 1;
        fire(1, "DOMContentLoaded");
    }
    for (int i = 0; i < MAXDYN; i++) if (D[i].state == 1) { idle = 1; return; }     /* load waits for async scripts */
    if (rstate < 2 && !dead) {
        set_ready(2, "readystatechange");
        JSValue a2[3] = { JS_NewInt32(ctx, 0), str("load"), JS_UNDEFINED };
        call(fn_dispatch, 3, a2);
    }
    idle = 1;
}

int js_pump(void) {
    if (!active) return 0;
    pump_fetches();
    if (++pumps > 1) run_scripts();                       /* the page gets painted once before its scripts run */
    uint32_t now = uptime_ms();
    int due[MAXT], nd = 0;
    for (int i = 0; i < nt; i++) if ((int32_t)(now - T[i].due) >= 0) due[nd++] = T[i].id;
    for (int k = 0; k < nd && !dead; k++) {                /* fire in id order; a handler may clear later ones */
        int i;
        for (i = 0; i < nt && T[i].id != due[k]; i++) ;
        if (i == nt) continue;
        int rep = T[i].every > 0;
        if (rep) { T[i].due += T[i].every; if ((int32_t)(now - T[i].due) > 0) T[i].due = now + T[i].every; }
        else T[i] = T[--nt];
        JSValue argv[2] = { JS_NewInt32(ctx, due[k]), JS_NewBool(ctx, rep) };
        call(fn_fire, 2, argv);
    }
    int d = dirty;
    dirty = 0;
    return d;
}

int js_timeout(void) {
    if (active && !dead && !idle) return 0;                 /* scripts still to run */
    if (!active || dead || !nt) return -1;
    uint32_t now = uptime_ms();
    int best = 1 << 30;
    for (int i = 0; i < nt; i++) { int32_t d = (int32_t)(T[i].due - now); if (d < best) best = d < 0 ? 0 : d; }
    return best;
}

void js_reset(void) {
    for (int i = 0; i < MAXF; i++) if (F[i].used) { if (!F[i].file) fetch_cancel(&F[i].f); F[i].used = 0; free(F[i].post); F[i].post = 0; }
    for (int i = 0; i < nfq; i++) free(FQ[i].post);
    nfq = 0;
    if (ctx) {
        JS_FreeValue(ctx, fn_dispatch); JS_FreeValue(ctx, fn_fire); JS_FreeValue(ctx, fn_fetch);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    fn_dispatch = fn_fire = fn_fetch = JS_UNDEFINED;
    ctx = 0; rt = 0;
    for (int i = 0; i < nn; i++) {
        struct node *x = &N[i];
        free(x->text); free(x->val);
        for (int k = 0; k < x->na; k++) { free(x->an[k]); free(x->av[k]); }
        free(x->an); free(x->av);
    }
    free(N);
    N = 0; nn = ncap = 0;
    free(RC); RC = 0; nrc = rc_set = 0;
    for (int i = 0; i < nscr; i++) { free(S[i].body); free(S[i].url); }
    for (int i = 0; i < MAXDYN; i++) { free(D[i].body); free(D[i].url); }
    memset(S, 0, sizeof S); memset(D, 0, sizeof D);
    cur_def = cur_script = rstate = idle = strikes = url_changed = depth = 0; scroll_req_y = -1; anchor_req[0] = 0;
    free(alert_text);
    alert_text = 0;
    nscr = cur_scr = nt = 0;
    active = dirty = dead = killed = loaded = focus_req = pumps = 0;
    nav[0] = 0;
    live = 0;
}

int js_active(void) { return active; }
const char *js_nav(void) {
    static char out[URL_MAX];
    if (!nav[0]) return 0;
    strlcpy(out, nav, sizeof out);
    nav[0] = 0;
    return out;
}
int js_focus_req(void) { int f = focus_req; focus_req = 0; return f; }
char *js_alert(void) { char *a = alert_text; alert_text = 0; return a; }

/* main.c tells scripts about the viewport (window.scrollY/innerWidth, getBoundingClientRect) and picks up what they ask for */
void js_viewport(int scroll, int w, int h) { vscroll = scroll; vw = w; vh = h; }
int js_scroll_req(void) { int y = scroll_req_y; scroll_req_y = -1; return y; }                 /* window.scrollTo: document y, or -1 */
const char *js_anchor_req(void) { static char a[128]; if (!anchor_req[0]) return 0; strlcpy(a, anchor_req, sizeof a); anchor_req[0] = 0; return a; }   /* location.hash set: scroll to this anchor */
const char *js_url_change(void) { static char u[URL_MAX]; if (!url_changed) return 0; url_changed = 0; strlcpy(u, page_url, sizeof u); return u; }   /* pushState/replaceState/hash: new address-bar URL */
int js_submit_req(void) { int f = submit_req; submit_req = 0; return f; }                   /* form.submit(): the form node, consumed, or 0 */
