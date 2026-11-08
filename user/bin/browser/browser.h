/* browser - "Vibe Browser": HTTP/1.x client + HTML subset renderer (see main.c) */
#pragma once
#include <ui.h>
#include <image.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PAGE (2u << 20)      /* body bytes kept per page */
#define URL_MAX  1024

/* ---- url.c ---- */
void url_resolve(const char *base, const char *ref, char *out, int cap);
void url_from_input(const char *in, char *out, int cap);       /* address-bar text -> absolute URL */
int  url_split_http(const char *url, char *host, int hcap, int *port, char *path, int pcap);   /* http:// or https:// */
int  url_is_web(const char *url);
void url_unescape(const char *s, char *out, int cap);          /* %XX -> byte */
void url_strip_frag(char *url);                                /* in place */
void url_file_path(const char *url, char *out, int cap);       /* file: URL -> filesystem path */

/* ---- fetch.c: HTTP runs in a child process (/bin/vfetch URL, a small binary of its own) so DNS and connect
 * never block the UI; the parent polls the pipe next to the window fd. ---- */
struct fetch {
    int pid, fd;
    unsigned char *buf; size_t len, cap;
    uint32_t last;                 /* uptime of the last byte (timeout) */
    long hdr_end, clen;            /* offset of the body (0 = headers incomplete), Content-Length or -1 */
    int chunked, status, truncated;
};
struct response {
    int status;
    char text[48], ctype[96], loc[URL_MAX], err[160];
    char *body; size_t blen;       /* malloc'd, NUL-terminated; caller frees */
    int truncated, latin1;
};
int  fetch_main(const char *url, const char *post);   /* the child (user/bin/vfetch); post = form body for a POST, or NULL */
int  fetch_start(struct fetch *f, const char *url, const char *post);
int  fetch_pump(struct fetch *f);                 /* 0 running, 1 finished, -1 failed */
void fetch_cancel(struct fetch *f);
void fetch_reap(void);                            /* collect killed children */
void fetch_finish(struct fetch *f, struct response *r);   /* consumes f->buf */

/* ---- css.c: stylesheets -> computed style. Two entry points:
 *  - css_compute(): the DOM-tree API (LAYOUT). Fills EVERY property of `struct cprops` with its computed value
 *    (inheritance, initial values, em/rem/vw resolved, var(), user-agent defaults, ::before/::after).
 *  - css_cascade(): legacy streaming entry (html.c today): only what the author sheets declare, in the old encoding. ---- */
enum { P_COLOR, P_BG, P_BOLD, P_ALIGN, P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL, P_DISPLAY, P_DECO,
       P_BWT, P_BWR, P_BWB, P_BWL, P_BCT, P_BCR, P_BCB, P_BCL, P_WIDTH, P_MAXW, P_HEIGHT, P_FSIZE, P_MONO, P_ITALIC, P_LIST,
       P_VALIGN, P_COLLAPSE, P_SPACING,   /* 0..31: the legacy ones (cprops.set is a bitmask over these only) */
       P_MINW, P_MINH, P_MAXH, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT,       /* inset: px/%/auto */
       P_POSITION, P_FLOAT, P_CLEAR, P_ZINDEX, P_OVX, P_OVY, P_BOXSZ, P_VISIB, P_OPACITY, P_DIR,
       P_RTL, P_RTR, P_RBR, P_RBL,                                     /* border-radius corners (px or %) */
       P_BSTT, P_BSTR, P_BSTB, P_BSTL,                                 /* border styles BS_* */
       P_BGIMG, P_BGREP, P_BGPX, P_BGPY, P_BGSW, P_BGSH,
       P_FLEXDIR, P_FLEXWRAP, P_JUSTC, P_ALIGNI, P_ALIGNC, P_ALIGNS, P_JUSTI, P_JUSTS, P_FGROW, P_FSHRINK, P_FBASIS, P_ORDER, P_RGAP, P_CGAP,
       P_GCOLS, P_GROWS, P_GFLOW, P_GACOLS, P_GAROWS, P_GCS, P_GCE, P_GRS, P_GRE, P_GAREAS, P_GAREA,
       P_WS, P_TT, P_LH, P_LS, P_WSP, P_TIND, P_TOVF, P_WBRK, P_FWEIGHT, P_FAMILY, P_VAOFF, P_SPACINGY,
       P_LISTT, P_LISTP, P_TBLAY, P_CURSOR, P_PEV, P_CONTENT, P_OBJFIT, P_ASPECT,
       P_N };
/* value kinds. K_PX also carries every keyword/enum/colour (value in v). Lengths: K_PX px, K_PCT percent*100, K_CALC compiled
 * calc()/min()/max()/clamp() with a percentage inside (resolve with css_len()). Never read p->v of a length directly: css_len(). */
enum { K_PX, K_PCT, K_AUTO, K_CALC, K_NUM /* number*100 (line-height factor) */, K_STR /* v = css_str() id */, K_NONE, K_FIT, K_MINC, K_MAXC,
       K_COVER, K_CONTAIN, K_SPAN /* grid line span */, K_GRAD /* gradient text in css_str(v) */,
       K_INHERIT, K_INITIAL, K_UNSET, K_EM, K_REM, K_CALCS, K_RAW, K_CUR };   /* the last ones only exist inside the engine */
enum { D_NONE = 1, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID, D_TABLE, D_INLINE_TABLE, D_TABLE_ROW,
       D_TABLE_CELL, D_TABLE_ROWGROUP, D_TABLE_HEADER, D_TABLE_FOOTER, D_TABLE_COL, D_TABLE_COLGROUP, D_TABLE_CAPTION, D_LIST_ITEM, D_CONTENTS };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FLT_NONE, FLT_LEFT, FLT_RIGHT };            /* P_FLOAT; P_CLEAR: 0 none, 1 left, 2 right, 3 both */
enum { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO, OV_CLIP };
enum { BS_NONE, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_GROOVE, BS_RIDGE, BS_INSET, BS_OUTSET };
enum { AL_NORMAL, AL_START, AL_END, AL_CENTER, AL_BETWEEN, AL_AROUND, AL_EVENLY, AL_STRETCH, AL_BASELINE };   /* justify-/align-* */
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE, WS_BREAK_SPACES };
enum { TA_LEFT, TA_CENTER, TA_RIGHT, TA_JUSTIFY };  /* P_ALIGN */
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_TEXT_TOP, VA_TEXT_BOTTOM, VA_SUB, VA_SUPER };   /* P_VALIGN (+ P_VAOFF px for lengths) */
enum { FAM_SANS, FAM_SERIF, FAM_MONO };
enum { LT_DISC, LT_CIRCLE, LT_SQUARE, LT_DECIMAL, LT_ZERO, LT_LALPHA, LT_UALPHA, LT_LROMAN, LT_UROMAN, LT_LGREEK, LT_NONE };
enum { REP_REPEAT, REP_X, REP_Y, REP_NONE };
enum { DECO_UNDERLINE = 1, DECO_LINETHROUGH = 2, DECO_OVERLINE = 4 };
enum { PE_NONE, PE_BEFORE, PE_AFTER };
enum { CS_HOVER = 1, CS_FOCUS = 2, CS_ACTIVE = 4, CS_CHECKED = 8, CS_DISABLED = 16, CS_LINK = 32, CS_VISITED = 64, CS_FOCUSWITHIN = 128 };
/* cprops.set (legacy css_cascade only): bit p set = property p declared */
struct cprops { uint32_t set; int v[P_N]; uint8_t k[P_N]; uint32_t bloom[8]; int vars; };   /* bloom/vars: ancestor filter + custom properties, carried to children */
struct el { char name[16], id[32], cls[64]; int tag, js; char at[240]; short nkids; uint8_t first; };   /* legacy: one open element, for selector matching; js: DOM node id;
                                                  at: "\1name\2value" pairs (only filled when css_attrs()); first: no earlier sibling element */
/* The matcher sees the document through these callbacks; nodes are small ints, 0 = none. Elements only (no text nodes). */
struct css_dom {
    int (*parent)(int n);                         /* parent element, 0 at the root (<html>) */
    int (*prev)(int n), (*next)(int n);           /* previous / next sibling ELEMENT */
    int (*first)(int n);                          /* first child ELEMENT */
    const char *(*tag)(int n);                    /* lowercase local name */
    const char *(*attr)(int n, const char *name); /* value or NULL if absent ("id", "class", "style" too; name is lowercase) */
    int (*state)(int n);                          /* CS_* bits */
    int (*has_text)(int n);                       /* 1 if n has a non-empty text child (for :empty) */
};
void css_dom_set(const struct css_dom *d);        /* before css_compute / css_match_node */
void css_initial(struct cprops *out);             /* the initial values (root's "parent") */
/* node's computed style in *out. parent = the parent element's computed style (NULL for <html>). pe = PE_BEFORE/PE_AFTER computes the pseudo-element
 * (parent = the element's own style); returns 1 if it generates a box (content other than none; out->v[P_CONTENT] is css_str() text). */
int  css_compute(int node, int pe, const struct cprops *parent, struct cprops *out);
int  css_len(const struct cprops *p, int prop, int base);      /* a length property in px: px, % of base, calc(); auto/none -> 0 */
const char *css_str(int id);                      /* text of a K_STR value (grid track lists, content, url of background-image) */
int  css_match_node(const char *sel, int node);   /* matches()/closest()/querySelector for the css_dom tree: 1/0, -1 = unparsable */
void css_base(const char *base);                  /* base URL for url() in style="" attributes */
void css_viewport(int w);                         /* window width for @media and vw units */
void css_viewport_size(int w, int h);             /* ... and height */
void css_reset(void);
void css_add(const char *s, size_t n);           /* append a stylesheet (author origin, in document order) */
void css_add_from(const char *s, size_t n, const char *base);   /* same; base resolves @import and url() (0 = none) */
int  css_media(const char *q);                   /* media query list (a media="" attribute): 1 = applies on screen */
int  css_attrs(void);                            /* legacy: some rule uses an attribute selector: elements need el.at */
int  css_any(void);
void css_cascade(const struct el *st, int n, const char *style, struct cprops *out);   /* legacy; st[n-1] is the subject */
int  css_color(const char *s, color_t *out, int legacy);   /* any CSS colour (alpha in the top byte; transparent = 0); legacy: bare hex too */
int  css_match(const char *sel, const struct el *st, int n);   /* legacy: selector list vs st[n-1]: 1/0, -1 = nothing parsable */

/* ---- sheet.c: external stylesheets (<link rel=stylesheet>, @import): one fetch each, kept until the next page ---- */
const char *sheet_get(const char *url, size_t *len);   /* the text once loaded; NULL while loading or failed (queues on first use) */
int  sheet_pump(void);                           /* advance downloads; nonzero when a sheet arrived or failed */
int  sheet_busy(void);
void sheet_reset(void);                          /* new page */

/* ---- img.c: per-page image cache. Images are fetched by helper processes (two at a time), decoded
 * in-process and kept (plus one scaled copy) until the next page. ---- */
enum { IMG_QUEUED, IMG_LOADING, IMG_READY, IMG_FAILED };
int  img_ref(const char *url);                   /* index; queues the download on first use */
int  img_info(int i, int *w, int *h);            /* IMG_*; size when ready */
const uint32_t *img_pixels(int i, int w, int h, color_t bg);   /* w*h opaque pixels (scaled, over bg), or NULL */
int  img_pump(void);                             /* advance downloads; nonzero when an image became ready/failed */
int  img_busy(void);                             /* images still queued or downloading */
void img_reset(void);                            /* new page */
int  img_add_data(const char *url, const uint8_t *d, size_t n);   /* decode a body we already have */

/* ---- html.c ---- */
enum { C_TEXT, C_PASS, C_AREA, C_BTN, C_CHECK, C_RADIO, C_HIDDEN, C_SELECT };
struct ctl { uint8_t kind, sub, chk; int form, ord, item, node; uint32_t name, val; int o0, no, cols, rows; };   /* sub: C_BTN 1 = submits */
struct opt { uint32_t lab, val; };
struct form { uint32_t action; uint8_t post; int node; };
#define FL_UL 1
#define FL_STRIKE 2
#define FL_IMG 4
#define FL_CTL 8
struct item { int x, y; short w, h, bel; uint8_t font, flags; uint16_t len; color_t col; int link; uint32_t off; int ref, node; };   /* node: js.c DOM id */
struct decor { int x, y, w, h; color_t col; };
struct anchor { char name[40]; int y; };
struct doc {
    struct item *it; int nit, itcap;
    struct decor *dc; int ndc, dccap;
    char *txt; size_t ntxt, txtcap;
    char **link; int nlink, linkcap;
    struct anchor *an; int nan;
    struct ctl *ct; int nct, ctcap;
    struct opt *op; int nop, opcap;
    struct form *fm; int nfm, fmcap;
    char title[128];
    color_t bg, fg, linkc, hoverc;
    int height, truncated, maxh;     /* maxh: tallest item (images, textareas), for the y search */
    int *pmax, *smin;                /* per item: max bottom edge of items 0..i, min top edge of items i..end (table cells make y non-monotonic) */
};
void doc_free(struct doc *d);
void doc_layout(struct doc *d, const char *src, size_t n, int is_html, int width, const char *base);
/* sel: selection (document order, item/char offsets; from < 0 = none), find: highlight needle (or NULL) with the
 * active match at find_it/find_off */
struct docmark { int from_it, from_ch, to_it, to_ch; const char *find; int find_it, find_off; };
void doc_draw(struct ui *ui, const struct doc *d, int vx, int vy, int vw, int vh, int scroll, int hover,
              const struct docmark *mk, int focus_ord, int caret);
int  doc_hit(const struct doc *d, int x, int y);          /* link index at document coords, or -1 */
int  doc_pos_at(const struct doc *d, int x, int y, int *item, int *ch);   /* text run + byte offset nearest the point (for selecting) */
int  doc_ctl_at(const struct doc *d, int x, int y);       /* control index under the point, or -1 */
int  doc_anchor(const struct doc *d, const char *name);   /* y, or -1 */
int  doc_node_at(const struct doc *d, int x, int y);      /* DOM node (js.c) of the innermost item under the point, or 0 */
void html_unescape(char *s);                              /* decode entities in place */
char *doc_text(const struct doc *d, int a_it, int a_ch, int b_it, int b_ch);   /* malloc'd selection text (a <= b) */
int  doc_find(const struct doc *d, const char *needle, int from_it, int from_off, int dir, int *it, int *off);   /* 1 = found */
int  doc_count(const struct doc *d, const char *needle);

/* ---- form.c: control state (survives re-layout), drawing, editing, submit URLs ---- */
struct fstate { char *val; int cap, on, sel, init, sx, sy; };   /* sx/sy: text scroll */
struct fstate *fs_get(const struct doc *d, int ci);      /* state of control ci (lazily initialised) */
struct fstate *fs_ord(int ord);                          /* by ordinal, or NULL */
void fs_set(struct fstate *s, const char *v);
void fs_reset(void);
void ctl_draw(struct canvas *c, const struct doc *d, const struct ctl *t, int x, int y, int w, int h, int focused, int caret);
int  ctl_key(const struct doc *d, int ci, const struct gui_event *k, int *caret);   /* 1 = Enter pressed */
int  ctl_caret_at(const struct doc *d, int ci, int rx, int ry);
void ctl_toggle(const struct doc *d, int ci);            /* click on a checkbox / radio */
void form_reset(const struct doc *d, int form);
/* URL (GET) or body (POST) for submitting `form`, pressed by control `btn` (or -1): returns 1 for POST, 0 for GET */
int  form_submit(const struct doc *d, int form, int btn, const char *base, char *url, int ucap, char *body, int bcap);

/* ---- js.c: <script> and a DOM over QuickJS. A page with scripts is kept as a node tree; layout runs on the tree
 * serialised back to HTML with data-vjs="ID" on every element, so items, controls and forms know their node. ---- */
int  js_page(const char *src, size_t n, const char *url);   /* new page: 1 = it has scripts (the DOM takes over) */
char *js_html(size_t *n);                         /* the tree as HTML for doc_layout (malloc'd) */
void js_attach(struct doc *d);                    /* the doc laid out from the last js_html() */
void js_sync(void);                               /* copy form state into the tree before a relayout */
int  js_pump(void);                               /* run scripts, timers, fetches: 1 = the tree changed (relayout) */
int  js_timeout(void);                            /* ms until the next timer, -1 = none */
int  js_busy(void);                               /* script or fetch downloads in flight */
void js_reset(void);                              /* leave the page */
/* dispatch `type` (bubbling) at node (0 = window); k: key event or NULL. Returns 1 if a handler called preventDefault */
int  js_event(int node, const char *type, const struct gui_event *k, int x, int y);
int  js_active(void);
const char *js_nav(void);                         /* location set by a script: navigate there (consumes it), or NULL */
int  js_focus_req(void);                          /* element.focus() target (consumes it), or 0 */
char *js_alert(void);                             /* pending alert() text (malloc'd, consumed), or NULL */
int  js_log_count(void);                          /* console lines (oldest first) */
const char *js_log_line(int i, int *kind);        /* kind: 0 log, 1 warn, 2 error */
extern const char js_prelude[];                   /* dom_js.c: the DOM classes over the natives */
void browser_status(int bad, const char *fmt, ...);   /* main.c: the status bar */
