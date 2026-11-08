/* browser - "Vibe Browser": HTTP/1.x client + HTML subset renderer (see main.c) */
#pragma once
#include <ui.h>
#include <image.h>
#include <stddef.h>
#include <stdint.h>

/* the browser's faces have wide glyph coverage and a box for what is still missing (common/fontsw.c) */
#define font_ui bfont_ui
#define font_ui_bold bfont_ui_bold
#define font_title bfont_title
#define font_mono bfont_mono
#define font_mono_bold bfont_mono_bold

#define MAX_PAGE (2u << 20)      /* body bytes kept per page */
#define URL_MAX  1024
/* what we call ourselves everywhere (HTTP User-Agent; JS navigator.userAgent should say the same): Firefox-shaped so sites
 * serve the normal page, VibeBrowser at the end so we're still identifiable */
#define BROWSER_UA "Mozilla/5.0 (X11; Linux i686; rv:128.0) Gecko/20100101 Firefox/128.0 VibeBrowser/1.0"

/* ---- url.c ---- */
void url_resolve(const char *base, const char *ref, char *out, int cap);
void url_from_input(const char *in, char *out, int cap);       /* address-bar text -> absolute URL */
int  url_split_http(const char *url, char *host, int hcap, int *port, char *path, int pcap);   /* http:// or https:// */
int  url_is_web(const char *url);                              /* http:, https: or data: (fetch_start handles all three) */
void url_unescape(const char *s, char *out, int cap);          /* %XX -> byte */
void url_strip_frag(char *url);                                /* in place */
void url_file_path(const char *url, char *out, int cap);       /* file: URL -> filesystem path */

/* ---- frame.c: where a response body ends (shared with vfetch) ---- */
enum { FR_NONE, FR_LEN, FR_CHUNK, FR_CLOSE };
struct fr { int mode, st, ext, line, done; long rem, sz; };
void   fr_init(struct fr *f, int head, int status, long clen, int chunked);
size_t fr_eat(struct fr *f, const unsigned char *b, size_t n);   /* body bytes in: how many belong to the response; f->done when complete */
int    hdr_get(const char *h, size_t n, const char *name, char *out, int cap);   /* header value (trimmed) from a header block */

/* ---- fetch.c: HTTP runs in a child process (/bin/vfetch URL, a small binary of its own) so DNS and connect
 * never block the UI; the parent polls the pipe next to the window fd. ---- */
struct fetch {
    int pid, fd;
    unsigned char *buf; size_t len, cap;
    uint32_t last;                 /* uptime of the last byte (timeout) */
    long hdr_end, clen;            /* offset of the body (0 = headers incomplete), Content-Length or -1 */
    int chunked, status, truncated;
    int head, ready;               /* HEAD request (no body); buf already holds the whole response (data: URL, cache hit) */
    char *url;                     /* for the cache (malloc'd; NULL = don't cache) */
    struct fr fr; size_t fpos;     /* body framing: fed up to fpos body bytes */
    int wk;                        /* index of the pooled keep-alive helper doing this fetch, or -1 (one-shot process / no process) */
};
struct response {
    int status;
    char text[48], ctype[96], loc[URL_MAX], err[160];
    char hdrs[3072];               /* the response header lines after the status line ("Name: value\r\n"...), for fetch()/XHR */
    char *body; size_t blen;       /* malloc'd, NUL-terminated; caller frees. Already inflated (gzip/deflate) and, for text, always UTF-8 */
    int truncated, latin1;         /* latin1: unused (always 0): the charset conversion happens in fetch_finish */
};
int  fetch_main(const char *url, const char *post);   /* the child (user/bin/vfetch); post = form body for a POST, or NULL */
int  fetch_start(struct fetch *f, const char *url, const char *post);
/* any request (fetch()/XHR): method (NULL = GET, or POST with a body), hdrs ("Name: v\r\nName: v" or NULL; Host, Cookie,
 * Content-Length, Connection, Accept-Encoding, Referer, Origin are ours and ignored), body/blen (may hold NULs; blen 0 = no body,
 * but method POST/PUT then sends an empty one). Cookies are sent and stored by vfetch as for any page. 0 / -1 */
int  fetch_start_req(struct fetch *f, const char *url, const char *method, const char *hdrs, const char *body, size_t blen);
int  fetch_pump(struct fetch *f);                 /* 0 running, 1 finished, -1 failed */
void fetch_cancel(struct fetch *f);
void fetch_reap(void);                            /* collect killed children */
void fetch_finish(struct fetch *f, struct response *r);   /* consumes f->buf */
void fetch_set_referer(const char *url);          /* Referer for the requests that follow (the current page) */
void fetch_cache_drop(const char *url);           /* forget one URL (reload), or everything when url is NULL */

/* ---- cookie.c: the jar (~/.config/browser/cookies), shared with the vfetch processes ---- */
void cookie_init(void);                           /* browser start: forget session cookies */
void cookie_clear(void);
/* "a=1; b=2" for a request to host + path (path may carry ?query); from_js: leave out HttpOnly. Returns the length */
int  cookie_header(const char *host, const char *path, int secure, int from_js, char *out, int cap);
/* file one Set-Cookie value, or a document.cookie assignment (from_js = 1); secure_origin: the page is https */
void cookie_store(const char *host, const char *rpath, const char *setcookie, int from_js, int secure_origin);
void cookie_get(const char *url, char *out, int cap);          /* document.cookie getter for the page at url (no HttpOnly), "a=1; b=2" */
void cookie_set(const char *url, const char *setcookie);       /* document.cookie = "..." on the page at url */

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
       P_OUTW, P_OUTS, P_OUTC, P_OUTO,                                 /* outline width/style (BS_*)/colour/offset */
       P_SHX, P_SHY, P_SHB, P_SHS, P_SHC, P_SHI,                       /* box-shadow, first layer: offset x/y, blur, spread, colour (K_NONE = no shadow), inset 0/1 */
       P_CRESET, P_CINC,                                               /* counter-reset / counter-increment: K_STR "name N name N" (K_NONE = none); see css_cnt_* */
       P_OBJPX, P_OBJPY, P_TRX, P_TRY, P_MASKIMG,                      /* object-position (px/%), transform: translate() offset (px/%), mask-image (K_STR url or K_NONE; draw the background colour through it) */
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
enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_MARKER };   /* PE_MARKER: ::marker style only (returns 1 if a rule matched; content K_NONE = the default marker) */
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
/* CSS counters. Call css_cnt_reset_all() when a layout starts, css_cnt_enter(style, depth) when entering an element (depth = nesting depth, root 0) BEFORE
 * computing its ::before/::after, css_cnt_leave(depth) when leaving it. counter()/counters() in `content` then resolve. */
void css_cnt_reset_all(void);
void css_cnt_enter(const struct cprops *style, int depth);
void css_cnt_leave(int depth);
int  css_counter_format(int lt, int n, char *out, int cap);    /* n in a list-style-type LT_*: "iv", "c", "07", a bullet...; returns length */
int  css_len(const struct cprops *p, int prop, int base);      /* a length property in px: px, % of base, calc(); auto/none -> 0 */
const char *css_str(int id);                      /* text of a K_STR value (grid track lists, content, url of background-image) */
int  css_match_node(const char *sel, int node);
/* Restyle scoping (after a change, which part of the tree needs css_compute again; hover must not restyle the page).
 * Each returns the node to restyle from (0 = no rule can be affected). *deep = 1: every descendant too (a selector looks at this as an
 * ancestor); *deep = 0: just that node, and its children only if !css_inherit_same(old_style, new_style). A returned node may be an
 * ancestor of the changed one (sibling combinators, :has). Recompute in document order, parent first. */
int  css_restyle_for_state(int node, int changed_cs_bits, int *deep);          /* CS_* bits that flipped */
int  css_restyle_for_class(int node, const char *old_cls, const char *new_cls, int *deep);
int  css_restyle_for_id(int node, const char *old_id, const char *new_id, int *deep);
int  css_restyle_for_attr(int node, const char *name, int *deep);              /* any other attribute, incl. "style" */
int  css_restyle_for_insert(int node, int *deep);                              /* element added/removed/moved */
int  css_inherit_same(const struct cprops *old_style, const struct cprops *new_style);   /* matches()/closest()/querySelector for the css_dom tree: 1/0, -1 = unparsable */
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
void *sheet_save(void);                          /* same trio as img_save, for a background tab */
void sheet_restore(void *h);
void sheet_drop(void *h);

/* ---- img.c: per-page image cache. Images are fetched by helper processes (two at a time), decoded
 * in-process and kept (plus one scaled copy) until the next page. ---- */
enum { IMG_QUEUED, IMG_LOADING, IMG_READY, IMG_FAILED };
int  img_ref(const char *url);                   /* index; queues the download on first use */
int  img_info(int i, int *w, int *h);            /* IMG_*; size when ready */
const uint32_t *img_pixels(int i, int w, int h, color_t bg);   /* w*h opaque pixels (scaled, over bg), or NULL */
int  img_pump(void);                             /* advance downloads; nonzero when an image became ready/failed */
int  img_busy(void);                             /* images still queued or downloading */
void img_reset(void);                            /* new page */
void *img_save(void);                            /* background tab: take the cache (empties it); img_restore(h) puts it back and frees h, img_drop(h) frees it */
void img_restore(void *h);
void img_drop(void *h);
int  img_add_data(const char *url, const uint8_t *d, size_t n);   /* decode a body we already have */
int  img_svg_inline(const char *svg, size_t n, long color);       /* an inline <svg> element's markup as an image (index, or -1); color = currentColor 0xRRGGBB or -1 */
const char *img_url(int i);                      /* the cache key / URL of image i */
int  img_anim_due(void);                         /* ms until an animated GIF on screen wants its next frame, -1 = none (wake the loop then; no relayout needed) */
/* srcset/sizes: the URL to load for an <img> or <source> (src may be NULL), vw = viewport width. 1 = out filled (unresolved URL) */
int  img_pick(const char *src, const char *srcset, const char *sizes, int vw, char *out, int cap);
int  img_type_ok(const char *mime);              /* a <source type=...> the decoders handle (png/jpeg/gif/svg/bmp; not webp/avif) */

/* ---- html.c ---- */
extern int doc_zoom;                             /* main.c: page zoom in percent (100 = normal); layout scales fonts, lengths and images by it */
enum { C_TEXT, C_PASS, C_AREA, C_BTN, C_CHECK, C_RADIO, C_HIDDEN, C_SELECT };
struct ctl { uint8_t kind, sub, chk, fl; int form, ord, item, node; uint32_t name, val, ph; int o0, no, cols, rows; };   /* sub: C_BTN 1 = submits; ph: placeholder (txt offset, "" = none); fl: CF_* */
enum { CF_REQ = 1, CF_EMAIL = 2, CF_AUTO = 4, CF_RO = 8 };   /* required, type=email, autofocus, readonly/disabled */
struct opt { uint32_t lab, val; };
struct form { uint32_t action; uint8_t post; int node; };
#define FL_UL 1
#define FL_STRIKE 2
#define FL_IMG 4
#define FL_CTL 8
struct item { int x, y; short w, h, clip; uint8_t font, flags; uint16_t len; color_t col; int link; uint32_t off; int ref, node; };   /* node: js.c DOM id */
struct decor { int x, y, w, h; color_t col; int it; short clip; uint8_t rad, ring; };   /* drawn before item `it`; rad: corner radius; ring: draw a border ring of that width instead of a fill */
struct cliprect { int x, y, w, h; };
struct anchor { char name[40]; int y; };
struct doc {
    struct item *it; int nit, itcap;
    struct decor *dc; int ndc, dccap;
    struct cliprect *cl; int ncl, clcap;   /* overflow clips; index 0 = none */
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
struct fstate { char *val; int cap, on, sel, init, sx, sy, anc1; };   /* sx/sy: text scroll; anc1: selection anchor byte + 1 (0 = none) */
struct fstate *fs_get(const struct doc *d, int ci);      /* state of control ci (lazily initialised) */
struct fstate *fs_ord(int ord);                          /* by ordinal, or NULL */
void fs_set(struct fstate *s, const char *v);
void fs_reset(void);
void *fs_save(void);                                     /* same trio as img_save: field values of a background tab */
void fs_restore(void *h);
void fs_drop(void *h);
void ctl_draw(struct canvas *c, const struct doc *d, const struct ctl *t, int x, int y, int w, int h, int focused, int caret);
int  ctl_key(const struct doc *d, int ci, const struct gui_event *k, int *caret);   /* 1 = Enter pressed */
int  ctl_caret_at(const struct doc *d, int ci, int rx, int ry);
int  form_check(const struct doc *d, int form, char *msg, int cap);   /* first control failing required / type=email: its index + a message, else -1 */
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
