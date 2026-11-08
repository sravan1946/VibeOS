/* vfetch [-X METHOD] [-H "Name: v\nName: v"] [-d BODY] [-r REFERER] URL [POST-BODY] - the Vibe Browser's network helper (parent
 * half: browser/fetch.c): one HTTP/1.1 request over plain TCP or TLS (Connection: close). DNS + connect + read, the raw response
 * streamed to stdout ("VBERR:kind:text" instead on failure). Adds the browser's standard headers and the cookie jar (cookie.c)
 * and files the response's Set-Cookie lines. A binary of its own so a fetch doesn't have to load all of /bin/browser (1.5 MB of
 * QuickJS, fonts and layout code) from disk first: ~300 ms per page, image, stylesheet or script. */
#include "../browser/browser.h"
#include "../browser/url.c"
#include "../browser/cookie.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <syscall.h>
#include <ctype.h>
#include <sys/socket.h>
#include <tls.h>
#include <fcntl.h>
#include <sys/stat.h>

static void out_all(const void *p, size_t n) {
    const char *c = p;
    while (n) { ssize_t w = write(1, c, n); if (w <= 0) _exit(1); c += w; n -= w; }
}

static int fail(const char *kind, const char *msg) {
    char b[200];
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

static long file_blen = -1;      /* length of a body read with -f (it may contain NULs) */

int fetch_main_ex(const char *url, const char *method, const char *uhdrs, const char *post, const char *referer) {
    char host[128], path[URL_MAX];
    int port;
    if (url_split_http(url, host, sizeof host, &port, path, sizeof path) < 0) return fail("url", "bad URL");
    uint32_t ip;
    int rr = 0;
    if (!inet_aton(host, &ip)) {
        rr = -1;
        for (int t = 0; t < 3 && rr < 0; t++) {                /* the in-kernel resolver gives up after ~4.5 s: ask again on timeouts */
            rr = resolve(host, &ip);
            if (rr < 0 && errno != ETIMEDOUT) break;
        }
    }
    if (rr < 0) return fail("dns", host);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { AF_INET, htons(port), ip, { 0 } };
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        return fail("connect", fd < 0 ? strerror(errno) : errno == ECONNREFUSED ? "connection refused" : errno == ETIMEDOUT ? "connection timed out" : "connection refused or timed out");
    int https = !strncasecmp(url, "https:", 6);
    struct tls *t = https ? tls_open(fd, host) : 0;
    if (https && !t) return fail("tls", tls_error());
    int cap = URL_MAX + 12000;
    char *req = malloc(cap), hh[160];
    if (!req) return fail("net", "out of memory");
    if (port == (https ? 443 : 80)) snprintf(hh, sizeof hh, "%s", host); else snprintf(hh, sizeof hh, "%s:%d", host, port);
    if (!method) method = post ? "POST" : "GET";
    if (!uhdrs) uhdrs = "";
    int rl = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, path, hh);
    if (!has_hdr(uhdrs, "user-agent")) rl += snprintf(req + rl, cap - rl, "User-Agent: " BROWSER_UA "\r\n");
    if (!has_hdr(uhdrs, "accept")) rl += snprintf(req + rl, cap - rl, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/png,image/jpeg,image/gif,image/bmp,*/*;q=0.8\r\n");
    if (!has_hdr(uhdrs, "accept-language")) rl += snprintf(req + rl, cap - rl, "Accept-Language: en-US,en;q=0.9\r\n");
    rl += snprintf(req + rl, cap - rl, "Accept-Encoding: gzip, deflate\r\nConnection: close\r\n");   /* never br: no decoder */
    if (referer && *referer && !(!strncasecmp(referer, "https:", 6) && !https)) {         /* not on an https -> http downgrade */
        char ref[URL_MAX];
        snprintf(ref, sizeof ref, "%s", referer);
        url_strip_frag(ref);
        rl += snprintf(req + rl, cap - rl, "Referer: %s\r\n", ref);
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) {     /* Origin on unsafe methods: scheme://authority of the referring page */
            const char *a = strstr(ref, "://");
            if (a) rl += snprintf(req + rl, cap - rl, "Origin: %.*s\r\n", (int)(a + 3 + strcspn(a + 3, "/?#") - ref), ref);
        }
    }
    char ck[4096];
    if (cookie_header(host, path, https, 0, ck, sizeof ck)) rl += snprintf(req + rl, cap - rl, "Cookie: %s\r\n", ck);
    rl += add_user_hdrs(req + rl, cap - rl, uhdrs);
    int pl = post ? (file_blen >= 0 ? (int)file_blen : (int)strlen(post)) : 0;
    if (post || !strcmp(method, "POST") || !strcmp(method, "PUT")) {
        if (!has_hdr(uhdrs, "content-type")) rl += snprintf(req + rl, cap - rl, "Content-Type: application/x-www-form-urlencoded\r\n");
        rl += snprintf(req + rl, cap - rl, "Content-Length: %d\r\n", pl);
    }
    rl += snprintf(req + rl, cap - rl, "\r\n");
    if (rl + pl > cap) return fail("net", "request too large");
    if (pl) { memcpy(req + rl, post, pl); rl += pl; }
    if (t ? tls_write(t, req, rl) != rl : write(fd, req, rl) != rl) return fail("send", t ? tls_error() : strerror(errno));
    free(req);
    char buf[4096], hb[16384];
    size_t hn = 0;
    int hdone = 0;
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };            /* TLS may hold decrypted bytes the fd doesn't show */
        if (!t && poll(&pf, 1, 25000) <= 0) break;       /* parent has its own timeout */
        ssize_t n = t ? tls_read(t, buf, sizeof buf) : read(fd, buf, sizeof buf);
        if (n <= 0) break;
        if (!hdone) {                                    /* watch the header block go by for Set-Cookie */
            size_t k = n < (ssize_t)(sizeof hb - hn) ? (size_t)n : sizeof hb - hn;
            size_t from = hn > 3 ? hn - 3 : 0;
            memcpy(hb + hn, buf, k); hn += k;
            for (size_t i = from; i + 3 < hn; i++)
                if (!memcmp(hb + i, "\r\n\r\n", 4)) { hdone = 1; file_cookies(hb, i, host, path, https); break; }
            if (hn == sizeof hb) hdone = 1;
        }
        out_all(buf, n);
    }
    if (t) tls_close(t); else close(fd);
    return 0;
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

int fetch_main(const char *url, const char *post) { return fetch_main_ex(url, 0, 0, post, 0); }

int main(int argc, char **argv) {
    const char *m = 0, *h = 0, *d = 0, *r = 0;
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-' && argv[i][2] == 0; i += 2)
        switch (argv[i][1]) { case 'X': m = argv[i + 1]; break; case 'H': h = argv[i + 1]; break; case 'd': d = argv[i + 1]; break; case 'f': d = slurp(argv[i + 1]); break; case 'r': r = argv[i + 1]; break; default: goto usage; }
    if (i >= argc) { usage: fprintf(stderr, "usage: vfetch [-X METHOD] [-H HEADERS] [-d BODY] [-r REFERER] URL [POST-BODY]\n"); return 2; }
    return fetch_main_ex(argv[i], m, h, d ? d : i + 1 < argc ? argv[i + 1] : 0, r);
}
