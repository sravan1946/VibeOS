/* frame.c - where an HTTP/1.1 message body ends, shared by the browser (fetch.c) and the vfetch helper, which both have to know
 * exactly so a keep-alive connection can carry the next response: Content-Length, chunked (extensions, trailers) or, for
 * everything else, only the server closing. fr_eat is fed the body bytes as they arrive, in any split. */
#include "browser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* case-insensitive header lookup in h[0..n) ("Name: value" lines); copies the trimmed value */
int hdr_get(const char *h, size_t n, const char *name, char *out, int cap) {
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

void fr_init(struct fr *f, int head, int status, long clen, int chunked) {
    memset(f, 0, sizeof *f);
    if (head || status == 204 || status == 304 || (status >= 100 && status < 200 && status != 101)) f->mode = FR_NONE, f->done = 1;
    else if (chunked) f->mode = FR_CHUNK;
    else if (clen >= 0) { f->mode = FR_LEN; f->rem = clen; f->done = clen == 0; }
    else f->mode = FR_CLOSE;
}

/* feed b[0..n) (body bytes): returns how many belong to this response; f->done once it is complete */
size_t fr_eat(struct fr *f, const unsigned char *b, size_t n) {
    size_t i = 0;
    if (f->done) return 0;
    if (f->mode == FR_CLOSE) return n;
    if (f->mode == FR_LEN) {
        size_t t = (size_t)f->rem < n ? (size_t)f->rem : n;
        f->rem -= t;
        f->done = f->rem == 0;
        return t;
    }
    while (i < n && !f->done) {
        unsigned c = b[i];
        switch (f->st) {
        case 0:                                        /* chunk-size line */
            i++;
            if (c == '\n') {
                if (f->sz == 0) { f->st = 3; f->line = 0; } else { f->rem = f->sz; f->st = 1; }
                f->sz = 0; f->ext = 0;
            } else if (!f->ext && isxdigit(c)) f->sz = f->sz * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            else if (c == ';') f->ext = 1;
            break;
        case 1: {                                      /* chunk data */
            size_t t = (size_t)f->rem < n - i ? (size_t)f->rem : n - i;
            i += t; f->rem -= t;
            if (!f->rem) f->st = 2;
            break;
        }
        case 2:                                        /* CRLF after the data */
            i++;
            if (c == '\n') f->st = 0;
            break;
        default:                                       /* trailers, then an empty line */
            i++;
            if (c == '\n') { if (!f->line) f->done = 1; f->line = 0; }
            else if (c != '\r') f->line++;
        }
    }
    return i;
}
