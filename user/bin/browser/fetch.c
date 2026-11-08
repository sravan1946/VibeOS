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
#include <sys/socket.h>

#define HDR_MAX 16384
#define NET_MAX (24u << 20)      /* raw bytes accepted per response (MAX_PAGE still caps what a page keeps) */

/* ---------- cache ---------- */
#define NCACHE 48
#define CACHE_BYTES (16u << 20)
#define CACHE_ONE (3u << 20)
static struct cent { char *url; unsigned char *raw; size_t len; long expires; } C[NCACHE];   /* C[0] oldest after compaction; raw = the whole response as received */
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

static const struct cent *cache_find(const char *url) {
    long now = uptime_ms() / 1000;
    for (int i = 0; i < ncache; i++)
        if (!strcmp(C[i].url, url)) {
            if (C[i].expires <= now) { cache_del(i); return 0; }
            return &C[i];
        }
    return 0;
}

static int hdr_get(const char *h, size_t n, const char *name, char *out, int cap);

/* keep a finished GET: Cache-Control no-store/no-cache/private-less rules in brief. Freshness: max-age if given, else 2 minutes
 * (so back/forward and repeated subresources are free, but a news page isn't stale for long). ponytail: no revalidation
 * (ETag / If-None-Match), no Vary; a stale entry is simply refetched. */
static void cache_put(const char *url, const unsigned char *raw, size_t n, long hdr_end) {
    char cc[200] = "", v[64];
    hdr_get((const char *)raw, hdr_end, "cache-control", cc, sizeof cc);
    for (char *c = cc; *c; c++) *c = tolower((unsigned char)*c);
    if (strstr(cc, "no-store") || strstr(cc, "no-cache") || n > CACHE_ONE) return;
    long age = 120;
    const char *m = strstr(cc, "max-age=");
    if (m) age = atol(m + 8);
    else if (hdr_get((const char *)raw, hdr_end, "pragma", v, sizeof v) && strstr(v, "no-cache")) return;
    if (age <= 0) return;
    if (age > 3600) age = 3600;
    fetch_cache_drop(url);
    while (ncache && (ncache == NCACHE || cbytes + n > CACHE_BYTES)) cache_del(0);
    unsigned char *cp = malloc(n + 1);
    char *u = strdup(url);
    if (!cp || !u) { free(cp); free(u); return; }
    memcpy(cp, raw, n);
    C[ncache++] = (struct cent){ u, cp, n, uptime_ms() / 1000 + age };
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

int fetch_start_req(struct fetch *f, const char *url, const char *method, const char *hdrs, const char *body, size_t blen) {
    memset(f, 0, sizeof *f);
    f->fd = -1; f->clen = -1;
    if (!method) method = body ? "POST" : "GET";
    f->last = uptime_ms();
    f->head = !strcmp(method, "HEAD");
    if (!strncasecmp(url, "data:", 5)) return data_response(f, url);
    int plain = !strcmp(method, "GET") && !hdrs && !body;       /* the only kind the cache serves */
    if (plain) {
        const struct cent *c = cache_find(url);
        if (c && (f->buf = malloc(c->len + 1))) { memcpy(f->buf, c->raw, c->len); f->len = c->len; f->cap = c->len + 1; f->ready = 1; return 0; }
        f->url = strdup(url);
    }
    char tmp[40] = "";
    if (body && (blen != strlen(body) || blen > 60000)) {      /* NULs or a big body: through a temp file, vfetch -f reads and removes it */
        static int seq;
        snprintf(tmp, sizeof tmp, "/tmp/vf%d.%d", (int)getpid(), seq++);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || write(fd, body, blen) != (ssize_t)blen) { if (fd >= 0) close(fd); free(f->url); f->url = 0; return -1; }
        close(fd);
    }
    int p[2];
    if (pipe(p) < 0) return -1;
    char *argv[12];
    int n = 0;
    argv[n++] = "/bin/vfetch";
    if (strcmp(method, "GET")) { argv[n++] = "-X"; argv[n++] = (char *)method; }
    if (hdrs && *hdrs) { argv[n++] = "-H"; argv[n++] = (char *)hdrs; }
    if (referer[0]) { argv[n++] = "-r"; argv[n++] = referer; }
    if (tmp[0]) { argv[n++] = "-f"; argv[n++] = tmp; }
    else if (body) { argv[n++] = "-d"; argv[n++] = (char *)body; }
    argv[n++] = (char *)url;
    argv[n] = 0;
    struct spawn_opts o = { { -1, p[1], -1 }, 0, 0 };
    f->pid = spawnve("/bin/vfetch", argv, environ, &o);
    close(p[1]);
    if (f->pid < 0) { close(p[0]); free(f->url); f->url = 0; return -1; }
    f->fd = p[0];
    return 0;
}

int fetch_start(struct fetch *f, const char *url, const char *post) {
    return fetch_start_req(f, url, 0, 0, post, post ? strlen(post) : 0);
}

void fetch_cancel(struct fetch *f) {
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    if (f->pid > 0) {
        kill(f->pid, SIGKILL);
        if (waitpid(f->pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = f->pid;
        f->pid = 0;
    }
    free(f->buf); free(f->url);
    f->buf = 0; f->url = 0; f->len = f->cap = 0;
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

/* hdr_end: offset of the body, or -1 if the header block isn't complete. */
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
        while (j < n && b[j] != '\n') j++;      /* skips chunk extensions (";name=value") */
        if (j >= n) return 0;
        j++;
        if (!sz) return 1;
        if (sz > n || j + sz + 2 > n) return 0;
        i = j + sz + 2;
    }
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
        }
    }
    if (f->hdr_end) {
        size_t bl = f->len - f->hdr_end;
        if (f->head || f->status == 204 || f->status == 304 || (f->status >= 100 && f->status < 200 && f->status != 101)) eof = 1;
        else if (!f->chunked && f->clen >= 0 && (long)bl >= f->clen) eof = 1;
        else if (f->chunked && bl >= 5 && !memcmp(f->buf + f->len - 5, "0\r\n\r\n", 5) && chunks_done(f->buf + f->hdr_end, bl)) eof = 1;
    }
    if (eof) { f->buf[f->len] = 0; return 1; }
    return 0;
}

/* ---------- charset -> UTF-8 ---------- */
enum { E_UTF8, E_1252, E_8859_15, E_1251, E_U16LE, E_U16BE, E_OTHER };

static const uint16_t W1252[32] = { 0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                                    0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178 };
static const uint16_t W1251[64] = { 0x402,0x403,0x201A,0x453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x409,0x2039,0x40A,0x40C,0x40B,0x40F,
                                    0x452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0xFFFD,0x2122,0x459,0x203A,0x45A,0x45C,0x45B,0x45F,
                                    0xA0,0x40E,0x45E,0x408,0xA4,0x490,0xA6,0xA7,0x401,0xA9,0x404,0xAB,0xAC,0xAD,0xAE,0x407,
                                    0xB0,0xB1,0x406,0x456,0x491,0xB5,0xB6,0xB7,0x451,0x2116,0x454,0xBB,0x458,0x405,0x455,0x457 };

static int enc_of(const char *cs) {
    static const struct { const char *n; int e; } T[] = {
        { "utf-8", E_UTF8 }, { "utf8", E_UTF8 }, { "us-ascii", E_UTF8 }, { "ascii", E_UTF8 },
        { "iso-8859-1", E_1252 }, { "iso8859-1", E_1252 }, { "latin1", E_1252 }, { "latin-1", E_1252 }, { "l1", E_1252 }, { "windows-1252", E_1252 }, { "cp1252", E_1252 }, { "x-cp1252", E_1252 },
        { "iso-8859-15", E_8859_15 }, { "latin9", E_8859_15 }, { "windows-1251", E_1251 }, { "cp1251", E_1251 }, { "x-cp1251", E_1251 },
        { "utf-16", E_U16LE }, { "utf-16le", E_U16LE }, { "utf-16be", E_U16BE } };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) if (!strcasecmp(cs, T[i].n)) return T[i].e;
    return E_OTHER;           /* gbk, shift_jis, koi8-r, iso-8859-2 ...: left alone (ponytail: add tables when a page needs them) */
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

/* b[0..*n) in some charset -> UTF-8 (the buffer may be replaced); the parser only ever sees UTF-8.
 * Order: BOM, Content-Type charset, <meta charset> in the first 2 KB (html), else UTF-8 if it validates, else windows-1252. */
static unsigned char *to_utf8(unsigned char *b, size_t *n, const char *ctype) {
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
    if (enc == E_OTHER) return b;
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
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    if (f->pid > 0) { kill(f->pid, SIGKILL); if (waitpid(f->pid, 0, WNOHANG) == 0 && ndying < 8) dying[ndying++] = f->pid; f->pid = 0; }
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
        for (size_t i = 0; i < bl && i < 512; i++) if (!b[i]) bin = 1;         /* an unlabelled blob that isn't text: leave it */
        if (!bin) b = to_utf8(b, &bl, r->ctype);
        if (bl > MAX_PAGE) { bl = MAX_PAGE; r->truncated = 1; }
    }
    b[bl] = 0;
    r->body = (char *)b;
    r->blen = bl;
}
