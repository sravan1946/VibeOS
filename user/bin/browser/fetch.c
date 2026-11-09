/* fetch.c - HTTP/1.1 client, the parent half: starts /bin/vfetch (user/bin/vfetch, which does DNS + connect + TLS + read, adds
 * the standard headers and cookies, and streams the raw response to stdout, "VBERR:kind:text" instead on failure), collects the
 * output without ever blocking, then parses status, headers and chunked bodies, inflates gzip/deflate, converts the charset to
 * UTF-8 and keeps an in-memory cache of finished GETs. data: URLs and cache hits never start a process: fetch_start fills the
 * buffer with a synthetic response and fetch_pump reports it finished on the first call. */
#include "browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <syscall.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>

#define HDR_MAX 16384
#define NET_MAX (24u << 20)      /* raw bytes accepted per response (MAX_PAGE still caps what a page keeps) */

/* ---------- timing log: with /tmp/netlog present every finished fetch is logged to /dev/kmsg (serial) ---------- */
static int netlog_on = -1;
static uint32_t netlog_t0;
static void netlog(uint32_t start, const char *tag, int status, size_t n, const char *url) {
    if (netlog_on < 0) { struct stat st; netlog_on = stat("/tmp/netlog", &st) == 0; }
    if (!netlog_on) return;
    if (!netlog_t0) netlog_t0 = start;
    char b[400];
    int l = snprintf(b, sizeof b, "NETLOG +%u..+%u ms %s %d %lu %.200s\n", start - netlog_t0, uptime_ms() - netlog_t0, tag, status, (unsigned long)n, url);
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) { write(fd, b, l); close(fd); }
}

/* ---------- cache ---------- */
#define NCACHE 48
#define CACHE_BYTES (16u << 20)
#define CACHE_ONE (3u << 20)
static struct cent { char *url; unsigned char *raw; size_t len; long expires; char etag[100], lm[48]; long cksum; } C[NCACHE];   /* C[0] oldest after compaction; raw = the whole response as received */
static int ncache;
static size_t cbytes;
static char referer[URL_MAX];

void fetch_set_referer(const char *url) { snprintf(referer, sizeof referer, "%s", url ? url : ""); }

static void cache_del(int i) {
    cbytes -= C[i].len;
    free(C[i].url); free(C[i].raw);
    memmove(&C[i], &C[i + 1], (--ncache - i) * sizeof *C);
}

void fetch_cache_drop(const char *url) {
    for (int i = ncache - 1; i >= 0; i--) if (!url || !strcmp(C[i].url, url)) cache_del(i);
}

/* the entry for url, fresh or not (a stale one with an ETag / Last-Modified is revalidated, see fetch_start_req) */
static struct cent *cache_find(const char *url) {
    for (int i = 0; i < ncache; i++) if (!strcmp(C[i].url, url)) return &C[i];
    return 0;
}

/* Vary: Cookie -> the entry is only good for the same cookies; this is a hash of what we'd send now (0 = don't care) */
static long ck_sum(const char *url) {
    char host[128], path[URL_MAX], c[4096];
    int port;
    if (url_split_http(url, host, sizeof host, &port, path, sizeof path) < 0) return 0;
    cookie_header(host, path, !strncasecmp(url, "https:", 6), 0, c, sizeof c);
    long h = 5381;
    for (const char *p = c; *p; p++) h = h * 33 + (unsigned char)*p;
    return h | 1;
}

/* seconds a response stays fresh: max-age if given, else 2 minutes (so back/forward and repeated subresources are free, but a
 * news page isn't stale for long); 0 = always revalidate. -1 = never store (no-store). h = the response's header block */
static long freshness(const char *h, long hl) {
    char cc[200] = "", v[64];
    hdr_get(h, hl, "cache-control", cc, sizeof cc);
    for (char *c = cc; *c; c++) *c = tolower((unsigned char)*c);
    if (strstr(cc, "no-store")) return -1;
    if (strstr(cc, "no-cache")) return 0;
    const char *m = strstr(cc, "max-age=");
    long age = 120;
    if (m) age = atol(m + 8);
    else if (hdr_get(h, hl, "pragma", v, sizeof v) && strstr(v, "no-cache")) return 0;
    return age < 0 ? 0 : age > 3600 ? 3600 : age;
}

/* keep a finished GET. Stale-on-arrival ones (no-cache, max-age=0) are kept too when they carry a validator: the next use sends
 * If-None-Match / If-Modified-Since and a 304 turns it back into the answer. Vary: * is not stored; Vary: Cookie is honoured
 * (Accept-Encoding, User-Agent etc. never change for us). */
static void cache_put(const char *url, const unsigned char *raw, size_t n, long hdr_end) {
    char et[100] = "", lm[48] = "", vary[100] = "";
    if (n > CACHE_ONE) return;
    long age = freshness((const char *)raw, hdr_end);
    hdr_get((const char *)raw, hdr_end, "etag", et, sizeof et);
    hdr_get((const char *)raw, hdr_end, "last-modified", lm, sizeof lm);
    hdr_get((const char *)raw, hdr_end, "vary", vary, sizeof vary);
    for (char *c = vary; *c; c++) *c = tolower((unsigned char)*c);
    if (age < 0 || strchr(vary, '*') || (age == 0 && !et[0] && !lm[0])) return;
    fetch_cache_drop(url);
    while (ncache && (ncache == NCACHE || cbytes + n > CACHE_BYTES)) cache_del(0);
    unsigned char *cp = malloc(n + 1);
    char *u = strdup(url);
    if (!cp || !u) { free(cp); free(u); return; }
    memcpy(cp, raw, n);
    C[ncache] = (struct cent){ u, cp, n, uptime_ms() / 1000 + age };
    snprintf(C[ncache].etag, sizeof C[0].etag, "%s", et);
    snprintf(C[ncache].lm, sizeof C[0].lm, "%s", lm);
    C[ncache].cksum = strstr(vary, "cookie") ? ck_sum(url) : 0;
    ncache++;
    cbytes += n;
}

/* ---------- data: URLs ---------- */
static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1; }

/* data:[<mime>][;base64],<payload> -> a synthetic 200 response in f->buf */
static int data_response(struct fetch *f, const char *url) {
    const char *comma = strchr(url, ',');
    if (!comma) return -1;
    const char *meta = url + 5;
    size_t ml = comma - meta, pl = strlen(comma + 1);
    int b64 = ml >= 7 && !strncasecmp(meta + ml - 7, ";base64", 7);
    if (b64) ml -= 7;
    char head[300];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: %.*s\r\n\r\n", ml ? (int)(ml > 200 ? 200 : ml) : 24, ml ? meta : "text/plain;charset=US-ASCII");
    unsigned char *b = malloc(hl + pl + 1);
    if (!b) return -1;
    memcpy(b, head, hl);
    unsigned char *o = b + hl;
    const char *p = comma + 1;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < pl; i++) {
        int c = (unsigned char)p[i];
        if (c == '%' && i + 2 < pl &&isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
            char h[3] = { p[i + 1], p[i + 2], 0 };
            c = (int)strtol(h, 0, 16);
            i += 2;
        }
        if (!b64) { *o++ = c; continue; }
        int v = b64v(c);
        if (v < 0) continue;                           /* '=', whitespace, anything odd: skip (decoding stops being exact at padding) */
        acc = acc << 6 | v; bits += 6;
        if (bits >= 8) { bits -= 8; *o++ = acc >> bits & 255; }
    }
    f->buf = b; f->len = o - b; f->cap = hl + pl + 1;
    f->ready = 1;
    return 0;
}

/* ---------- parent ---------- */
static int dying[8], ndying;

void fetch_reap(void) {
    for (int i = 0; i < ndying; i++)
        if (waitpid(dying[i], 0, WNOHANG) != 0) dying[i--] = dying[--ndying];
}

/* ---------- helper pool ---------- */
/* Keep-alive: instead of one process per request, up to MAXW `vfetch --worker` processes take requests over a pipe and keep
 * their connection open between them. A request goes to an idle worker that last talked to the same origin, else any idle
 * one (it reconnects), else a new one. A worker is busy from fetch_start until fetch_finish/fetch_cancel; one whose response
 * didn't end cleanly is killed. They end by themselves when the browser exits (their stdin closes). */
#define MAXW 24
#define MAXIDLE 6
static struct wk { int pid, in, out, busy; char origin[200]; } W[MAXW];

static void wk_kill(int i) {
    close(W[i].in); close(W[i].out);
    kill(W[i].pid, SIGKILL);
    if (waitpid(W[i].pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = W[i].pid;
    W[i].pid = 0;
}

static int wk_new(void) {
    int free_slot = -1;
    for (int i = 0; i < MAXW; i++) if (!W[i].pid) { free_slot = i; break; }
    if (free_slot < 0) return -1;
    int pin[2], pout[2];
    if (pipe(pin) < 0) return -1;
    if (pipe(pout) < 0) { close(pin[0]); close(pin[1]); return -1; }
    char *argv[] = { "/bin/vfetch", "--worker", 0 };
    struct spawn_opts o = { { pin[0], pout[1], -1 }, 0, 0 };
    int pid = spawnve("/bin/vfetch", argv, environ, &o);
    close(pin[0]); close(pout[1]);
    if (pid < 0) { close(pin[1]); close(pout[0]); return -1; }
    W[free_slot] = (struct wk){ pid, pin[1], pout[0], 0, "" };
    return free_slot;
}

static int wk_get(const char *origin) {
    int idle = -1, free_slot = -1;
    for (int i = 0; i < MAXW; i++) {
        if (W[i].pid && !W[i].busy && waitpid(W[i].pid, 0, WNOHANG) != 0) { close(W[i].in); close(W[i].out); W[i].pid = 0; }   /* died while idle */
        if (!W[i].pid) { if (free_slot < 0) free_slot = i; continue; }
        if (W[i].busy) continue;
        if (!strcmp(W[i].origin, origin)) return i;
        if (idle < 0) idle = i;
    }
    if (idle >= 0) return idle;
    (void)free_slot;
    return wk_new();
}

/* the fetch is over: its worker goes back to the pool if the response ended cleanly, else it's killed */
static void release(struct fetch *f) {
    if (f->wk >= 0) {
        int i = f->wk, nidle = 0;
        if (f->fr.done && f->hdr_end && !f->truncated) {
            W[i].busy = 0;
            for (int k = 0; k < MAXW; k++) nidle += W[k].pid && !W[k].busy;
            if (nidle > MAXIDLE) wk_kill(i);
        } else wk_kill(i);
        f->wk = -1; f->fd = -1; f->pid = 0;
        return;
    }
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    if (f->pid > 0) { kill(f->pid, SIGKILL); if (waitpid(f->pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = f->pid; f->pid = 0; }
}

/* %XX-escape s[0..n) (for % TAB CR LF NUL) onto o; returns the new end */
static char *esc_add(char *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '%' || c == '\t' || c == '\r' || c == '\n' || c == 0) { *o++ = '%'; *o++ = "0123456789ABCDEF"[c >> 4]; *o++ = "0123456789ABCDEF"[c & 15]; }
        else *o++ = c;
    }
    return o;
}

/* Preconnect: the page's head names the stylesheets and images it will need, so as soon as the HTML is in (before layout, which
 * is what asks for them) open connections to those origins in idle helpers: DNS + TCP + TLS (150-500 ms each) then overlap
 * with layout instead of delaying the first request. Up to 4 per origin (the callers' slots), 8 origins. */
void fetch_preconnect(const char *html, size_t n, const char *base) {
    struct { char origin[200], url[URL_MAX]; int want; } O[8];
    int no = 0;
    if (n > 262144) n = 262144;
    for (size_t i = 0; i < n && no < 8; i++) {
        if (html[i] != '<') continue;
        size_t j = i + 1, nl = 0;
        while (j + nl < n && isalpha((unsigned char)html[j + nl])) nl++;
        int tagok = (nl == 4 && !strncasecmp(html + j, "link", 4)) || (nl == 3 && !strncasecmp(html + j, "img", 3)) ||
                    (nl == 6 && (!strncasecmp(html + j, "script", 6) || !strncasecmp(html + j, "source", 6)));
        if (!tagok) continue;
        size_t e = j;
        while (e < n && html[e] != '>') e++;
        for (size_t a = j + nl; a + 5 < e; a++) {
            int isurl = (!strncasecmp(html + a, " src=", 5) || !strncasecmp(html + a, " href=", 6));
            if (!isurl) continue;
            a += html[a + 1] == 's' || html[a + 1] == 'S' ? 5 : 6;
            char q = html[a] == '"' || html[a] == '\'' ? html[a++] : 0;
            char ref[URL_MAX], abs[URL_MAX];
            size_t k = 0;
            while (a + k < e && k < sizeof ref - 1 && (q ? html[a + k] != q : !isspace((unsigned char)html[a + k]))) { ref[k] = html[a + k]; k++; }
            ref[k] = 0;
            if (!k || !strncasecmp(ref, "data:", 5)) break;
            url_resolve(base, ref, abs, sizeof abs);
            char host[128], path[8];
            int port;
            if ((strncasecmp(abs, "http://", 7) && strncasecmp(abs, "https://", 8)) || url_split_http(abs, host, sizeof host, &port, path, sizeof path) < 0) break;
            char origin[200];
            snprintf(origin, sizeof origin, "%s://%s:%d", strncasecmp(abs, "https:", 6) ? "http" : "https", host, port);
            int o = 0;
            while (o < no && strcmp(O[o].origin, origin)) o++;
            if (o == no && no < 8) { strcpy(O[no].origin, origin); { const char *sl = strchr(abs + 8, '/'); snprintf(O[no].url, sizeof O[0].url, "%.*s", sl ? (int)(sl - abs) : (int)strlen(abs), abs); } O[no].want = 0; no++; }
            if (o < no && O[o].want < 4) O[o].want++;
            break;
        }
        i = e;
    }
    for (int o = 0; o < no; o++) {
        int have = 0;
        for (int i = 0; i < MAXW; i++) have += W[i].pid && !W[i].busy && !strcmp(W[i].origin, O[o].origin);
        for (; have < O[o].want; have++) {
            int w = wk_new();
            if (w < 0) return;
            char ln[URL_MAX + 40];
            int l = snprintf(ln, sizeof ln, "PRECONNECT\t%s/\t\t\t-\n", O[o].url);
            if (write(W[w].in, ln, l) != l) { wk_kill(w); return; }
            snprintf(W[w].origin, sizeof W[w].origin, "%s", O[o].origin);
        }
    }
}

int fetch_start_req(struct fetch *f, const char *url, const char *method, const char *hdrs, const char *body, size_t blen) {
    memset(f, 0, sizeof *f);
    f->fd = -1; f->clen = -1; f->wk = -1;
    if (!method) method = body ? "POST" : "GET";
    f->last = f->t0 = uptime_ms();
    f->head = !strcmp(method, "HEAD");
    if (!strncasecmp(url, "data:", 5)) return data_response(f, url);
    int plain = !strcmp(method, "GET") && !hdrs && !body;       /* the only kind the cache serves */
    if (!hdrs && !strncasecmp(url, "https://fonts.googleapis.com/", 29))   /* modern UAs get woff2 only; this one gets a TTF per weight (no brotli decoder needed) */
        hdrs = "User-Agent: Mozilla/4.0 (compatible; VibeBrowser/1.0)\r\n";
    char cond[300];
    if (plain) {
        struct cent *c = cache_find(url);
        if (c && c->cksum && c->cksum != ck_sum(url)) c = 0;           /* Vary: Cookie and the cookies changed */
        if (c && c->expires > (long)(uptime_ms() / 1000) && (f->buf = malloc(c->len + 1))) { memcpy(f->buf, c->raw, c->len); f->len = c->len; f->cap = c->len + 1; f->ready = 1; return 0; }
        if (c && (c->etag[0] || c->lm[0]) && (f->stale = malloc(c->len + 1))) {      /* stale but revalidatable */
            memcpy(f->stale, c->raw, f->stale_len = c->len);
            snprintf(cond, sizeof cond, "%s%s%s%s%s%s", c->etag[0] ? "If-None-Match: " : "", c->etag, c->etag[0] ? "\r\n" : "",
                     c->lm[0] ? "If-Modified-Since: " : "", c->lm, c->lm[0] ? "\r\n" : "");
            hdrs = cond;
        }
        f->url = strdup(url);
    }
    char origin[200], host[128], path[8];
    int port;
    if (url_split_http(url, host, sizeof host, &port, path, sizeof path) < 0) { free(f->url); f->url = 0; return -1; }
    snprintf(origin, sizeof origin, "%s://%s:%d", strncasecmp(url, "https:", 6) ? "http" : "https", host, port);
    int w = wk_get(origin);
    if (w < 0) { free(f->url); f->url = 0; return -1; }
    /* one request line: METHOD TAB URL TAB REFERER TAB HEADERS TAB BODY ("-" none, "+" then the body), %XX-escaped */
    size_t need = 16 + 3 * (strlen(method) + strlen(url) + strlen(referer) + (hdrs ? strlen(hdrs) : 0) + blen);
    char *ln = malloc(need), *o = ln;
    if (!ln) { wk_kill(w); free(f->url); f->url = 0; return -1; }
    o = esc_add(o, method, strlen(method)); *o++ = '\t';
    o = esc_add(o, url, strlen(url)); *o++ = '\t';
    o = esc_add(o, referer, strlen(referer)); *o++ = '\t';
    o = esc_add(o, hdrs ? hdrs : "", hdrs ? strlen(hdrs) : 0); *o++ = '\t';
    if (body) { *o++ = '+'; o = esc_add(o, body, blen); } else *o++ = '-';
    *o++ = '\n';
    ssize_t wr = 1;
    for (size_t k = 0; k < (size_t)(o - ln) && (wr = write(W[w].in, ln + k, o - ln - k)) > 0; k += wr) ;
    free(ln);
    if (wr <= 0) { wk_kill(w); free(f->url); f->url = 0; return -1; }
    W[w].busy = 1;
    snprintf(W[w].origin, sizeof W[w].origin, "%s", origin);
    f->wk = w; f->pid = W[w].pid; f->fd = W[w].out;
    return 0;
}

int fetch_start(struct fetch *f, const char *url, const char *post) {
    return fetch_start_req(f, url, 0, 0, post, post ? strlen(post) : 0);
}

void fetch_cancel(struct fetch *f) {
    if (f->wk >= 0) { wk_kill(f->wk); f->wk = -1; f->fd = -1; f->pid = 0; }     /* mid-response: not reusable */
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    if (f->pid > 0) {
        kill(f->pid, SIGKILL);
        if (waitpid(f->pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = f->pid;
        f->pid = 0;
    }
    free(f->buf); free(f->url); free(f->stale);
    f->buf = 0; f->url = 0; f->stale = 0; f->len = f->cap = 0;
}

/* hdr_end: offset of the body, or -1 if the header block isn't complete. */
static long find_hdr_end(const unsigned char *b, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (b[i] == '\n' && b[i + 1] == '\n') return i + 2;
        if (i + 3 < n && b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return i + 4;
    }
    return -1;
}

/* a response header of the data read so far (once the headers are complete): 1 = found */
int fetch_hdr(const struct fetch *f, const char *name, char *out, int cap) {
    return f->hdr_end ? hdr_get((const char *)f->buf, f->hdr_end, name, out, cap) : 0;
}

int fetch_pump(struct fetch *f) {
    int eof = f->ready;
    for (int rounds = 0; rounds < 64 && !f->ready; rounds++) {
        struct pollfd pf = { f->fd, POLLIN, 0 };
        if (poll(&pf, 1, 0) <= 0) break;
        if (f->len + 16384 + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 32768;
            unsigned char *nb = realloc(f->buf, nc);
            if (!nb) return -1;
            f->buf = nb; f->cap = nc;
        }
        ssize_t n = read(f->fd, f->buf + f->len, f->cap - f->len - 1 > 32768 ? 32768 : f->cap - f->len - 1);
        if (n < 0) return -1;
        if (n == 0) { eof = 1; break; }
        f->len += n;
        f->last = uptime_ms();
        if (f->len > (size_t)NET_MAX + HDR_MAX) { f->truncated = 1; eof = 1; break; }
    }
    if (!f->hdr_end && f->len >= 5 && !memcmp(f->buf, "HTTP/", 5)) {
        long he = find_hdr_end(f->buf, f->len);
        if (he > 0) {
            char v[64];
            f->hdr_end = he;
            f->buf[f->len] = 0;
            f->status = atoi((char *)f->buf + 9);
            if (hdr_get((char *)f->buf, he, "content-length", v, sizeof v)) f->clen = atol(v);
            if (hdr_get((char *)f->buf, he, "transfer-encoding", v, sizeof v))
                for (char *c = v; *c; c++) if (!strncasecmp(c, "chunked", 7)) f->chunked = 1;
            fr_init(&f->fr, f->head, f->status, f->clen, f->chunked);
            if (f->stale && f->status == 304) {            /* not modified: the cached copy is the answer, fresh for another while */
                long age = freshness((char *)f->stale, find_hdr_end(f->stale, f->stale_len));     /* the stored headers rule (no-cache stays no-cache) */
                struct cent *c = f->url ? cache_find(f->url) : 0;
                if (c) c->expires = uptime_ms() / 1000 + age;
                free(f->buf);
                f->buf = f->stale; f->len = f->stale_len; f->cap = f->len + 1; f->stale = 0;
                f->hdr_end = find_hdr_end(f->buf, f->len);
                f->buf[f->len] = 0;
                f->status = atoi((char *)f->buf + 9);
                f->clen = hdr_get((char *)f->buf, f->hdr_end, "content-length", v, sizeof v) ? atol(v) : -1;
                f->chunked = hdr_get((char *)f->buf, f->hdr_end, "transfer-encoding", v, sizeof v) && strstr(v, "hunked");
                f->fpos = f->len - f->hdr_end;               /* the worker's own (empty) 304 is complete: f->fr stays done */
                free(f->url); f->url = 0;                    /* already cached */
                f->ready = 1;
            }
        }
    }
    if (f->hdr_end) {                                  /* where does the body end? (exactly: a keep-alive helper has no EOF to say so) */
        size_t have = f->len - f->hdr_end;
        if (have > f->fpos) f->fpos += fr_eat(&f->fr, f->buf + f->hdr_end + f->fpos, have - f->fpos);
        if (f->fr.done) eof = 1;
    }
    if (eof) { f->buf[f->len] = 0; return 1; }
    return 0;
}

/* ---------- charset -> UTF-8 ---------- */
enum { E_UTF8, E_1252, E_8859_15, E_1251, E_8859_2, E_KOI8R, E_U16LE, E_U16BE, E_OTHER };

static const uint16_t W1252[32] = { 0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                                    0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178 };
static const uint16_t W1251[64] = { 0x402,0x403,0x201A,0x453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x409,0x2039,0x40A,0x40C,0x40B,0x40F,
                                    0x452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0xFFFD,0x2122,0x459,0x203A,0x45A,0x45C,0x45B,0x45F,
                                    0xA0,0x40E,0x45E,0x408,0xA4,0x490,0xA6,0xA7,0x401,0xA9,0x404,0xAB,0xAC,0xAD,0xAE,0x407,
                                    0xB0,0xB1,0x406,0x456,0x491,0xB5,0xB6,0xB7,0x451,0x2116,0x454,0xBB,0x458,0x405,0x455,0x457 };

static const uint16_t L2[96] = {   /* ISO-8859-2 0xA0..0xFF */
    0xA0,0x104,0x2D8,0x141,0xA4,0x13D,0x15A,0xA7,0xA8,0x160,0x15E,0x164,0x179,0xAD,0x17D,0x17B,
    0xB0,0x105,0x2DB,0x142,0xB4,0x13E,0x15B,0x2C7,0xB8,0x161,0x15F,0x165,0x17A,0x2DD,0x17E,0x17C,
    0x154,0xC1,0xC2,0x102,0xC4,0x139,0x106,0xC7,0x10C,0xC9,0x118,0xCB,0x11A,0xCD,0xCE,0x10E,
    0x110,0x143,0x147,0xD3,0xD4,0x150,0xD6,0xD7,0x158,0x16E,0xDA,0x170,0xDC,0xDD,0x162,0xDF,
    0x155,0xE1,0xE2,0x103,0xE4,0x13A,0x107,0xE7,0x10D,0xE9,0x119,0xEB,0x11B,0xED,0xEE,0x10F,
    0x111,0x144,0x148,0xF3,0xF4,0x151,0xF6,0xF7,0x159,0x16F,0xFA,0x171,0xFC,0xFD,0x163,0x2D9 };
static const uint16_t K8[128] = {  /* KOI8-R 0x80..0xFF: box drawing, then the Cyrillic letters in KOI order */
    0x2500,0x2502,0x250C,0x2510,0x2514,0x2518,0x251C,0x2524,0x252C,0x2534,0x253C,0x2580,0x2584,0x2588,0x258C,0x2590,
    0x2591,0x2592,0x2593,0x2320,0x25A0,0x2219,0x221A,0x2248,0x2264,0x2265,0xA0,0x2321,0xB0,0xB2,0xB7,0xF7,
    0x2550,0x2551,0x2552,0x451,0x2553,0x2554,0x2555,0x2556,0x2557,0x2558,0x2559,0x255A,0x255B,0x255C,0x255D,0x255E,
    0x255F,0x2560,0x2561,0x401,0x2562,0x2563,0x2564,0x2565,0x2566,0x2567,0x2568,0x2569,0x256A,0x256B,0x256C,0xA9,
    0x44E,0x430,0x431,0x446,0x434,0x435,0x444,0x433,0x445,0x438,0x439,0x43A,0x43B,0x43C,0x43D,0x43E,
    0x43F,0x44F,0x440,0x441,0x442,0x443,0x436,0x432,0x44C,0x44B,0x437,0x448,0x44D,0x449,0x447,0x44A,
    0x42E,0x410,0x411,0x426,0x414,0x415,0x424,0x413,0x425,0x418,0x419,0x41A,0x41B,0x41C,0x41D,0x41E,
    0x41F,0x42F,0x420,0x421,0x422,0x423,0x416,0x412,0x42C,0x42B,0x417,0x428,0x42D,0x429,0x427,0x42A };

static int enc_of(const char *cs) {
    static const struct { const char *n; int e; } T[] = {
        { "utf-8", E_UTF8 }, { "utf8", E_UTF8 }, { "us-ascii", E_UTF8 }, { "ascii", E_UTF8 },
        { "iso-8859-1", E_1252 }, { "iso8859-1", E_1252 }, { "latin1", E_1252 }, { "latin-1", E_1252 }, { "l1", E_1252 }, { "windows-1252", E_1252 }, { "cp1252", E_1252 }, { "x-cp1252", E_1252 },
        { "iso-8859-2", E_8859_2 }, { "iso8859-2", E_8859_2 }, { "latin2", E_8859_2 }, { "l2", E_8859_2 }, { "koi8-r", E_KOI8R }, { "koi8", E_KOI8R },
        { "iso-8859-15", E_8859_15 }, { "latin9", E_8859_15 }, { "windows-1251", E_1251 }, { "cp1251", E_1251 }, { "x-cp1251", E_1251 },
        { "utf-16", E_U16LE }, { "utf-16le", E_U16LE }, { "utf-16be", E_U16BE } };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) if (!strcasecmp(cs, T[i].n)) return T[i].e;
    return E_OTHER;           /* gbk, shift_jis, koi8-r, iso-8859-2 ...: legacy_to_utf8 looks them up in charset.bin.gz */
}

/* charset name after "charset=" in s (header value or meta tag) */
static int find_charset(const char *s, size_t n, char *out, int cap) {
    for (size_t i = 0; i + 8 < n; i++) {
        if (strncasecmp(s + i, "charset", 7)) continue;
        size_t j = i + 7;
        while (j < n && s[j] == ' ') j++;
        if (j >= n || s[j] != '=') continue;
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '"' || s[j] == '\'')) j++;
        int k = 0;
        while (j < n && k < cap - 1 && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) out[k++] = s[j++];
        out[k] = 0;
        return k > 0;
    }
    return 0;
}

static int valid_utf8(const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; ) {
        unsigned c = b[i];
        int k = c < 0x80 ? 0 : c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : -1;
        if (k < 0 || i + k >= n + (k == 0)) return 0;
        for (int j = 1; j <= k; j++) if ((b[i + j] & 0xC0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}

static size_t put_utf8(unsigned char *o, unsigned c) {
    if (c < 0x80) { o[0] = c; return 1; }
    if (c < 0x800) { o[0] = 0xC0 | c >> 6; o[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { o[0] = 0xE0 | c >> 12; o[1] = 0x80 | (c >> 6 & 63); o[2] = 0x80 | (c & 63); return 3; }
    o[0] = 0xF0 | c >> 18; o[1] = 0x80 | (c >> 12 & 63); o[2] = 0x80 | (c >> 6 & 63); o[3] = 0x80 | (c & 63);
    return 4;
}

/* Charsets without a built-in table (GBK, Big5, Shift_JIS, EUC-JP/KR, windows-125x, ISO-8859-x, KOI8-U ...): the tables live in
 * /usr/share/vibeos/charset.bin.gz (tools/mkcharset.py), read the first time a page declares one. */
static uint8_t *cs_raw;
static size_t cs_len;
static int cs_tried;

static uint32_t cs_rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const uint8_t *cs_entry(const char *cs) {
    static const struct { const char *a, *n; } AL[] = {
        { "gb2312", "gbk" }, { "gb_2312", "gbk" }, { "gb18030", "gbk" }, { "x-gbk", "gbk" }, { "big5-hkscs", "big5" }, { "x-x-big5", "big5" },
        { "cn-big5", "big5" }, { "sjis", "shift_jis" }, { "shift-jis", "shift_jis" }, { "x-sjis", "shift_jis" }, { "windows-31j", "shift_jis" },
        { "ms932", "shift_jis" }, { "ms_kanji", "shift_jis" }, { "eucjp", "euc-jp" }, { "x-euc-jp", "euc-jp" }, { "ks_c_5601-1987", "euc-kr" },
        { "ks_c_5601", "euc-kr" }, { "windows-949", "euc-kr" }, { "cp949", "euc-kr" }, { "korean", "euc-kr" }, { "latin5", "iso-8859-9" },
        { "tis-620", "windows-874" }, { "iso-8859-11", "windows-874" }, { "cp1250", "windows-1250" }, { "cp1253", "windows-1253" },
        { "cp1254", "windows-1254" }, { "cp1255", "windows-1255" }, { "cp1256", "windows-1256" }, { "cp1257", "windows-1257" },
        { "cp866", "ibm866" }, { "x-mac-roman", "macintosh" }, { "mac", "macintosh" }, { "iso8859-9", "iso-8859-9" }, { "iso8859-5", "iso-8859-5" },
        { "iso8859-7", "iso-8859-7" }, { "iso8859-6", "iso-8859-6" }, { "iso8859-8", "iso-8859-8" }, { "iso8859-4", "iso-8859-4" } };
    if (!cs_tried) {
        cs_tried = 1;
        FILE *f = fopen("/usr/share/vibeos/charset.bin.gz", "rb");
        if (f) {
            size_t n = 0, cap = 1 << 18, r;
            unsigned char *z = malloc(cap);
            while (z && (r = fread(z + n, 1, cap - n, f)) > 0) {
                n += r;
                if (n == cap) { unsigned char *nz = realloc(z, cap *= 2); if (!nz) { free(z); z = 0; } else z = nz; }
            }
            fclose(f);
            if (!z || gunzip_buf(z, n, &cs_raw, &cs_len) < 0 || cs_len < 8 || memcmp(cs_raw, "VCHS", 4)) { free(cs_raw); cs_raw = 0; }
            free(z);
        }
    }
    if (!cs_raw) return 0;
    for (unsigned i = 0; i < sizeof AL / sizeof *AL; i++) if (!strcasecmp(cs, AL[i].a)) { cs = AL[i].n; break; }
    uint32_t cnt = cs_rd32(cs_raw + 4);
    if ((uint64_t)cnt * 28 + 8 > cs_len) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = cs_raw + 8 + i * 28;
        if (strncasecmp((const char *)e, cs, 16)) continue;
        uint32_t off = 8 + cnt * 28 + cs_rd32(e + 24);
        size_t need = e[16] ? (size_t)(e[19] - e[18] + 1) * (e[21] - e[20] + 1) * 2 : 256;
        return off + need <= cs_len ? e : 0;
    }
    return 0;
}

/* b[0..*n) in the named table charset -> new malloc'd UTF-8 (b is freed), or NULL when we have no table for it */
static unsigned char *legacy_to_utf8(unsigned char *b, size_t *n, const char *cs) {
    const uint8_t *e = cs_entry(cs);
    if (!e) return 0;
    const uint16_t *t = (const uint16_t *)(cs_raw + 8 + cs_rd32(cs_raw + 4) * 28 + cs_rd32(e + 24));
    int dbcs = e[16], kana = e[17], l0 = e[18], l1 = e[19], t0 = e[20], t1 = e[21];
    unsigned char *u = malloc(*n * 3 + 4);
    if (!u) return 0;
    size_t o = 0;
    for (size_t i = 0; i < *n; i++) {
        unsigned c = b[i], nx = i + 1 < *n ? b[i + 1] : 0;
        if (c >= 0x80) {
            if (!dbcs) c = t[c - 0x80];
            else if (kana == 1 && c >= 0xA1 && c <= 0xDF) c = 0xFF61 + c - 0xA1;
            else if (kana == 2 && c == 0x8E && nx >= 0xA1 && nx <= 0xDF) { c = 0xFF61 + nx - 0xA1; i++; }
            else if (c >= (unsigned)l0 && c <= (unsigned)l1 && nx >= (unsigned)t0 && nx <= (unsigned)t1) { c = t[(c - l0) * (t1 - t0 + 1) + nx - t0]; i++; if (!c) c = 0xFFFD; }
            else c = 0xFFFD;
        }
        o += put_utf8(u + o, c);
    }
    u[o] = 0;
    free(b);
    *n = o;
    return u;
}

/* b[0..*n) in some charset -> UTF-8 (the buffer may be replaced); the parser only ever sees UTF-8.
 * Order: BOM, Content-Type charset, <meta charset> in the first 2 KB (html), else UTF-8 if it validates, else windows-1252. */
unsigned char *text_to_utf8(unsigned char *b, size_t *n, const char *ctype) {
    int enc = -1, skip = 0;
    char cs[40];
    if (*n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { enc = E_UTF8; skip = 3; }
    else if (*n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { enc = E_U16LE; skip = 2; }
    else if (*n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { enc = E_U16BE; skip = 2; }
    else if (find_charset(ctype, strlen(ctype), cs, sizeof cs)) enc = enc_of(cs);
    else if (strstr(ctype, "html") || !*ctype) {
        char head[2049];
        size_t hn = *n < 2048 ? *n : 2048;
        for (size_t i = 0; i < hn; i++) head[i] = b[i] ? tolower(b[i]) : ' ';
        head[hn] = 0;
        if (strstr(head, "<meta") && find_charset(head, hn, cs, sizeof cs)) enc = enc_of(cs);
    }
    if (enc == E_OTHER) { unsigned char *l = legacy_to_utf8(b, n, cs); return l ? l : b; }
    if (enc < 0) enc = valid_utf8(b, *n) ? E_UTF8 : E_1252;
    if (enc == E_UTF8) { if (skip) { *n -= skip; memmove(b, b + skip, *n); b[*n] = 0; } return b; }
    size_t len = *n - skip, o = 0;
    unsigned char *u = malloc(len * 3 + 4);
    if (!u) return b;
    const unsigned char *s = b + skip;
    if (enc == E_U16LE || enc == E_U16BE) {
        for (size_t i = 0; i + 1 < len; i += 2) {
            unsigned c = enc == E_U16LE ? s[i] | s[i + 1] << 8 : s[i] << 8 | s[i + 1];
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
                unsigned d = enc == E_U16LE ? s[i + 2] | s[i + 3] << 8 : s[i + 2] << 8 | s[i + 3];
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i += 2;
            }
            o += put_utf8(u + o, c);
        }
    } else
        for (size_t i = 0; i < len; i++) {
            unsigned c = s[i];
            if (c >= 0x80) {
                if (enc == E_1251) c = c < 0xC0 ? W1251[c - 0x80] : 0x410 + c - 0xC0;
                else if (enc == E_KOI8R) c = K8[c - 0x80];
                else if (enc == E_8859_2) { if (c >= 0xA0) c = L2[c - 0xA0]; }
                else if (c < 0xA0) c = W1252[c - 0x80];
                else if (enc == E_8859_15) c = c == 0xA4 ? 0x20AC : c == 0xA6 ? 0x160 : c == 0xA8 ? 0x161 : c == 0xB4 ? 0x17D : c == 0xB8 ? 0x17E : c == 0xBC ? 0x152 : c == 0xBD ? 0x153 : c == 0xBE ? 0x178 : c;
            }
            o += put_utf8(u + o, c);
        }
    u[o] = 0;
    free(b);
    *n = o;
    return u;
}

/* ---------- gzip / deflate ---------- */
/* b[0..*n) is a gzip member or a zlib/raw deflate stream: -> new malloc'd plain bytes (NULL = damaged). zlib_inflate (libimage) wants a
 * zlib header, so a gzip body is fed through with a fake one in front of its deflate data. */
static unsigned char *inflate_body(const unsigned char *b, size_t n, int gz, size_t *olen) {
    unsigned char *out = 0;
    if (gz) {
        if (n < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8) return 0;
        size_t p = 10;
        int fl = b[3];
        if (fl & 4) { if (p + 2 > n) return 0; p += 2 + (b[p] | b[p + 1] << 8); }
        if (fl & 8) { while (p < n && b[p]) p++; p++; }
        if (fl & 16) { while (p < n && b[p]) p++; p++; }
        if (fl & 2) p += 2;
        if (p >= n) return 0;
        unsigned char *t = malloc(n - p + 2);
        if (!t) return 0;
        t[0] = 0x78; t[1] = 0x01;                      /* 0x7801 % 31 == 0: a valid zlib header, no dictionary */
        memcpy(t + 2, b + p, n - p);
        if (zlib_inflate(t, n - p + 2, &out, olen, n * 4)) out = 0;
        free(t);
        return out;
    }
    if (!zlib_inflate(b, n, &out, olen, n * 4)) return out;        /* "deflate" is zlib-wrapped by the book ... */
    unsigned char *t = malloc(n + 2);                              /* ... and raw deflate in practice for some servers */
    if (!t) return 0;
    t[0] = 0x78; t[1] = 0x01;
    memcpy(t + 2, b, n);
    if (zlib_inflate(t, n + 2, &out, olen, n * 4)) out = 0;
    free(t);
    return out;
}

/* ---------- finish ---------- */
void fetch_finish(struct fetch *f, struct response *r) {
    memset(r, 0, sizeof *r);
    unsigned char *b = f->buf;
    size_t n = f->len;
    char *url = f->url;
    f->buf = 0; f->url = 0; f->len = f->cap = 0;
    release(f);
    free(f->stale); f->stale = 0;
    netlog(f->t0, f->ready ? "hit" : "net", f->status, n, url ? url : "(other)");
    if (!b) { snprintf(r->err, sizeof r->err, "net:no data"); free(url); return; }
    b[n] = 0;
    if (n >= 6 && !memcmp(b, "VBERR:", 6)) { snprintf(r->err, sizeof r->err, "%s", (char *)b + 6); free(b); free(url); return; }
    if (!f->hdr_end || n < 12) {
        snprintf(r->err, sizeof r->err, "net:%s", n ? "the server sent an invalid response" : "the server closed the connection without sending anything");
        free(b); free(url);
        return;
    }
    r->status = f->status;
    const char *line = (char *)b + 9;
    if (isdigit((unsigned char)line[0]) && line[3] == ' ') line += 4;
    snprintf(r->text, sizeof r->text, "%.*s", (int)strcspn(line, "\r\n"), line);
    {   /* header lines after the status line, whole lines only */
        const char *h = (char *)b + strcspn((char *)b, "\n");
        if (*h) h++;
        size_t hl = f->hdr_end - (h - (char *)b);
        while (hl > 2 && (h[hl - 1] == '\n' || h[hl - 1] == '\r')) hl--;
        if (hl >= sizeof r->hdrs - 2) { hl = sizeof r->hdrs - 3; while (hl && h[hl - 1] != '\n') hl--; }
        memcpy(r->hdrs, h, hl);
        r->hdrs[hl] = 0;
        if (hl && r->hdrs[hl - 1] != '\n') strcat(r->hdrs, "\r\n");
    }
    hdr_get((char *)b, f->hdr_end, "content-type", r->ctype, sizeof r->ctype);
    hdr_get((char *)b, f->hdr_end, "location", r->loc, sizeof r->loc);
    char enc[32] = "";
    hdr_get((char *)b, f->hdr_end, "content-encoding", enc, sizeof enc);
    int gz = !strcasecmp(enc, "gzip") || !strcasecmp(enc, "x-gzip"), df = !strcasecmp(enc, "deflate");
    if (enc[0] && !gz && !df && strcasecmp(enc, "identity")) {
        snprintf(r->err, sizeof r->err, "enc:%s", enc);
        free(b); free(url);
        return;
    }
    if (url && f->status == 200 && !f->truncated) cache_put(url, b, n, f->hdr_end);
    free(url);
    for (char *c = r->ctype; *c; c++) *c = tolower((unsigned char)*c);
    r->truncated = f->truncated;
    unsigned char *body = b + f->hdr_end;
    size_t bl = n - f->hdr_end, o = 0;
    if (f->chunked) {                                  /* de-chunk in place */
        size_t i = 0;
        while (i < bl) {
            char *e;
            unsigned long sz = strtoul((char *)body + i, &e, 16);
            size_t j = (unsigned char *)e - body;
            if (j == i) break;
            while (j < bl && body[j] != '\n') j++;
            j++;
            if (!sz || j >= bl + 1) break;
            if (sz > bl - j) sz = bl - j;
            memmove(body + o, body + j, sz);
            o += sz; i = j + sz + 2;
        }
        bl = o;
    } else if (f->clen >= 0 && (size_t)f->clen < bl) bl = f->clen;
    if (f->head) bl = 0;
    memmove(b, body, bl);
    b[bl] = 0;
    if ((gz || df) && bl) {
        size_t ol = 0;
        unsigned char *p = inflate_body(b, bl, gz, &ol);
        if (!p) {
            if (f->truncated) { snprintf(r->err, sizeof r->err, "net:the page was too large to download completely"); }
            else snprintf(r->err, sizeof r->err, "net:the compressed data (%s) was damaged", enc);
            free(b);
            return;
        }
        free(b);
        b = realloc(p, ol + 1);                        /* shrink; zlib_inflate gave us exactly ol bytes, maybe more room */
        if (!b) b = p;
        bl = ol;
        b[bl] = 0;
    }
    int text = !r->ctype[0] || !strncmp(r->ctype, "text/", 5) || strstr(r->ctype, "html") || strstr(r->ctype, "xml") || strstr(r->ctype, "json") || strstr(r->ctype, "javascript") || strstr(r->ctype, "css");
    if (text) {
        int bin = 0;
        if (!(bl >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))))     /* UTF-16 has NULs by design */
            for (size_t i = 0; i < bl && i < 512; i++) if (!b[i]) bin = 1;         /* an unlabelled blob that isn't text: leave it */
        if (!bin) b = text_to_utf8(b, &bl, r->ctype);
        if (bl > MAX_PAGE) { bl = MAX_PAGE; r->truncated = 1; }
    }
    b[bl] = 0;
    r->body = (char *)b;
    r->blen = bl;
}
