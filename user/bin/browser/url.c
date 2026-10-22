/* url.c - just enough URL handling: scheme://authority/path?query#frag */
#include "browser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static int has_scheme(const char *s) {
    if (!isalpha((unsigned char)*s)) return 0;
    while (isalnum((unsigned char)*s) || *s == '+' || *s == '-' || *s == '.') s++;
    return *s == ':';
}

static void put(char *o, int cap, int *n, const char *s, int k) {
    for (int i = 0; i < k && *n < cap - 1; i++) o[(*n)++] = s[i];
    o[*n] = 0;
}

/* append path p[0..n) (starts with '/') with . and .. segments resolved */
static void dotseg(const char *p, int n, char *o, int cap, int *len) {
    int base = *len, i = 0;
    while (i < n) {
        int j = i + 1;
        while (j < n && p[j] != '/') j++;
        const char *s = p + i + 1;
        int sl = j - i - 1;
        if (sl == 1 && s[0] == '.') { if (j >= n) put(o, cap, len, "/", 1); }
        else if (sl == 2 && s[0] == '.' && s[1] == '.') {
            while (*len > base && o[*len - 1] != '/') (*len)--;
            if (*len > base) (*len)--;
            o[*len] = 0;
            if (j >= n) put(o, cap, len, "/", 1);
        } else put(o, cap, len, p + i, j - i);
        i = j;
    }
    if (*len == base) put(o, cap, len, "/", 1);
}

/* assemble scheme + authority + path-and-query (+fragment) */
static void build(const char *scheme, int sl, const char *auth, int al, int has_auth, const char *rest, char *out, int cap) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < sl; i++) { char c = tolower((unsigned char)scheme[i]); put(out, cap, &n, &c, 1); }
    put(out, cap, &n, ":", 1);
    if (has_auth) {
        put(out, cap, &n, "//", 2);
        for (int i = 0; i < al; i++) { char c = tolower((unsigned char)auth[i]); put(out, cap, &n, &c, 1); }
    }
    int pl = strcspn(rest, "?#");
    if (rest[0] == '/') dotseg(rest, pl, out, cap, &n);
    else if (has_auth) put(out, cap, &n, "/", 1);
    else put(out, cap, &n, rest, pl);
    put(out, cap, &n, rest + pl, strlen(rest + pl));
}

/* split an absolute URL; *rest points at the path (or "") */
static void split(const char *u, int *sl, const char **auth, int *al, int *has_auth, const char **rest) {
    const char *c = strchr(u, ':');
    *sl = c - u;
    c++;
    *has_auth = c[0] == '/' && c[1] == '/';
    if (*has_auth) {
        c += 2;
        *auth = c;
        *al = strcspn(c, "/?#");
        *rest = c + *al;
    } else { *auth = c; *al = 0; *rest = c; }
}

void url_resolve(const char *base, const char *ref, char *out, int cap) {
    char r[URL_MAX];
    int n = 0;
    while (isspace((unsigned char)*ref)) ref++;
    for (; *ref && n < URL_MAX - 1; ref++) if (*ref != '\n' && *ref != '\r' && *ref != '\t') r[n++] = *ref;
    while (n && r[n - 1] == ' ') n--;
    r[n] = 0;
    int sl, al, ha;
    const char *auth, *rest;
    if (has_scheme(r)) {
        split(r, &sl, &auth, &al, &ha, &rest);
        build(r, sl, auth, al, ha, rest, out, cap);
        return;
    }
    if (!has_scheme(base)) { strlcpy(out, r, cap); return; }
    split(base, &sl, &auth, &al, &ha, &rest);
    char path[URL_MAX], t[URL_MAX];
    snprintf(path, sizeof path, "%.*s", (int)strcspn(rest, "?#"), rest);
    if (!r[0]) { build(base, sl, auth, al, ha, rest, out, cap); url_strip_frag(out); return; }
    if (r[0] == '#') {
        char b[URL_MAX];
        strlcpy(b, base, sizeof b);
        url_strip_frag(b);
        snprintf(out, cap, "%s%s", b, r);
        return;
    }
    if (r[0] == '/' && r[1] == '/') {
        snprintf(t, sizeof t, "%.*s:%s", sl, base, r);
        url_resolve("about:blank", t, out, cap);
        return;
    }
    if (r[0] == '/') strlcpy(t, r, sizeof t);
    else if (r[0] == '?') snprintf(t, sizeof t, "%s%s", path, r);
    else {
        char *slash = strrchr(path, '/');
        snprintf(t, sizeof t, "%.*s%s", slash ? (int)(slash - path + 1) : 1, slash ? path : "/", r);
    }
    build(base, sl, auth, al, ha, t, out, cap);
}

void url_from_input(const char *in, char *out, int cap) {
    char t[URL_MAX];
    while (isspace((unsigned char)*in)) in++;
    if (!*in) { strlcpy(out, "about:blank", cap); return; }
    if (has_scheme(in) && strncmp(in, "localhost:", 10)) snprintf(t, sizeof t, "%s", in);
    else if (in[0] == '/') snprintf(t, sizeof t, "file://%s", in);
    else if (in[0] == '~') snprintf(t, sizeof t, "file:///home%s", in + 1);
    else snprintf(t, sizeof t, "http://%s", in);
    if (!strncasecmp(t, "file:", 5) && t[5] != '/') snprintf(t, sizeof t, "file:///%s", in + 5);
    url_resolve("about:blank", t, out, cap);
}

int url_is_web(const char *url) { return !strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8); }

int url_split_http(const char *url, char *host, int hcap, int *port, char *path, int pcap) {
    int tls = !strncasecmp(url, "https://", 8);
    if (!tls && strncasecmp(url, "http://", 7)) return -1;
    const char *p = url + (tls ? 8 : 7);
    int al = strcspn(p, "/?#");
    const char *at = memchr(p, '@', al);
    if (at) { al -= at + 1 - p; p = at + 1; }
    int hl = al;
    *port = tls ? 443 : 80;
    const char *col = memchr(p, ':', al);
    if (col) { hl = col - p; *port = atoi(col + 1); if (*port <= 0 || *port > 65535) return -1; }
    if (hl <= 0 || hl >= hcap) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    const char *rest = p + al;
    snprintf(path, pcap, "%s%.*s", *rest == '/' ? "" : "/", (int)strcspn(rest, "#"), rest);
    return 0;
}

void url_unescape(const char *s, char *out, int cap) {
    int n = 0;
    for (; *s && n < cap - 1; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            out[n++] = (char)strtol(h, 0, 16);
            s += 2;
        } else out[n++] = *s;
    }
    out[n] = 0;
}

void url_strip_frag(char *url) { char *h = strchr(url, '#'); if (h) *h = 0; }

/* file:///a%20b/c.png -> /a b/c.png */
void url_file_path(const char *url, char *out, int cap) {
    const char *p = url + 5;
    char raw[URL_MAX];
    if (p[0] == '/' && p[1] == '/') { p += 2; p += strcspn(p, "/"); }
    snprintf(raw, sizeof raw, "%.*s", (int)strcspn(p, "?#"), p);
    url_unescape(raw, out, cap);
    if (!out[0]) strlcpy(out, "/", cap);
}
