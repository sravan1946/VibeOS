/* fetch.c - HTTP/1.1 GET (Connection: close). The child half does DNS+connect+read and
 * streams the raw response to stdout ("VBERR:kind:text" instead on failure); the parent half
 * collects it without ever blocking, then parses status, headers and chunked bodies. */
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
#include <sys/socket.h>

#define HDR_MAX 16384

/* ---------- child ---------- */
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

int fetch_main(const char *url) {
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
    char req[URL_MAX + 400], hh[160];
    if (port == 80) snprintf(hh, sizeof hh, "%s", host); else snprintf(hh, sizeof hh, "%s:%d", host, port);
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VibeBrowser/1.0 (VibeOS)\r\n"
                      "Accept: text/html,text/plain;q=0.9,*/*;q=0.5\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", path, hh);
    if (write(fd, req, rl) != rl) return fail("send", strerror(errno));
    char buf[4096];
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 25000) <= 0) return 0;          /* parent has its own timeout */
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out_all(buf, n);
    }
    close(fd);
    return 0;
}

/* ---------- parent ---------- */
static int dying[8], ndying;

void fetch_reap(void) {
    for (int i = 0; i < ndying; i++)
        if (waitpid(dying[i], 0, WNOHANG) != 0) dying[i--] = dying[--ndying];
}

int fetch_start(struct fetch *f, const char *url) {
    memset(f, 0, sizeof *f);
    f->fd = -1; f->clen = -1;
    int p[2];
    if (pipe(p) < 0) return -1;
    char *argv[] = { "/bin/browser", "--fetch", (char *)url, 0 };
    struct spawn_opts o = { { -1, p[1], -1 }, 0, 0 };
    f->pid = spawnve("/bin/browser", argv, environ, &o);
    close(p[1]);
    if (f->pid < 0) { close(p[0]); return -1; }
    f->fd = p[0];
    f->last = uptime_ms();
    return 0;
}

void fetch_cancel(struct fetch *f) {
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    if (f->pid > 0) {
        kill(f->pid, SIGKILL);
        if (waitpid(f->pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = f->pid;
        f->pid = 0;
    }
    free(f->buf);
    f->buf = 0; f->len = f->cap = 0;
}

/* case-insensitive header lookup in hdr[0..n); copies the value (trimmed) */
static int hdr_get(const char *h, size_t n, const char *name, char *out, int cap) {
    size_t nl = strlen(name);
    for (size_t i = 0; i < n; ) {
        const char *e = memchr(h + i, '\n', n - i);
        size_t le = e ? (size_t)(e - h) : n;
        if (le - i > nl && h[i + nl] == ':' && !strncasecmp(h + i, name, nl)) {
            const char *v = h + i + nl + 1;
            const char *end = h + le;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            while (end > v && (end[-1] == '\r' || end[-1] == ' ')) end--;
            snprintf(out, cap, "%.*s", (int)(end - v), v);
            return 1;
        }
        i = le + 1;
    }
    return 0;
}

/* hdr_end: offset of the body, or -1 if the header block isn't complete. *hlen = header bytes */
static long find_hdr_end(const unsigned char *b, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (b[i] == '\n' && b[i + 1] == '\n') return i + 2;
        if (i + 3 < n && b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return i + 4;
    }
    return -1;
}

/* walk chunks of body[0..n): returns 1 if the terminating 0-chunk is present */
static int chunks_done(const unsigned char *b, size_t n) {
    size_t i = 0;
    for (;;) {
        char *e;
        unsigned long sz = strtoul((const char *)b + i, &e, 16);
        if (i >= n || (const unsigned char *)e == b + i) return 0;
        size_t j = (const unsigned char *)e - b;
        while (j < n && b[j] != '\n') j++;
        if (j >= n) return 0;
        j++;
        if (!sz) return 1;
        if (sz > n || j + sz + 2 > n) return 0;
        i = j + sz + 2;
    }
}

int fetch_pump(struct fetch *f) {
    int eof = 0;
    for (int rounds = 0; rounds < 64; rounds++) {
        struct pollfd pf = { f->fd, POLLIN, 0 };
        if (poll(&pf, 1, 0) <= 0) break;
        if (f->len + 4096 + 1 > f->cap) {
            size_t nc = f->cap ? f->cap * 2 : 16384;
            unsigned char *nb = realloc(f->buf, nc);
            if (!nb) return -1;
            f->buf = nb; f->cap = nc;
        }
        ssize_t n = read(f->fd, f->buf + f->len, f->cap - f->len - 1);
        if (n < 0) return -1;
        if (n == 0) { eof = 1; break; }
        f->len += n;
        f->last = uptime_ms();
        if (f->len > MAX_PAGE + HDR_MAX) { f->truncated = 1; eof = 1; break; }
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
        }
    }
    if (f->hdr_end) {
        size_t bl = f->len - f->hdr_end;
        if (f->status == 204 || f->status == 304 || (f->status >= 100 && f->status < 200)) eof = 1;
        else if (!f->chunked && f->clen >= 0 && (long)bl >= f->clen) eof = 1;
        else if (f->chunked && bl >= 5 && !memcmp(f->buf + f->len - 5, "0\r\n\r\n", 5) && chunks_done(f->buf + f->hdr_end, bl)) eof = 1;
    }
    if (eof) { f->buf[f->len] = 0; return 1; }
    return 0;
}

/* Content-Type charset check, and the whole header block -> response */
void fetch_finish(struct fetch *f, struct response *r) {
    memset(r, 0, sizeof *r);
    unsigned char *b = f->buf;
    size_t n = f->len;
    f->buf = 0; f->len = f->cap = 0;
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    if (f->pid > 0) { kill(f->pid, SIGKILL); if (waitpid(f->pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = f->pid; f->pid = 0; }
    if (!b) { snprintf(r->err, sizeof r->err, "net:no data"); return; }
    b[n] = 0;
    if (n >= 6 && !memcmp(b, "VBERR:", 6)) { snprintf(r->err, sizeof r->err, "%s", (char *)b + 6); free(b); return; }
    if (!f->hdr_end || n < 12) {
        snprintf(r->err, sizeof r->err, "net:%s", n ? "the server sent an invalid response" : "the server closed the connection without sending anything");
        free(b);
        return;
    }
    r->status = f->status;
    const char *line = (char *)b + 9;
    if (isdigit((unsigned char)line[0]) && line[3] == ' ') line += 4;
    snprintf(r->text, sizeof r->text, "%.*s", (int)strcspn(line, "\r\n"), line);
    hdr_get((char *)b, f->hdr_end, "content-type", r->ctype, sizeof r->ctype);
    hdr_get((char *)b, f->hdr_end, "location", r->loc, sizeof r->loc);
    char enc[32];
    if (hdr_get((char *)b, f->hdr_end, "content-encoding", enc, sizeof enc) && strcasecmp(enc, "identity")) {
        snprintf(r->err, sizeof r->err, "enc:%s", enc);
        free(b);
        return;
    }
    for (char *c = r->ctype; *c; c++) *c = tolower((unsigned char)*c);
    r->latin1 = strstr(r->ctype, "8859-1") || strstr(r->ctype, "latin1") || strstr(r->ctype, "1252");
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
    if (bl > MAX_PAGE) { bl = MAX_PAGE; r->truncated = 1; }
    memmove(b, body, bl);
    b[bl] = 0;
    r->body = (char *)b;
    r->blen = bl;
}
