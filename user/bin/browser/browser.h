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

/* ---- fetch.c: HTTP runs in a child process (/bin/browser --fetch URL) so DNS and connect
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
int  fetch_main(const char *url, const char *post);   /* the child; post = form body for a POST, or NULL */
int  fetch_start(struct fetch *f, const char *url, const char *post);
int  fetch_pump(struct fetch *f);                 /* 0 running, 1 finished, -1 failed */
void fetch_cancel(struct fetch *f);
void fetch_reap(void);                            /* collect killed children */
void fetch_finish(struct fetch *f, struct response *r);   /* consumes f->buf */

/* ---- css.c: <style>/style="" -> per-element property set ---- */
enum { P_COLOR, P_BG, P_BOLD, P_ALIGN, P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL, P_DISPLAY, P_DECO,
       P_BWT, P_BWR, P_BWB, P_BWL, P_BCT, P_BCR, P_BCB, P_BCL, P_WIDTH, P_MAXW, P_HEIGHT, P_FSIZE, P_MONO, P_ITALIC, P_LIST, P_N };
enum { K_PX, K_PCT, K_AUTO };                    /* value kinds; colours are K_PX with the colour in v (0 = transparent) */
enum { D_NONE = 1, D_BLOCK, D_INLINE };
struct cprops { uint32_t set; int v[P_N]; uint8_t k[P_N]; };
struct el { char name[16], id[32], cls[64]; int tag, js; };   /* one open element, for selector matching; js: DOM node id */
void css_reset(void);
void css_add(const char *s, size_t n);           /* append a stylesheet */
int  css_any(void);
void css_cascade(const struct el *st, int n, const char *style, struct cprops *out);   /* st[n-1] is the subject */
int  css_color(const char *s, color_t *out, int legacy);   /* names, #rgb, #rrggbb, rgb()/rgba(); legacy: bare hex too */
int  css_match(const char *sel, const struct el *st, int n);   /* selector list vs st[n-1]: 1/0, -1 = nothing parsable */

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
