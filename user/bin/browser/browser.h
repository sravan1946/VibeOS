/* browser - "Vibe Browser": HTTP/1.x client + HTML subset renderer (see main.c) */
#pragma once
#include <ui.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PAGE (2u << 20)      /* body bytes kept per page */
#define URL_MAX  1024

/* ---- url.c ---- */
void url_resolve(const char *base, const char *ref, char *out, int cap);
void url_from_input(const char *in, char *out, int cap);       /* address-bar text -> absolute URL */
int  url_split_http(const char *url, char *host, int hcap, int *port, char *path, int pcap);
void url_unescape(const char *s, char *out, int cap);          /* %XX -> byte */
void url_strip_frag(char *url);                                /* in place */

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
int  fetch_main(const char *url);                 /* the child */
int  fetch_start(struct fetch *f, const char *url);
int  fetch_pump(struct fetch *f);                 /* 0 running, 1 finished, -1 failed */
void fetch_cancel(struct fetch *f);
void fetch_reap(void);                            /* collect killed children */
void fetch_finish(struct fetch *f, struct response *r);   /* consumes f->buf */

/* ---- html.c ---- */
struct item { int x, y; short w, h; uint8_t font, flags; uint16_t len; color_t col; int link; uint32_t off; };
struct decor { int x, y, w, h; color_t col; };
struct anchor { char name[40]; int y; };
struct doc {
    struct item *it; int nit, itcap;
    struct decor *dc; int ndc, dccap;
    char *txt; size_t ntxt, txtcap;
    char **link; int nlink, linkcap;
    struct anchor *an; int nan;
    char title[128];
    color_t bg, fg, linkc, hoverc;
    int height, truncated;
};
void doc_free(struct doc *d);
void doc_layout(struct doc *d, const char *src, size_t n, int is_html, int width);
void doc_draw(struct ui *ui, const struct doc *d, int vx, int vy, int vw, int vh, int scroll, int hover);
int  doc_hit(const struct doc *d, int x, int y);          /* link index at document coords, or -1 */
int  doc_anchor(const struct doc *d, const char *name);   /* y, or -1 */
