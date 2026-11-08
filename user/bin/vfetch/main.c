/* vfetch URL [POST-BODY] - the Vibe Browser's network helper (parent half: browser/fetch.c): HTTP/1.1 GET/POST over plain
 * TCP or TLS (Connection: close). DNS + connect + read, the raw response streamed to stdout ("VBERR:kind:text" instead on
 * failure). A binary of its own so a fetch doesn't have to load all of /bin/browser (1.5 MB of QuickJS, fonts and layout
 * code) from disk first: ~300 ms per page, image, stylesheet or script. */
#include "../browser/browser.h"
#include "../browser/url.c"
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

int fetch_main(const char *url, const char *post) {
    char host[128], path[URL_MAX];
    int port;
    if (url_split_http(url, host, sizeof host, &port, path, sizeof path) < 0) return fail("url", "bad URL");
    uint32_t ip;
    int rr = -1;
    for (int t = 0; t < 3 && rr < 0; t++) {                /* the in-kernel resolver gives up after ~4.5 s: ask again on timeouts */
        rr = resolve(host, &ip);
        if (rr < 0 && errno != ETIMEDOUT) break;
    }
    if (rr < 0) return fail("dns", host);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { AF_INET, htons(port), ip, { 0 } };
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) return fail("connect", fd < 0 ? strerror(errno) : "connection refused or timed out");
    int https = !strncasecmp(url, "https:", 6);
    struct tls *t = https ? tls_open(fd, host) : 0;
    if (https && !t) return fail("tls", tls_error());
    char req[URL_MAX + 400], hh[160];
    if (port == (https ? 443 : 80)) snprintf(hh, sizeof hh, "%s", host); else snprintf(hh, sizeof hh, "%s:%d", host, port);
    int rl = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VibeBrowser/1.0 (VibeOS)\r\n"
                      "Accept: text/html,text/plain;q=0.9,image/png,image/jpeg,image/gif,image/bmp,*/*;q=0.5\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
                      post ? "POST" : "GET", path, hh);
    if (post) rl += snprintf(req + rl, sizeof req - rl, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n", (int)strlen(post));
    rl += snprintf(req + rl, sizeof req - rl, "\r\n");
    if (t ? tls_write(t, req, rl) != rl || (post && tls_write(t, post, strlen(post)) < 0)
          : write(fd, req, rl) != rl || (post && write(fd, post, strlen(post)) != (ssize_t)strlen(post)))
        return fail("send", t ? tls_error() : strerror(errno));
    char buf[4096];
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };            /* TLS may hold decrypted bytes the fd doesn't show */
        if (!t && poll(&pf, 1, 25000) <= 0) return 0;    /* parent has its own timeout */
        ssize_t n = t ? tls_read(t, buf, sizeof buf) : read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out_all(buf, n);
    }
    if (t) tls_close(t); else close(fd);
    return 0;
}


int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vfetch URL [POST-BODY]\n"); return 2; }
    return fetch_main(argv[1], argc > 2 ? argv[2] : 0);
}
