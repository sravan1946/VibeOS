/* wget [-O file] http://host[:port]/path - HTTP/1.0 GET (follows a few redirects; no TLS) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

static char url[512];

/* fetch `url`; body goes to fd out. Returns 0, or 1 + new url in `url` on redirect, or -1 on error */
static int fetch(int out, int *status) {
    char host[128], path[400] = "/", *p = url;
    int port = 80;
    if (strncasecmp(p, "http://", 7)) { fprintf(stderr, "wget: only http:// URLs are supported\n"); return -1; }
    p += 7;
    size_t hl = strcspn(p, "/:");
    if (!hl || hl >= sizeof host) { fprintf(stderr, "wget: bad URL\n"); return -1; }
    memcpy(host, p, hl); host[hl] = 0;
    p += hl;
    if (*p == ':') port = strtol(p + 1, &p, 10);
    if (*p == '/') strlcpy(path, p, sizeof path);

    uint32_t ip;
    if (resolve(host, &ip) < 0) { fprintf(stderr, "wget: %s: %s\n", host, errno == ENOENT ? "unknown host" : strerror(errno)); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { AF_INET, htons(port), ip, { 0 } };
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "wget: connect %s:%d: %s\n", host, port, strerror(errno));
        return -1;
    }
    char req[700];
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: vibeos-wget\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (write(fd, req, rl) != rl) { fprintf(stderr, "wget: send: %s\n", strerror(errno)); close(fd); return -1; }

    char buf[2048], hdr[2048];
    int hn = 0, in_body = 0, rc = 0;
    long total = 0;
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 15000) <= 0) { fprintf(stderr, "wget: timed out\n"); rc = -1; break; }
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { fprintf(stderr, "wget: read: %s\n", strerror(errno)); rc = -1; break; }
        if (n == 0) break;
        char *data = buf;
        if (!in_body) {
            int k = n < (ssize_t)sizeof hdr - 1 - hn ? n : (int)sizeof hdr - 1 - hn;
            memcpy(hdr + hn, buf, k); hn += k; hdr[hn] = 0;
            char *end = strstr(hdr, "\r\n\r\n");
            if (!end) { if (hn >= (int)sizeof hdr - 1) { fprintf(stderr, "wget: header too large\n"); rc = -1; break; } continue; }
            in_body = 1;
            *status = atoi(hdr + 9);
            int hlen = end + 4 - hdr;
            data = buf + (hlen - (hn - k));         /* body bytes that arrived with the header */
            n = n - (hlen - (hn - k));
            *end = 0;
            if (*status >= 300 && *status < 400) {
                for (char *l = strstr(hdr, "\r\n"); l; l = strstr(l + 2, "\r\n"))
                    if (!strncasecmp(l + 2, "Location:", 9)) {
                        char *v = l + 11;
                        size_t len = strcspn(v, "\r\n");
                        if (len >= sizeof url) break;
                        memcpy(url, v, len); url[len] = 0;
                        close(fd);
                        return 1;
                    }
            }
            fprintf(stderr, "%.*s\n", (int)strcspn(hdr, "\r\n"), hdr);
        }
        for (ssize_t o = 0; o < n; ) {
            ssize_t w = write(out, data + o, n - o);
            if (w <= 0) { fprintf(stderr, "wget: write error: %s\n", w < 0 ? strerror(errno) : "short write"); close(fd); return -1; }
            o += w;
        }
        total += n;
    }
    close(fd);
    if (rc == 0) fprintf(stderr, "wget: %ld bytes\n", total);
    return rc;
}

int main(int argc, char **argv) {
    const char *outfile = 0;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-O")) { outfile = argv[i + 1]; i += 2; }
    if (i != argc - 1) { fprintf(stderr, "usage: wget [-O file] http://host[:port]/path\n"); return 2; }
    strlcpy(url, argv[i], sizeof url);
    int out = outfile ? open(outfile, O_WRONLY | O_CREAT | O_TRUNC) : 1;
    if (out < 0) { fprintf(stderr, "wget: %s: %s\n", outfile, strerror(errno)); return 1; }
    int r = 1, status = 0;
    for (int hops = 0; hops < 5 && r == 1; hops++) {
        if (hops) fprintf(stderr, "wget: redirected to %s\n", url);
        r = fetch(out, &status);
    }
    if (outfile) close(out);
    return r == 0 && status < 400 ? 0 : 1;
}
