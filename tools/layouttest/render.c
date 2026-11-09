/* render.c - host harness: lay an HTML file out with the browser's real engine and draw the display list into a PPM.
 *   render FILE.html WIDTH OUT.ppm [HEIGHT] [BASEURL]     external CSS: files in $LT_SHEETS named by the URL with non-alnum -> '_' */
#include "browser.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
const char *sheet_get(const char *url, size_t *len) {
    static char *keep[256]; static int nk;
    const char *dir = getenv("LT_SHEETS");
    if (!dir) return 0;
    char p[1400]; int k = snprintf(p, sizeof p, "%s/", dir);
    for (const char *u = url; *u && k < 1390; u++) p[k++] = ((*u >= '0' && *u <= '9') || ((*u | 32) >= 'a' && (*u | 32) <= 'z')) ? *u : '_';
    if (k - (int)strlen(dir) - 1 > 150) { int full = k - strlen(dir) - 1; k = strlen(dir) + 1 + 120; k += snprintf(p + k, 20, "_%d", full); }
    p[k] = 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f);
    if (nk < 256) keep[nk++] = b;
    *len = n;
    return b;
}
struct theme ui_th;
int gui_clip_set(const char *s, int n) { return 0; }
char *gui_clip_get(int *n) { return 0; }
int img_pick(const char *s, const char *ss, const char *sz, int vw, char *out, int cap) { return 0; }
int img_type_ok(const char *m) { return 1; }
int img_svg_inline(const char *s, size_t n, long c) { return -1; }
const char *img_url(int i) { return ""; }
int doc_zoom = 100;
int js_active(void) { return 0; }
void js_set_rect(int n, int x, int y, int w, int h) {}
int dom_first(int n) { return 0; }
int dom_next(int n) { return 0; }
int dom_type(int n) { return 0; }
const char *dom_tag(int n) { return ""; }
const char *dom_text(int n) { return ""; }
int dom_nattr(int n) { return 0; }
int dom_attr_at(int n, int i, const char **a, const char **v) { return 0; }
const char *dom_ctl_value(int n) { return ""; }
int dom_ctl_checked(int n) { return 0; }
int dom_ctl_sel(int n) { return 0; }
const char *dom_attr(int n, const char *a) { return 0; }
int sheet_pump(void) { return 0; }
int sheet_busy(void) { return 0; }
void sheet_reset(void) {}
int img_ref(const char *u) { return 0; }
int img_ref_ex(const char *u, const char *l) { return 0; }
int img_info(int i, int *w, int *h) { return IMG_LOADING; }
const uint32_t *img_pixels(int i, int w, int h, color_t bg) { return 0; }
void browser_status(int bad, const char *fmt, ...) {}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(n + 1); fread(src, 1, n, f); src[n] = 0; fclose(f);
    int w = atoi(argv[2]), H = argc > 4 ? atoi(argv[4]) : 0;
    const char *base = argc > 5 ? argv[5] : "http://test.invalid/";
    struct doc d; memset(&d, 0, sizeof d);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    doc_layout(&d, src, n, !strstr(argv[1], ".txt"), w, base);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "layout %.1f ms, %d items, %d decors, height %d\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, d.nit, d.ndc, d.height);
    int h = H ? H : d.height > 12000 ? 12000 : d.height;
    uint32_t *px = calloc((size_t)w * h, 4);
    struct canvas c; canvas_init(&c, px, w, h, w);
    struct ui ui; memset(&ui, 0, sizeof ui); ui.c = &c;
    doc_draw(&ui, &d, 0, 0, w, h, getenv("LT_SCROLL") ? atoi(getenv("LT_SCROLL")) : 0, -1, 0, -1, 0);
    FILE *o = fopen(argv[3], "wb");
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { fputc(px[i] >> 16, o); fputc(px[i] >> 8, o); fputc(px[i], o); }
    fclose(o);
    return 0;
}
