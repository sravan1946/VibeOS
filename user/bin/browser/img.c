/* img.c - per-page image cache. <img> elements ask for a URL (img_ref); downloads run in helper processes (NSLOT at a time,
 * each one a `vfetch`), file:// images are read directly. A finished body is decoded with the image library (PNG, JPEG, GIF
 * incl. animation, BMP, PPM) or svg.c (SVG); anything big is shrunk right away to cap memory.
 * Loading is lazy in document order: an image is fetched when it has been drawn (img_pixels is the visibility signal), or
 * sits within LOOKAHEAD images after the last one drawn, or is among the first FIRST; drawn ones go first. So a page with 300
 * pictures fetches what you scroll to, and the visible ones never queue behind the footer.
 * img_pixels() hands out one scaled copy per image, already composited over the page colour, so drawing is a row copy.
 * Animated GIFs advance their frame only while drawn (img_anim_due says when to wake). ponytail: decode runs in the UI process,
 * one image per pump call (~tens of ms for a big JPEG); move it to a helper if that shows up as scroll jank.
 * Also here: srcset / sizes selection (img_pick). */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syscall.h>
#include "../benchutil.h"

#define NSLOT 6
#define MAXIMG 400
#define MAXDIM 2048
#define MAXPX 1800000             /* decoded pixels kept per picture (about 1600x1100) */
#define BUDGET (40L << 20)        /* bytes of decoded pictures kept; the least recently drawn are dropped and decoded again when drawn */
#define FIRST 8
#define LOOKAHEAD 8
#define MAX_HOPS 4
#define IMG_TIMEOUT 20000

int svg_is(const uint8_t *d, size_t n);
int svg_decode(const uint8_t *d, size_t n, struct image *im, int *nw, int *nh, long cur);

struct pimg {
    char *url; int state, hops;
    struct image im;                    /* the pixels (the current frame of an animation); im.w/h can exceed the natural size (SVG) or be smaller (shrunk) */
    int nw, nh;                         /* natural size: what layout uses */
    uint32_t *sc; int scw, sch; color_t scbg;
    uint32_t want;                      /* uptime_ms | 1 when last drawn, 0 = never */
    struct image_anim an; int nf, fr, plays; uint32_t t_next;
    uint8_t hasa;                       /* the scaled copy (sc) has translucent pixels */
    long cur;                           /* currentColor for an inline SVG (-1 = black) */
    uint8_t mode;                       /* loading="": 0 auto (look-ahead window), 1 lazy (only once drawn), 2 eager (now) */
    uint8_t reload;                     /* pixels were dropped (state QUEUED again, size still known): fetched again when drawn, no relayout */
};
static struct pimg *P;
static int np, pcap, want_hi = -1;
static struct slot { int idx; struct fetch f; char cur[URL_MAX]; } sl[NSLOT];
static int slots_init;

static void init(void) {
    if (slots_init) return;
    slots_init = 1;
    for (int i = 0; i < NSLOT; i++) sl[i].idx = -1;
}

int img_ref_ex(const char *url, const char *loading) {
    int i = img_ref(url);
    if (i >= 0 && loading) P[i].mode = !strcasecmp(loading, "lazy") ? 1 : !strcasecmp(loading, "eager") ? 2 : 0;
    return i;
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
    P[np] = (struct pimg){ .url = u, .cur = -1 };
    return np++;
}

const char *img_url(int i) { return i >= 0 && i < np ? P[i].url : ""; }

int img_info(int i, int *w, int *h) {
    if (i < 0 || i >= np) return IMG_FAILED;
    if (P[i].state == IMG_READY || P[i].reload) { *w = P[i].nw; *h = P[i].nh; return IMG_READY; }   /* (a dropped picture still has its size) */
    return P[i].state;
}

static void trim(void);
static long ub = -1;                    /* decoded bytes held, -1 = recompute (see used_bytes) */
/* take a decoded picture; nw x nh is its natural size. Pictures over the pixel budget are shrunk now. */
static void adopt(struct pimg *p, struct image im, int nw, int nh) {
    float s = 1;
    if (im.w > MAXDIM || im.h > MAXDIM) s = (float)MAXDIM / (im.w > im.h ? im.w : im.h);
    if ((double)im.w * im.h * s * s > MAXPX) s = sqrtf((float)MAXPX / ((float)im.w * im.h));
    if (s < 1) {
        int w = (int)(im.w * s), h = (int)(im.h * s);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        uint32_t *px = malloc((size_t)w * h * 4);
        if (px) { image_sample(&im, px, w, h, w, 0, 0, s, 1); free(im.px); im.px = px; im.w = w; im.h = h; }
    }
    p->im = im;
    p->nw = nw; p->nh = nh;
    p->state = IMG_READY;
    ub = -1;
    trim();
}

static void decode1(struct pimg *p, const uint8_t *d, size_t n);
static void decode(struct pimg *p, const uint8_t *d, size_t n) {
    uint32_t t0 = uptime_ms();
    decode1(p, d, n);
    if (uptime_ms() - t0 > 30) blog("BROWSER img %s decode %d bytes -> %dx%d took %u ms", p->url, (int)n, p->im.w, p->im.h, (unsigned)(uptime_ms() - t0));
}
static void decode1(struct pimg *p, const uint8_t *d, size_t n) {
    struct image im;
    int nw = 0, nh = 0;
    if (svg_is(d, n)) {
        if (svg_decode(d, n, &im, &nw, &nh, p->cur) < 0) p->state = IMG_FAILED; else adopt(p, im, nw, nh);
    } else if (n > 6 && !memcmp(d, "GIF8", 4)) {
        struct image_anim a;
        if (image_decode_anim(d, n, &a) < 0 || a.n < 1) { p->state = IMG_FAILED; return; }
        im = (struct image){ a.w, a.h, a.px[0] };
        if (a.n > 1 && (long)a.w * a.h <= MAXPX) {         /* keep every frame; frame 0 shows first */
            p->an = a; p->nf = a.n; p->fr = 0; p->plays = 0; p->t_next = 0;
            p->im = im; p->nw = a.w; p->nh = a.h; p->state = IMG_READY;
            return;
        }
        a.px[0] = 0;
        image_anim_free(&a);
        adopt(p, im, im.w, im.h);
    } else if (image_decode_fit(d, n, &im, 600) < 0) p->state = IMG_FAILED;
    else adopt(p, im, im.w, im.h);
}

int img_add_data(const char *url, const uint8_t *d, size_t n) {
    int i = img_ref(url);
    if (i < 0) return -1;
    P[i].state = IMG_LOADING;
    decode(&P[i], d, n);
    return i;
}

/* <canvas>: the picture under key url ("canvas:<js node id>") becomes these RGBA pixels (a copy). -1 on failure. */
int img_set_rgba(const char *url, const uint8_t *rgba, int w, int h) {
    int i = img_ref(url);
    if (i < 0 || w < 1 || h < 1) return -1;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) return -1;
    for (long k = 0, n = (long)w * h; k < n; k++) px[k] = (uint32_t)rgba[4 * k + 3] << 24 | rgba[4 * k] << 16 | rgba[4 * k + 1] << 8 | rgba[4 * k + 2];
    struct pimg *p = &P[i];
    free(p->im.px); free(p->sc);
    p->sc = 0; p->im = (struct image){ w, h, px }; p->nw = w; p->nh = h; p->state = IMG_READY; p->reload = 0;
    ub = -1;
    return i;
}
int img_find(const char *url) { for (int i = 0; i < np; i++) if (!strcmp(P[i].url, url)) return i; return -1; }

/* an inline <svg> element's markup (n bytes) as an image; color: what currentColor means (0xRRGGBB, -1 = black). Same markup, same index. */
int img_svg_inline(const char *svg, size_t n, long color) {
    uint32_t h = 2166136261u ^ (uint32_t)color;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)svg[i]) * 16777619u;
    char key[40];
    snprintf(key, sizeof key, "svg:%08x:%lu", (unsigned)h, (unsigned long)n);
    int i = img_ref(key);
    if (i < 0) return -1;
    if (P[i].state == IMG_QUEUED) {
        P[i].state = IMG_LOADING;
        P[i].cur = color;
        decode(&P[i], (const uint8_t *)svg, n);
    }
    return i;
}

static long img_bytes(const struct pimg *p) {
    if (p->state != IMG_READY || !p->im.px) return 0;
    return (long)p->im.w * p->im.h * 4 * (p->nf > 1 ? p->nf : 1) + (p->sc ? (long)p->scw * p->sch * 4 : 0);
}
static void evict(struct pimg *p) {
    ub = -1;                                                 /* drop the pixels, keep the size: it is fetched and decoded again if drawn */
    free(p->im.px); free(p->sc);
    p->im.px = 0; p->sc = 0;
    p->state = IMG_QUEUED; p->hops = 0; p->reload = 1;
}
/* over budget: drop the pictures drawn longest ago (never one drawn in the last 2 s, or an animation) */
static void trim(void) {
    uint32_t now = uptime_ms();
    for (;;) {
        long tot = 0;
        struct pimg *v = 0;
        for (int i = 0; i < np; i++) {
            struct pimg *p = &P[i];
            long b = img_bytes(p);
            tot += b;
            if (!b || p->nf > 1 || !strncmp(p->url, "canvas:", 7) || (p->want && now - p->want < 2000)) continue;
            if (!v || (int32_t)(p->want - v->want) < 0) v = p;
        }
        if (tot <= BUDGET || !v) return;
        evict(v);
    }
}

static long used_bytes(void) {
    if (ub < 0) { ub = 0; for (int i = 0; i < np; i++) ub += img_bytes(&P[i]); }
    return ub;
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
    int re = p->reload;
    decode(p, (uint8_t *)rs.body, rs.blen);
    free(rs.body);
    s->idx = -1;
    if (re && p->state == IMG_READY) { p->reload = 0; return 0; }      /* same picture, same size: nothing to lay out again */
    p->reload = 0;
    return 1;
}

static int eligible(int q) {
    if (P[q].reload) return P[q].want && uptime_ms() - P[q].want < 1000;     /* dropped: only while it is on screen */
    if (P[q].want || P[q].mode == 2) return 1;
    if (P[q].mode == 1) return 0;
    return (q < FIRST || q <= want_hi + LOOKAHEAD) && used_bytes() < BUDGET * 3 / 4;   /* prefetch only while there is room: it would just be dropped again */
}

static int hold;
void img_hold(int h) { hold = h; }          /* main.c: the page is scrolling fast; no decoding until it settles */

int img_pump(void) {
    init();
    int changed = 0;
    if (hold) return 0;
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) changed |= pump_slot(&sl[i]);
    int files = 0;
    for (int pass = 0; pass < 2; pass++)                     /* drawn images first, then the look-ahead window */
        for (int q = 0; q < np; q++) {
            if (P[q].state != IMG_QUEUED || !eligible(q) || (pass == 0) != (P[q].want != 0)) continue;
            const char *u = P[q].url;
            if (!strncasecmp(u, "file:", 5)) {                /* local file: read and decode now, one per call */
                if (files++) continue;
                char path[PATH_MAX];
                url_file_path(u, path, sizeof path);
                struct stat st;
                int fd = stat(path, &st) == 0 && st.st_size > 0 && st.st_size < (24 << 20) ? open(path, O_RDONLY) : -1;
                uint8_t *buf = fd >= 0 ? malloc(st.st_size) : 0;
                long got = 0, sz = st.st_size, r;
                while (buf && got < sz && (r = read(fd, buf + got, sz - got)) > 0) got += r;
                if (fd >= 0) close(fd);
                int re = P[q].reload;
                if (buf && got == sz) { P[q].state = IMG_LOADING; decode(&P[q], buf, got); } else P[q].state = IMG_FAILED;
                free(buf);
                P[q].reload = 0;
                changed |= !(re && P[q].state == IMG_READY);
                continue;
            }
            if (!url_is_web(u)) { P[q].state = IMG_FAILED; changed = 1; continue; }
            int f = 0;
            while (f < NSLOT && sl[f].idx >= 0) f++;
            if (f == NSLOT) return changed;
            sl[f].idx = q;
            P[q].state = IMG_LOADING;
            begin(&sl[f], u);
        }
    /* animations */
    uint32_t now = uptime_ms();
    for (int i = 0; i < np; i++) {
        struct pimg *p = &P[i];
        if (p->state != IMG_READY || p->nf < 2 || !p->want || now - p->want > 600) continue;
        if (!p->t_next) { p->t_next = now + (p->an.delay[0] < 20 ? 100 : p->an.delay[0]); continue; }
        if ((int32_t)(now - p->t_next) < 0) continue;
        if (p->fr + 1 == p->nf && p->an.loop && ++p->plays >= p->an.loop) { p->t_next = 0x7fffffff; continue; }   /* played its count: stays on the last frame */
        p->fr = (p->fr + 1) % p->nf;
        p->im.px = p->an.px[p->fr];
        free(p->sc); p->sc = 0;
        int dl = p->an.delay[p->fr];
        p->t_next = now + (dl < 20 ? 100 : dl);
    }
    return changed;
}

/* ms until an animation on screen needs its next frame, -1 = none */
int img_anim_due(void) {
    int best = -1;
    uint32_t now = uptime_ms();
    for (int i = 0; i < np; i++) {
        struct pimg *p = &P[i];
        if (p->state != IMG_READY || p->nf < 2 || !p->want || now - p->want > 600 || p->t_next == 0x7fffffff) continue;
        int d = p->t_next ? (int)(p->t_next - now) : 10;
        if (d < 1) d = 1;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

int img_busy(void) {
    for (int i = 0; i < np; i++) if (P[i].state == IMG_LOADING || (P[i].state == IMG_QUEUED && eligible(i))) return 1;
    return 0;
}

static void free_set(struct pimg *p, int n) {
    for (int i = 0; i < n; i++) {
        free(p[i].url); free(p[i].sc);
        if (p[i].nf > 1) image_anim_free(&p[i].an); else free(p[i].im.px);
    }
    free(p);
}

void img_reset(void) {
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) slot_free(&sl[i]);
    for (int i = 0; i < np; i++) {
        free(P[i].url); free(P[i].sc);
        if (P[i].nf > 1) image_anim_free(&P[i].an); else free(P[i].im.px);
    }
    np = 0; want_hi = -1; ub = -1;
}

/* a background tab keeps its decoded pictures: img_save() takes the cache (downloads in flight go back to the queue), img_restore() puts it back */
struct img_saved { struct pimg *P; int np, pcap, want_hi; };
void *img_save(void) {
    struct img_saved *h = malloc(sizeof *h);
    if (!h) return 0;
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) { P[sl[i].idx].state = IMG_QUEUED; P[sl[i].idx].hops = 0; slot_free(&sl[i]); }
    for (int i = 0; i < np; i++) if (img_bytes(&P[i]) > (256 << 10) && P[i].nf < 2) evict(&P[i]);   /* a parked tab keeps small pictures only */
    *h = (struct img_saved){ P, np, pcap, want_hi };
    P = 0; np = pcap = 0; want_hi = -1; ub = -1;
    return h;
}
void img_restore(void *v) {
    struct img_saved *h = v;
    img_reset(); free(P);
    P = h->P; np = h->np; pcap = h->pcap; want_hi = h->want_hi; ub = -1;
    free(h);
}
void img_drop(void *v) { struct img_saved *h = v; if (h) { free_set(h->P, h->np); free(h); } }

const uint32_t *img_pixels(int i, int w, int h, color_t bg) {
    if (i < 0 || i >= np || w < 1 || h < 1) return 0;
    struct pimg *p = &P[i];
    p->want = uptime_ms() | 1;                               /* drawn: fetch it first, keep it animating */
    if (i > want_hi) want_hi = i;
    if (p->state != IMG_READY) return 0;
    if (p->sc && p->scw == w && p->sch == h && p->scbg == bg) return p->sc;
    free(p->sc); ub = -1;
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
    p->hasa = 0;
    for (long k = 0, n = (long)w * h; k < n; k++) if (ALPHA(o[k]) != 255) { p->hasa = 1; if (bg) o[k] = blend(bg, o[k]); }   /* bg 0: keep the alpha (draw with gfx_blit_alpha) */
    return o;
}
/* the last scaled copy of image i has translucent pixels */
int img_translucent(int i) { return i >= 0 && i < np && P[i].hasa; }

/* ---- srcset / sizes ---- */
/* a CSS length in px at viewport width vw: 12px, 50vw, 3em, 40%, calc(100vw - 2em); advances *sp */
static int css_px(const char **sp, int vw) {
    const char *s = *sp;
    int total = 0, sign = 1, calc = 0;
    while (*s == ' ') s++;
    if (!strncmp(s, "calc(", 5)) { s += 5; calc = 1; }
    for (;;) {
        while (*s == ' ') s++;
        char *e;
        double v = strtod(s, &e);
        if (e == s) break;
        s = e;
        if (!strncmp(s, "vw", 2)) { v = v * vw / 100; s += 2; }
        else if (!strncmp(s, "rem", 3)) { v *= 16; s += 3; }
        else if (!strncmp(s, "em", 2)) { v *= 16; s += 2; }
        else if (*s == '%') { v = v * vw / 100; s++; }
        else if (!strncmp(s, "px", 2)) s += 2;
        total += sign * (int)v;
        while (*s == ' ') s++;
        if (!calc) break;
        if (*s == '+') sign = 1, s++;
        else if (*s == '-') sign = -1, s++;
        else break;
    }
    if (calc && *s == ')') s++;
    *sp = s;
    return total;
}

/* the slot width the sizes="" attribute gives at viewport vw (default 100vw) */
static int slot_width(const char *sizes, int vw) {
    if (!sizes) return vw;
    for (const char *s = sizes; *s; ) {
        while (*s == ' ' || *s == ',') s++;
        const char *e = s;
        int depth = 0;
        for (; *e && (*e != ',' || depth); e++) depth += *e == '(' ? 1 : *e == ')' ? -1 : 0;
        const char *sz = s, *cond = 0;
        if (*s == '(') {                                     /* last ") " at depth 0 ends the media condition */
            depth = 0;
            for (const char *c = s; c < e; c++) {
                depth += *c == '(' ? 1 : *c == ')' ? -1 : 0;
                if (!depth && *c == ')' && c + 1 < e && c[1] == ' ') sz = c + 2;
            }
            if (sz != s) cond = s;
        }
        int ok = 1;
        if (cond) {
            char q[120];
            snprintf(q, sizeof q, "%.*s", (int)(sz - cond - 1 < 119 ? sz - cond - 1 : 119), cond);
            ok = css_media(q) > 0;
        }
        if (ok) { int w = css_px(&sz, vw); return w > 0 ? w : vw; }
        s = e;
    }
    return vw;
}

/* The URL to load for an <img>/<source>: the srcset candidate whose pixel density is the smallest that still reaches 1x
 * (the largest if none does), else src. out gets the (unresolved) URL; returns 0 when there is nothing to load. */
int img_pick(const char *src, const char *srcset, const char *sizes, int vw, char *out, int cap) {
    if (srcset && *srcset) {
        int slot = slot_width(sizes, vw > 0 ? vw : 800);
        char best[URL_MAX] = "", big[URL_MAX] = "";
        double bd = 1e9, gd = 0;
        for (const char *s = srcset; *s; ) {
            while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t') s++;
            const char *u = s;
            while (*s && *s != ' ' && *s != '\n' && *s != '\t') s++;
            int ul = s - u, trail = 0;
            while (ul > 0 && u[ul - 1] == ',') ul--, trail = 1;
            if (!ul) continue;
            double dens = 1;
            if (!trail) {                                    /* descriptor up to the next comma */
                while (*s == ' ') s++;
                char *e;
                double v = strtod(s, &e);
                if (e != s && *e == 'w') dens = slot > 0 ? v / slot : 1;
                else if (e != s && *e == 'x') dens = v;
                while (*s && *s != ',') s++;
            }
            if (dens >= 1 && dens < bd) { bd = dens; snprintf(best, sizeof best, "%.*s", ul, u); }
            if (dens > gd) { gd = dens; snprintf(big, sizeof big, "%.*s", ul, u); }
        }
        const char *r = best[0] ? best : big;
        if (*r) { strlcpy(out, r, cap); return 1; }
    }
    if (src && *src) { strlcpy(out, src, cap); return 1; }
    return 0;
}

/* a <source type=""> we can decode (no type = yes). AVIF is not supported, so a <picture> falls through to a WebP/JPEG/PNG source or its <img>. */
int img_type_ok(const char *type) {
    if (!type || !*type) return 1;
    return !strncasecmp(type, "image/png", 9) || !strncasecmp(type, "image/jpeg", 10) || !strncasecmp(type, "image/jpg", 9) ||
           !strncasecmp(type, "image/gif", 9) || !strncasecmp(type, "image/svg", 9) || !strncasecmp(type, "image/bmp", 9) ||
           !strncasecmp(type, "image/webp", 10);
}
