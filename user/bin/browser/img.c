/* img.c - per-page image cache. <img> elements ask for a URL (img_ref); downloads run in helper
 * processes (NSLOT at a time, each one a `browser --fetch`), file:// images are read directly. A finished
 * body is decoded with the image library; anything wider than MAXDIM is shrunk right away to keep memory sane.
 * img_pixels() hands out one scaled copy per image, already composited over the page colour, so drawing
 * is a row copy. ponytail: decode runs in the UI process (one image per pump call), JPEG/GIF are unsupported. */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <syscall.h>

#define NSLOT 2
#define MAXIMG 300
#define MAXDIM 2048
#define MAX_HOPS 4
#define IMG_TIMEOUT 20000

struct pimg { char *url; int state, hops; struct image im; uint32_t *sc; int scw, sch; color_t scbg; };
static struct pimg *P;
static int np, pcap;
static struct slot { int idx; struct fetch f; char cur[URL_MAX]; } sl[NSLOT];
static int slots_init;

static void init(void) {
    if (slots_init) return;
    slots_init = 1;
    for (int i = 0; i < NSLOT; i++) sl[i].idx = -1;
}

int img_ref(const char *url) {
    init();
    for (int i = 0; i < np; i++) if (!strcmp(P[i].url, url)) return i;
    if (np >= MAXIMG) return -1;
    if (np == pcap) {
        int nc = pcap ? pcap * 2 : 32;
        struct pimg *n = realloc(P, nc * sizeof *n);
        if (!n) return -1;
        P = n; pcap = nc;
    }
    char *u = strdup(url);
    if (!u) return -1;
    P[np] = (struct pimg){ .url = u };
    return np++;
}

int img_info(int i, int *w, int *h) {
    if (i < 0 || i >= np) return IMG_FAILED;
    if (P[i].state == IMG_READY) { *w = P[i].im.w; *h = P[i].im.h; }
    return P[i].state;
}

/* take a decoded picture; shrink huge ones */
static void adopt(struct pimg *p, struct image im) {
    if (im.w > MAXDIM || im.h > MAXDIM) {
        float s = (float)MAXDIM / (im.w > im.h ? im.w : im.h);
        int w = (int)(im.w * s), h = (int)(im.h * s);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        uint32_t *px = malloc((size_t)w * h * 4);
        if (px) { image_sample(&im, px, w, h, w, 0, 0, s, 1); free(im.px); im.px = px; im.w = w; im.h = h; }
    }
    p->im = im;
    p->state = IMG_READY;
}

static void decode(struct pimg *p, const uint8_t *d, size_t n) {
    struct image im;
    if (image_decode(d, n, &im) < 0) p->state = IMG_FAILED; else adopt(p, im);
}

int img_add_data(const char *url, const uint8_t *d, size_t n) {
    int i = img_ref(url);
    if (i < 0) return -1;
    P[i].state = IMG_LOADING;
    decode(&P[i], d, n);
    return i;
}

static void slot_free(struct slot *s) { fetch_cancel(&s->f); s->idx = -1; }

static void fail(struct slot *s) { P[s->idx].state = IMG_FAILED; slot_free(s); }

static void begin(struct slot *s, const char *url) {
    strlcpy(s->cur, url, sizeof s->cur);
    if (fetch_start(&s->f, url, 0) < 0) fail(s);
}

static int pump_slot(struct slot *s) {
    struct pimg *p = &P[s->idx];
    int r = fetch_pump(&s->f);
    if (r == 0) {
        if (uptime_ms() - s->f.last > IMG_TIMEOUT) { fail(s); return 1; }
        return 0;
    }
    if (r < 0) { fail(s); return 1; }
    struct response rs;
    fetch_finish(&s->f, &rs);
    if (rs.err[0] || rs.status >= 400 || !rs.body) { free(rs.body); fail(s); return 1; }
    if (rs.status >= 300 && rs.loc[0]) {
        char next[URL_MAX];
        url_resolve(s->cur, rs.loc, next, sizeof next);
        free(rs.body);
        if (++p->hops > MAX_HOPS || !url_is_web(next)) { fail(s); return 1; }
        begin(s, next);
        return 0;
    }
    decode(p, (uint8_t *)rs.body, rs.blen);
    free(rs.body);
    s->idx = -1;
    return 1;
}

int img_pump(void) {
    init();
    int changed = 0;
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) changed |= pump_slot(&sl[i]);
    int files = 0;
    for (int q = 0; q < np; q++) {
        if (P[q].state != IMG_QUEUED) continue;
        const char *u = P[q].url;
        if (!strncasecmp(u, "file:", 5)) {                   /* local file: read and decode now, one per call */
            if (files++) continue;
            char path[PATH_MAX];
            url_file_path(u, path, sizeof path);
            struct image im;
            if (image_load(path, &im) == 0) adopt(&P[q], im); else P[q].state = IMG_FAILED;
            changed = 1;
            continue;
        }
        if (!url_is_web(u)) { P[q].state = IMG_FAILED; changed = 1; continue; }
        int f = 0;
        while (f < NSLOT && sl[f].idx >= 0) f++;
        if (f == NSLOT) break;
        sl[f].idx = q;
        P[q].state = IMG_LOADING;
        begin(&sl[f], u);
    }
    return changed;
}

int img_busy(void) {
    for (int i = 0; i < np; i++) if (P[i].state == IMG_QUEUED || P[i].state == IMG_LOADING) return 1;
    return 0;
}

void img_reset(void) {
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) slot_free(&sl[i]);
    for (int i = 0; i < np; i++) { free(P[i].url); free(P[i].im.px); free(P[i].sc); }
    np = 0;
}

const uint32_t *img_pixels(int i, int w, int h, color_t bg) {
    if (i < 0 || i >= np || P[i].state != IMG_READY || w < 1 || h < 1) return 0;
    struct pimg *p = &P[i];
    if (p->sc && p->scw == w && p->sch == h && p->scbg == bg) return p->sc;
    free(p->sc);
    p->sc = malloc((size_t)w * h * 4);
    if (!p->sc) return 0;
    p->scw = w; p->sch = h; p->scbg = bg;
    uint32_t *o = p->sc;
    const struct image *im = &p->im;
    if (w == im->w && h == im->h) memcpy(o, im->px, (size_t)w * h * 4);
    else {
        float s = (float)w / im->w;
        int sh = (int)(im->h * s + .5f);
        if (sh < 1) sh = 1;
        if (sh == h) image_sample(im, o, w, h, w, 0, 0, s, 1);
        else {                                               /* odd aspect ratio: sample at width scale, stretch rows */
            uint32_t *t = malloc((size_t)w * sh * 4);
            if (!t) { free(p->sc); p->sc = 0; return 0; }
            image_sample(im, t, w, sh, w, 0, 0, s, 1);
            for (int y = 0; y < h; y++) memcpy(o + (size_t)y * w, t + (size_t)(y * sh / h) * w, w * 4);
            free(t);
        }
    }
    for (long k = 0, n = (long)w * h; k < n; k++) if (ALPHA(o[k]) != 255) o[k] = blend(bg, o[k]);
    return o;
}
