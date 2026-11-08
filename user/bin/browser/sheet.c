/* sheet.c - external stylesheets (<link rel=stylesheet>, @import). Like img.c: each sheet is fetched once per page by a
 * helper process (NSLOT at a time; file: URLs are read on the spot) and kept until the next page. A sheet that fails,
 * times out or is too big never blocks anything: sheet_get just keeps returning NULL for it. html.c asks while it
 * lays out, in document order; main.c relayouts when sheet_pump() reports an arrival. */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syscall.h>

#define NSLOT 4
#define MAXSHEET 32
#define SHEET_MAX (512u << 10)           /* bytes kept per sheet; the rest is cut off */
#define SHEET_TIMEOUT 8000               /* ms without a byte */
#define MAX_HOPS 4

enum { S_QUEUED, S_LOADING, S_READY, S_FAILED };
struct sheet { char *url, *body; size_t len; int state, hops; };
static struct sheet S[MAXSHEET];
static int ns;
static struct slot { int idx; struct fetch f; char cur[URL_MAX]; } sl[NSLOT];
static int inited;

static void init(void) {
    if (inited) return;
    inited = 1;
    for (int i = 0; i < NSLOT; i++) sl[i].idx = -1;
}

static void slot_free(struct slot *s) { fetch_cancel(&s->f); s->idx = -1; }
static void fail(struct slot *s) { S[s->idx].state = S_FAILED; slot_free(s); }

static void begin(struct slot *s, const char *url) {
    strlcpy(s->cur, url, sizeof s->cur);
    if (fetch_start(&s->f, url, 0) < 0) fail(s);
}

static void read_file(struct sheet *p) {
    char path[PATH_MAX];
    url_file_path(p->url, path, sizeof path);
    int fd = open(path, O_RDONLY);
    char *b = fd >= 0 ? malloc(SHEET_MAX + 1) : 0;
    size_t got = 0;
    while (b && got < SHEET_MAX) {
        ssize_t r = read(fd, b + got, SHEET_MAX - got);
        if (r <= 0) break;
        got += r;
    }
    if (fd >= 0) close(fd);
    if (!b) { p->state = S_FAILED; return; }
    b[got] = 0;
    p->body = b; p->len = got; p->state = S_READY;
}

static int start_queued(void) {                          /* hand queued sheets to free slots */
    int changed = 0;
    for (int q = 0; q < ns; q++) {
        if (S[q].state != S_QUEUED) continue;
        int f = 0;
        while (f < NSLOT && sl[f].idx >= 0) f++;
        if (f == NSLOT) break;
        sl[f].idx = q;
        S[q].state = S_LOADING;
        begin(&sl[f], S[q].url);
        changed |= S[q].state == S_FAILED;
    }
    return changed;
}

const char *sheet_get(const char *url, size_t *len) {
    init();
    int i;
    for (i = 0; i < ns && strcmp(S[i].url, url); i++) ;
    if (i == ns) {
        char *u = ns < MAXSHEET && (!strncasecmp(url, "file:", 5) || url_is_web(url)) ? strdup(url) : 0;
        if (!u) return 0;
        S[ns] = (struct sheet){ .url = u };
        if (!strncasecmp(u, "file:", 5)) read_file(&S[ns]);
        ns++;
        start_queued();
    }
    if (S[i].state != S_READY) return 0;
    *len = S[i].len;
    return S[i].body;
}

static int pump_slot(struct slot *s) {
    struct sheet *p = &S[s->idx];
    int r = fetch_pump(&s->f);
    if (r == 0) {
        if (uptime_ms() - s->f.last > SHEET_TIMEOUT) { fail(s); return 1; }
        return 0;
    }
    if (r < 0) { fail(s); return 1; }
    struct response rs;
    fetch_finish(&s->f, &rs);
    if (rs.err[0] || !rs.body) { free(rs.body); fail(s); return 1; }
    if (rs.status >= 300 && rs.status < 400 && rs.loc[0]) {
        char next[URL_MAX];
        url_resolve(s->cur, rs.loc, next, sizeof next);
        free(rs.body);
        if (++p->hops > MAX_HOPS || !url_is_web(next)) { fail(s); return 1; }
        begin(s, next);
        return s->idx < 0;
    }
    if (rs.status >= 400) { free(rs.body); fail(s); return 1; }
    p->body = rs.body;
    p->len = rs.blen > SHEET_MAX ? SHEET_MAX : rs.blen;
    p->body[p->len] = 0;
    p->state = S_READY;
    s->idx = -1;
    return 1;
}

int sheet_pump(void) {
    init();
    int changed = 0;
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) changed |= pump_slot(&sl[i]);
    return changed | start_queued();
}

int sheet_busy(void) {
    for (int i = 0; i < ns; i++) if (S[i].state == S_QUEUED || S[i].state == S_LOADING) return 1;
    return 0;
}

void sheet_reset(void) {
    init();
    for (int i = 0; i < NSLOT; i++) if (sl[i].idx >= 0) slot_free(&sl[i]);
    for (int i = 0; i < ns; i++) { free(S[i].url); free(S[i].body); }
    ns = 0;
}
