/* vfetch - the Vibe Browser's network helper (parent half: browser/fetch.c). Two ways to run:
 *   vfetch [-X METHOD] [-H "Name: v\nName: v"] [-d BODY | -f BODYFILE] [-r REFERER] URL [POST-BODY]
 *       one HTTP/1.1 request, then exit (Connection: close). Handy from a shell too.
 *   vfetch --worker
 *       a keep-alive worker: reads one request per line on stdin (fields METHOD TAB URL TAB REFERER TAB HEADERS TAB BODY, with
 *       %XX escapes for % TAB CR LF NUL; BODY is "-" for none or "+" and the body), writes each raw response to stdout and keeps
 *       the connection for the next request to the same origin. It stops when stdin closes, or on an error / a response that
 *       only ends when the server hangs up (the parent sees the pipe close).
 * Either way: DNS + connect + TLS + read, the raw response streamed to stdout ("VBERR:kind:text" instead on failure), the
 * standard headers and the cookie jar (cookie.c) added, the response's Set-Cookie lines filed. A binary of its own so a fetch
 * doesn't have to load all of /bin/browser (1.5 MB of QuickJS, fonts and layout code) from disk first. */
#include "../browser/browser.h"
#include "../browser/url.c"
#include "../browser/cookie.c"
#include "../browser/frame.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <syscall.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <tls.h>

static void out_all(const void *p, size_t n) {
    const char *c = p;
    while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; }
}

static int quiet;                /* PRECONNECT: a failure is not reported (the real request will hit it and say so) */

static int fail(const char *kind, const char *msg) {
    char b[200];
    if (quiet) return 1;
    int n = snprintf(b, sizeof b, "VBERR:%s:%s", kind, msg);
    out_all(b, n);
    return 1;
}

/* does the header block hs ("A: b\nC: d") name `name`? */
static int has_hdr(const char *hs, const char *name) {
    size_t nl = strlen(name);
    for (const char *l = hs; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0)
        if (!strncasecmp(l, name, nl) && l[nl] == ':') return 1;
    return 0;
}

/* Accept for what the URL looks like it is: stylesheets and images (only the formats we decode: no webp/avif, so the server picks
 * png/jpeg) and scripts get what a browser asks for them; everything else is a page */
static const char *accept_for(const char *path) {
    int pl = strcspn(path, "?#");
    const char *dot = 0;
    for (int i = 0; i < pl; i++) if (path[i] == '.') dot = path + i + 1; else if (path[i] == '/') dot = 0;
    int el = dot ? (int)(path + pl - dot) : 0;
    #define EXT(s) (el == (int)sizeof(s) - 1 && !strncasecmp(dot, s, el))
    if (EXT("css") || strstr(path, "only=styles")) return "text/css,*/*;q=0.1";
    if (EXT("png") || EXT("jpg") || EXT("jpeg") || EXT("gif") || EXT("bmp") || EXT("svg") || EXT("ico"))
        return "image/png,image/jpeg,image/gif,image/bmp,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
    if (EXT("js") || EXT("mjs") || strstr(path, "only=scripts")) return "*/*";
    #undef EXT
    return "text/html,application/xhtml+xml,application/xml;q=0.9,image/png,image/jpeg,image/gif,image/bmp,*/*;q=0.8";
}

/* script-supplied header lines minus the ones only we may set */
static int add_user_hdrs(char *o, int cap, const char *hs) {
    int n = 0;
    static const char *const ban[] = { "host", "cookie", "content-length", "connection", "accept-encoding", "transfer-encoding", "referer", "origin" };
    while (hs && *hs) {
        int ll = strcspn(hs, "\r\n"), skip = 0;
        for (unsigned i = 0; i < sizeof ban / sizeof *ban; i++) { size_t k = strlen(ban[i]); if (!strncasecmp(hs, ban[i], k) && hs[k] == ':') skip = 1; }
        if (!skip && ll > 2 && memchr(hs, ':', ll) && n + ll + 2 < cap) { memcpy(o + n, hs, ll); n += ll; o[n++] = '\r'; o[n++] = '\n'; }
        hs += ll;
        while (*hs == '\r' || *hs == '\n') hs++;
    }
    return n;
}

/* the response's header block is complete in h[0..n): file its Set-Cookie lines */
static void file_cookies(const char *h, size_t n, const char *host, const char *path, int https) {
    for (size_t i = 0; i < n; ) {
        const char *e = memchr(h + i, '\n', n - i);
        size_t le = e ? (size_t)(e - h) : n;
        if (le - i > 11 && !strncasecmp(h + i, "set-cookie:", 11)) {
            char v[1500];
            size_t l = le - i - 11;
            if (l >= sizeof v) l = sizeof v - 1;
            memcpy(v, h + i + 11, l);
            while (l && (v[l - 1] == '\r' || v[l - 1] == ' ')) l--;
            v[l] = 0;
            cookie_store(host, path, v + strspn(v, " \t"), 0, https);
        }
        i = le + 1;
    }
}

/* ---- the connection (one at a time; a worker keeps it between requests) ---- */
struct req { const char *method, *url, *referer, *hdrs, *body; size_t blen; };
static struct { int fd; struct tls *t; char origin[200]; } cn = { -1, 0, "" };
static long file_blen = -1;
/* timing, logged to /dev/kmsg when /tmp/netlog exists: where a request's time goes */
static uint32_t tm_dns, tm_tcp, tm_tls, tm_ttfb;
static void vlog(const char *host, int reused, uint32_t t0) {
    struct stat st;
    if (stat("/tmp/netlog", &st)) return;
    char b[200];
    int l = snprintf(b, sizeof b, "VFETCH %s reused=%d dns=%u tcp=%u tls=%u ttfb=%u total=%u ms\n", host, reused, tm_dns, tm_tcp, tm_tls, tm_ttfb, uptime_ms() - t0);
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) { write(fd, b, l); close(fd); }
}      /* length of a body read with -f (it may contain NULs) */

static void conn_close(void) {
    if (cn.t) tls_close(cn.t); else if (cn.fd >= 0) close(cn.fd);
    cn.fd = -1; cn.t = 0; cn.origin[0] = 0;
}

/* DNS cache shared by every helper process: /tmp/dnscache, lines "host ip expires" (wall seconds). The kernel resolver costs
 * 20-45 ms even when the host's resolver has the answer and ~900 ms cold, and every page asks for the same few names. */
#define DNS_TTL 300
static int dns_cached(const char *host, uint32_t *ip) {
    int fd = open("/tmp/dnscache", O_RDONLY);
    if (fd < 0) return 0;
    char b[4096];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    long now = time(0);
    size_t hl = strlen(host);
    for (char *l = b; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0)
        if (!strncmp(l, host, hl) && l[hl] == ' ') {
            unsigned long v = strtoul(l + hl + 1, 0, 10);
            char *e = strchr(l + hl + 1, ' ');
            if (e && atol(e + 1) > now) { *ip = (uint32_t)v; return 1; }
        }
    return 0;
}

static void dns_store(const char *host, uint32_t ip) {
    struct stat st;
    int fl = O_WRONLY | O_CREAT | (stat("/tmp/dnscache", &st) == 0 && st.st_size > 3000 ? O_TRUNC : 0);   /* small: start over when full */
    int fd = open("/tmp/dnscache", fl, 0600);
    if (fd < 0) return;
    lseek(fd, 0, SEEK_END);
    char l[200];
    int n = snprintf(l, sizeof l, "%s %lu %ld\n", host, (unsigned long)ip, (long)time(0) + DNS_TTL);
    write(fd, l, n);
    close(fd);
}

static int conn_open(const char *host, int port, int https, const char *origin) {
    uint32_t ip, t0 = uptime_ms();
    int rr = 0;
    if (!inet_aton(host, &ip) && !dns_cached(host, &ip)) {
        rr = -1;
        for (int t = 0; t < 3 && rr < 0; t++) {                /* the in-kernel resolver gives up after ~4.5 s: ask again on timeouts */
            rr = resolve(host, &ip);
            if (rr < 0 && errno != ETIMEDOUT) break;
        }
        if (rr >= 0) dns_store(host, ip);
    }
    if (rr < 0) return fail("dns", host);
    tm_dns = uptime_ms() - t0; t0 = uptime_ms();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { AF_INET, htons(port), ip, { 0 } };
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        return fail("connect", fd < 0 ? strerror(errno) : errno == ECONNREFUSED ? "connection refused" : errno == ETIMEDOUT ? "connection timed out" : "connection refused or timed out");
    tm_tcp = uptime_ms() - t0; t0 = uptime_ms();
    struct tls *t = https ? tls_open(fd, host) : 0;
    if (https && !t) return fail("tls", tls_error());
    tm_tls = uptime_ms() - t0;
    cn.fd = fd; cn.t = t;
    snprintf(cn.origin, sizeof cn.origin, "%s", origin);
    return 0;
}

/* one request/response on the connection. Returns 0 = done and the connection may be reused, 1 = stop (error already
 * reported, or the body ran to the server's close), 2 = nothing came back (stale keep-alive connection: caller may retry) */
static int exchange(const struct req *r, const char *host, int port, const char *path, int https, int keep) {
    int cap = URL_MAX + 12000 + (int)r->blen;
    char *req = malloc(cap), hh[160];
    if (!req) return fail("net", "out of memory");
    if (port == (https ? 443 : 80)) snprintf(hh, sizeof hh, "%s", host); else snprintf(hh, sizeof hh, "%s:%d", host, port);
    const char *method = r->method, *uh = r->hdrs ? r->hdrs : "";
    int rl = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, path, hh);
    if (!has_hdr(uh, "user-agent")) rl += snprintf(req + rl, cap - rl, "User-Agent: " BROWSER_UA "\r\n");
    if (!has_hdr(uh, "accept")) rl += snprintf(req + rl, cap - rl, "Accept: %s\r\n", accept_for(path));
    if (!has_hdr(uh, "accept-language")) rl += snprintf(req + rl, cap - rl, "Accept-Language: en-US,en;q=0.9\r\n");
    rl += snprintf(req + rl, cap - rl, "Accept-Encoding: gzip, deflate\r\nConnection: %s\r\n", keep ? "keep-alive" : "close");   /* never br: no decoder */
    if (r->referer && *r->referer && !(!strncasecmp(r->referer, "https:", 6) && !https)) {      /* not on an https -> http downgrade */
        char ref[URL_MAX];
        snprintf(ref, sizeof ref, "%s", r->referer);
        url_strip_frag(ref);
        rl += snprintf(req + rl, cap - rl, "Referer: %s\r\n", ref);
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) {     /* Origin on unsafe methods: scheme://authority of the referring page */
            const char *a = strstr(ref, "://");
            if (a) rl += snprintf(req + rl, cap - rl, "Origin: %.*s\r\n", (int)(a + 3 + strcspn(a + 3, "/?#") - ref), ref);
        }
    }
    char ck[4096];
    if (cookie_header(host, path, https, 0, ck, sizeof ck)) rl += snprintf(req + rl, cap - rl, "Cookie: %s\r\n", ck);
    rl += add_user_hdrs(req + rl, cap - rl, uh);
    if (r->body || !strcmp(method, "POST") || !strcmp(method, "PUT")) {
        if (!has_hdr(uh, "content-type")) rl += snprintf(req + rl, cap - rl, "Content-Type: application/x-www-form-urlencoded\r\n");
        rl += snprintf(req + rl, cap - rl, "Content-Length: %d\r\n", (int)r->blen);
    }
    rl += snprintf(req + rl, cap - rl, "\r\n");
    if (rl + (int)r->blen > cap) { free(req); return fail("net", "request too large"); }
    if (r->blen) { memcpy(req + rl, r->body, r->blen); rl += r->blen; }
    int bad = cn.t ? tls_write(cn.t, req, rl) != rl : write(cn.fd, req, rl) != rl;
    free(req);
    if (bad) return 2;

    unsigned char buf[8192], hb[16384];
    size_t hn = 0, total = 0;
    uint32_t tsent = uptime_ms();
    int hdone = 0, http11 = 1, close_hdr = 0;
    struct fr fr = { 0 };
    for (;;) {
        struct pollfd pf = { cn.fd, POLLIN, 0 };         /* TLS may hold decrypted bytes the fd doesn't show */
        if (!cn.t && poll(&pf, 1, 25000) <= 0) { if (!total) return 2; break; }   /* parent has its own timeout */
        ssize_t n = cn.t ? tls_read(cn.t, buf, sizeof buf) : read(cn.fd, buf, sizeof buf);
        if (n <= 0) { if (!total) return 2; break; }
        if (!total) tm_ttfb = uptime_ms() - tsent;
        total += n;
        size_t out = n, bstart = hdone ? 0 : n;          /* bytes of this read to pass on; where the body starts in it */
        if (!hdone) {                                    /* watch the header block go by: Set-Cookie, framing */
            size_t k = (size_t)n < sizeof hb - hn ? (size_t)n : sizeof hb - hn, from = hn > 3 ? hn - 3 : 0, before = hn;
            memcpy(hb + hn, buf, k); hn += k;
            for (size_t i = from; i + 3 < hn; i++)
                if (!memcmp(hb + i, "\r\n\r\n", 4)) {
                    char v[64];
                    size_t he = i + 4;
                    hdone = 1; bstart = he - before;
                    file_cookies((char *)hb, i, host, path, https);
                    int status = atoi((char *)hb + 9), chunked = 0;
                    long clen = hdr_get((char *)hb, he, "content-length", v, sizeof v) ? atol(v) : -1;
                    if (hdr_get((char *)hb, he, "transfer-encoding", v, sizeof v))
                        for (char *c = v; *c; c++) if (!strncasecmp(c, "chunked", 7)) chunked = 1;
                    if (hdr_get((char *)hb, he, "connection", v, sizeof v)) close_hdr = !strncasecmp(v, "close", 5);
                    http11 = !memcmp(hb, "HTTP/1.1", 8);
                    fr_init(&fr, !strcmp(r->method, "HEAD"), status, clen, chunked);
                    break;
                }
            if (!hdone && hn == sizeof hb) { hdone = 1; bstart = n; fr.mode = FR_CLOSE; }
        }
        if (hdone) out = bstart + fr_eat(&fr, buf + bstart, n - bstart);
        out_all(buf, out);
        if (fr.done) break;
    }
    if (!fr.done) return 1;                               /* ended by the server closing: nothing more to read here */
    if (!keep || close_hdr || !http11) conn_close();
    return 0;
}

/* returns 0 = go on, 1 = stop */
static int do_request(const struct req *r, int keep) {
    if (!strcmp(r->method, "PRECONNECT")) {                 /* warm a connection for the origin of r->url, say nothing */
        char host[128], path[8], origin[200];
        int port, https = !strncasecmp(r->url, "https:", 6);
        if (url_split_http(r->url, host, sizeof host, &port, path, sizeof path) < 0) return 0;
        snprintf(origin, sizeof origin, "%s://%s:%d", https ? "https" : "http", host, port);
        if (cn.fd >= 0 && !strcmp(cn.origin, origin)) return 0;
        conn_close();
        quiet = 1;
        conn_open(host, port, https, origin);
        quiet = 0;
        return 0;
    }
    char host[128], path[URL_MAX], origin[200];
    int port;
    if (url_split_http(r->url, host, sizeof host, &port, path, sizeof path) < 0) return fail("url", "bad URL");
    int https = !strncasecmp(r->url, "https:", 6);
    snprintf(origin, sizeof origin, "%s://%s:%d", https ? "https" : "http", host, port);
    int retry = !strcmp(r->method, "GET") || !strcmp(r->method, "HEAD");
    uint32_t t0 = uptime_ms();
    for (int attempt = 0; ; attempt++) {
        int reused = 0;
        tm_dns = tm_tcp = tm_tls = tm_ttfb = 0;
        if (cn.fd >= 0 && strcmp(cn.origin, origin)) conn_close();
        if (cn.fd >= 0) {                                 /* idle connection still good? readable now means the server hung up */
            struct pollfd pf = { cn.fd, POLLIN, 0 };
            if (poll(&pf, 1, 0) > 0) conn_close(); else reused = 1;
        }
        if (cn.fd < 0 && conn_open(host, port, https, origin)) return 1;
        int rc = exchange(r, host, port, path, https, keep);
        vlog(host, reused, t0);
        if (rc == 2) {
            conn_close();
            if (reused && retry && attempt == 0) continue;   /* stale keep-alive connection: once more on a fresh one */
            return fail("net", "the server closed the connection without sending anything");
        }
        if (rc == 1) conn_close();
        return rc;
    }
}

/* -f FILE: the request body from a file the parent wrote (binary-safe, big); removed after reading */
static char *slurp(const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    char *b = 0;
    if (fd >= 0 && fstat(fd, &st) == 0 && (b = malloc(st.st_size + 1))) {
        ssize_t n = read(fd, b, st.st_size);
        b[n > 0 ? n : 0] = 0;
        file_blen = n > 0 ? n : 0;
    }
    if (fd >= 0) close(fd);
    unlink(path);
    return b;
}

int fetch_main(const char *url, const char *post) {
    struct req r = { post ? "POST" : "GET", url, 0, 0, post, post ? strlen(post) : 0 };
    return do_request(&r, 0);
}

/* %XX -> byte, in place; returns the new length */
static size_t unesc(char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char h[3] = { s[i + 1], s[i + 2], 0 };
            s[o++] = (char)strtol(h, 0, 16);
            i += 2;
        } else s[o++] = s[i];
    }
    s[o] = 0;
    return o;
}

static int worker(void) {
    char *buf = 0;
    size_t len = 0, cap = 0;
    for (;;) {
        char *nl;
        while (!(nl = memchr(buf, '\n', len))) {
            if (len + 4097 > cap) { char *nb = realloc(buf, cap = cap ? cap * 2 : 8192); if (!nb) return 1; buf = nb; }
            ssize_t n = read(0, buf + len, cap - len - 1);
            if (n <= 0) { conn_close(); return 0; }       /* parent went away */
            len += n;
        }
        *nl = 0;
        char *f[5];
        int k = 0;
        for (char *c = buf; k < 5; ) { f[k++] = c; c = strchr(c, '\t'); if (!c) break; *c++ = 0; }
        if (k < 5) return fail("net", "bad request line");
        for (int i = 0; i < 4; i++) unesc(f[i], strlen(f[i]));
        size_t bl = f[4][0] == '+' ? unesc(f[4] + 1, strlen(f[4] + 1)) : 0;
        struct req r = { f[0], f[1], f[2][0] ? f[2] : 0, f[3][0] ? f[3] : 0, f[4][0] == '+' ? f[4] + 1 : 0, bl };
        if (do_request(&r, 1)) return 1;
        len -= nl + 1 - buf;
        memmove(buf, nl + 1, len);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--worker")) return worker();
    const char *m = 0, *h = 0, *d = 0, *rf = 0;
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-' && argv[i][2] == 0; i += 2)
        switch (argv[i][1]) { case 'X': m = argv[i + 1]; break; case 'H': h = argv[i + 1]; break; case 'd': d = argv[i + 1]; break; case 'f': d = slurp(argv[i + 1]); break; case 'r': rf = argv[i + 1]; break; default: goto usage; }
    if (i >= argc) { usage: fprintf(stderr, "usage: vfetch [-X METHOD] [-H HEADERS] [-d BODY | -f FILE] [-r REFERER] URL [POST-BODY]\n       vfetch --worker\n"); return 2; }
    if (!d && i + 1 < argc) d = argv[i + 1];
    struct req r = { m ? m : d ? "POST" : "GET", argv[i], rf, h, d, d ? (file_blen >= 0 ? (size_t)file_blen : strlen(d)) : 0 };
    return do_request(&r, 0);
}
