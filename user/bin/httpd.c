/* httpd [-p port] [dir] - serve dir (default /) over HTTP/1.0: GET/HEAD, directory listings, index.html.
 * Clients are polled together so an idle browser preconnect can't stall the rest; each response is
 * written in one go, then the connection is closed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>

#define MAXC 16
#define IDLE 10                                   /* seconds before a silent client is dropped */

static struct client { int fd, n; time_t seen; char req[2048]; } cl[MAXC];
static const char *root = "/";

static char ob[8192];                             /* output is batched: one write per segment, not per byte */
static int on, ofd = -1;

static void flush(void) {
    for (char *c = ob; on; ) {
        ssize_t w = write(ofd, c, on);
        if (w <= 0) break;
        c += w; on -= w;
    }
    on = 0;
}

static void put(int fd, const void *p, size_t n) {
    if (fd != ofd) { flush(); ofd = fd; }
    for (const char *c = p; n; ) {
        size_t k = n < sizeof ob - on ? n : sizeof ob - on;
        memcpy(ob + on, c, k);
        on += k; c += k; n -= k;
        if (on == (int)sizeof ob) flush();
    }
}

static void puts_fd(int fd, const char *s) { put(fd, s, strlen(s)); }

static const struct { const char *ext, *type; } types[] = {
    { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" }, { "css", "text/css" },
    { "js", "text/javascript" }, { "json", "application/json" }, { "png", "image/png" },
    { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "bmp", "image/bmp" },
    { "svg", "image/svg+xml" }, { "ico", "image/x-icon" }, { "wav", "audio/wav" }, { "pdf", "application/pdf" },
};

/* by extension, else sniff: a NUL in the first block means binary */
static const char *ctype(const char *path, const char *head, int n) {
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (!strcasecmp(dot + 1, types[i].ext)) return types[i].type;
    return memchr(head, 0, n) ? "application/octet-stream" : "text/plain; charset=utf-8";
}

static void header(int fd, int code, const char *msg, const char *type, long len, const char *extra) {
    char h[512];
    int n = snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nServer: VibeOS httpd\r\nConnection: close\r\nContent-Type: %s\r\n",
                     code, msg, type);
    if (len >= 0) n += snprintf(h + n, sizeof h - n, "Content-Length: %ld\r\n", len);
    if (extra) n += snprintf(h + n, sizeof h - n, "%s\r\n", extra);
    n += snprintf(h + n, sizeof h - n, "\r\n");
    put(fd, h, n);
}

static int error(int fd, int code, const char *msg, int head) {
    char b[160];
    int n = snprintf(b, sizeof b, "<!doctype html><title>%d %s</title><h1>%d %s</h1>\n", code, msg, code, msg);
    header(fd, code, msg, "text/html; charset=utf-8", n, 0);
    if (!head) put(fd, b, n);
    return code;
}

static void esc(int fd, const char *s) {
    for (; *s; s++)
        switch (*s) {
        case '&': puts_fd(fd, "&amp;"); break;
        case '<': puts_fd(fd, "&lt;"); break;
        case '>': puts_fd(fd, "&gt;"); break;
        case '"': puts_fd(fd, "&quot;"); break;
        default: put(fd, s, 1);
        }
}

static int listing(int fd, const char *dir, const char *url, int head) {
    DIR *d = opendir(dir);
    if (!d) return error(fd, 403, "Forbidden", head);
    header(fd, 200, "OK", "text/html; charset=utf-8", -1, 0);
    if (head) { closedir(d); return 200; }
    puts_fd(fd, "<!doctype html><meta charset=utf-8><title>");
    esc(fd, url);
    puts_fd(fd, "</title><style>body{font:15px system-ui,sans-serif;margin:2em auto;max-width:46em;padding:0 1em}"
                "td{padding:.15em 1.5em .15em 0}td:last-child{text-align:right;color:#777}a{text-decoration:none}"
                "</style><h2>Index of ");
    esc(fd, url);
    puts_fd(fd, "</h2><table><tr><td><a href=\"../\">../</a><td>");
    struct dirent *e;
    char p[512], sz[32];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int isdir = !stat(p, &st) && S_ISDIR(st.st_mode);
        if (isdir) strcpy(sz, "-");
        else if (st.st_size < 10240) snprintf(sz, sizeof sz, "%u B", (unsigned)st.st_size);
        else snprintf(sz, sizeof sz, "%u KiB", (unsigned)(st.st_size >> 10));
        puts_fd(fd, "<tr><td><a href=\"");
        esc(fd, e->d_name);
        puts_fd(fd, isdir ? "/\">" : "\">");
        esc(fd, e->d_name);
        puts_fd(fd, isdir ? "/</a><td>" : "</a><td>");
        puts_fd(fd, sz);
        puts_fd(fd, "\n");
    }
    closedir(d);
    puts_fd(fd, "</table><p style=\"color:#999\">VibeOS httpd</p>\n");
    return 200;
}

static int hex(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static int serve(int fd, char *req) {
    char *sp = strchr(req, ' '), *url = sp ? sp + 1 : 0, *end = url ? strpbrk(url, " \r\n") : 0;
    if (!sp || !end) return error(fd, 400, "Bad Request", 0);
    *sp = *end = 0;
    int head = !strcmp(req, "HEAD");
    if (!head && strcmp(req, "GET")) return error(fd, 501, "Not Implemented", 0);
    char path[512];
    int n = 0;
    for (char *u = url; *u && *u != '?' && *u != '#' && n < (int)sizeof path - 1; u++) {
        if (*u == '%' && hex(u[1]) >= 0 && hex(u[2]) >= 0) { path[n++] = hex(u[1]) << 4 | hex(u[2]); u += 2; }
        else path[n++] = *u;
    }
    path[n] = 0;
    if (path[0] != '/' || strstr(path, "/../") || (n >= 3 && !strcmp(path + n - 3, "/.."))) return error(fd, 403, "Forbidden", head);
    char full[1024];
    snprintf(full, sizeof full, "%s%s", strcmp(root, "/") ? root : "", path);
    struct stat st;
    if (stat(full, &st) < 0) return error(fd, 404, "Not Found", head);
    if (S_ISDIR(st.st_mode)) {
        if (path[n - 1] != '/') {
            char loc[600];
            snprintf(loc, sizeof loc, "Location: %s/", path);
            header(fd, 301, "Moved Permanently", "text/plain", 0, loc);
            return 301;
        }
        char idx[1100];
        snprintf(idx, sizeof idx, "%sindex.html", full);
        if (stat(idx, &st) < 0) return listing(fd, full, path, head);
        strcpy(full, idx);
    }
    int f = open(full, O_RDONLY);
    if (f < 0) return error(fd, 403, "Forbidden", head);
    char buf[4096];
    int k = read(f, buf, sizeof buf);
    if (k < 0) k = 0;
    header(fd, 200, "OK", ctype(full, buf, k), st.st_size, 0);
    if (!head) do put(fd, buf, k); while ((k = read(f, buf, sizeof buf)) > 0);
    close(f);
    return 200;
}

static void drop(struct client *c) { close(c->fd); c->fd = -1; }

int main(int argc, char **argv) {
    int port = 80;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (argv[i][0] == '-') { fprintf(stderr, "usage: httpd [-p port] [dir]\n"); return 2; }
        else root = argv[i];
    }
    struct stat st;
    if (stat(root, &st) < 0 || !S_ISDIR(st.st_mode)) { fprintf(stderr, "httpd: %s: not a directory\n", root); return 1; }
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { AF_INET, htons(port), 0, { 0 } };
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, MAXC) < 0) {
        fprintf(stderr, "httpd: port %d: %s\n", port, strerror(errno));
        return 1;
    }
    struct netinfo ni;
    char ipb[16];
    netinfo(&ni);
    printf("httpd: serving %s at http://%s:%d/\n", root, inet_ntoa_r(ni.ip, ipb), port);
    for (int i = 0; i < MAXC; i++) cl[i].fd = -1;
    for (;;) {
        struct pollfd p[MAXC + 1];
        int map[MAXC + 1], np = 0, used = 0;
        for (int i = 0; i < MAXC; i++)
            if (cl[i].fd >= 0) { p[np] = (struct pollfd){ cl[i].fd, POLLIN, 0 }; map[np++] = i; used++; }
        if (used < MAXC) { p[np] = (struct pollfd){ lfd, POLLIN, 0 }; map[np++] = -1; }
        if (poll(p, np, 1000) < 0 && errno != EINTR) { perror("httpd: poll"); return 1; }
        time_t now = time(0);
        for (int j = 0; j < np; j++) {
            if (map[j] < 0) {
                if (!p[j].revents) continue;
                int c = accept(lfd, 0, 0);
                for (int i = 0; c >= 0 && i < MAXC; i++)
                    if (cl[i].fd < 0) { cl[i] = (struct client){ c, 0, now, { 0 } }; c = -1; }
                continue;
            }
            struct client *c = &cl[map[j]];
            if (!p[j].revents) { if (now - c->seen > IDLE) drop(c); continue; }
            int k = read(c->fd, c->req + c->n, sizeof c->req - 1 - c->n);
            if (k <= 0) { drop(c); continue; }
            c->n += k; c->req[c->n] = 0; c->seen = now;
            if (!strstr(c->req, "\r\n\r\n") && !strstr(c->req, "\n\n") && c->n < (int)sizeof c->req - 1) continue;
            char line[96];
            snprintf(line, sizeof line, "%.*s", (int)strcspn(c->req, "\r\n"), c->req);
            int code = c->n < (int)sizeof c->req - 1 ? serve(c->fd, c->req) : error(c->fd, 431, "Request Header Fields Too Large", 0);
            flush();
            printf("%d %s\n", code, line);
            drop(c);
        }
    }
}
