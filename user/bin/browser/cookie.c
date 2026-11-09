/* cookie.c - the cookie jar. Every fetch is a separate /bin/vfetch process, so the jar lives in a file all of them share:
 * ~/.config/browser/cookies, one cookie per line: domain TAB flags TAB path TAB expires TAB name TAB value
 * (flags: h = host-only, s = Secure, o = HttpOnly; expires = unix time, 0 = session cookie). Each call loads the file,
 * and a change rewrites it through a temp file + rename. ponytail: two processes saving in the same millisecond can lose
 * one update; a lock file if that ever shows. SameSite is parsed away, not enforced (every request here is top-level-ish). */
#include "browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

#define MAXCK 300
struct ck { char dom[80], path[128], name[64], val[2048]; char host, secure, httponly; long exp; };

static void jar_path(char *o, int cap, const char *suffix) {
    const char *h = getenv("HOME");
    snprintf(o, cap, "%s/.config/browser/cookies%s", h && *h ? h : "/home", suffix);
}

static int jar_load(struct ck **out) {
    char p[160];
    jar_path(p, sizeof p, "");
    struct ck *j = calloc(MAXCK + 1, sizeof *j);
    int n = 0, fd = open(p, O_RDONLY);
    *out = j;
    if (!j || fd < 0) { if (fd >= 0) close(fd); return 0; }
    struct stat st;
    char *b = fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (4 << 20) ? malloc(st.st_size + 1) : 0;
    ssize_t got = b ? read(fd, b, st.st_size) : 0;
    close(fd);
    if (got > 0) {
        b[got] = 0;
        for (char *l = b, *nx; l && *l && n < MAXCK; l = nx) {
            nx = strchr(l, '\n');
            if (nx) *nx++ = 0;
            char *f[6];
            int k = 0;
            for (char *c = l; k < 6; ) { f[k++] = c; c = strchr(c, '\t'); if (!c) break; *c++ = 0; }
            if (k < 6) continue;
            struct ck *c = &j[n++];
            snprintf(c->dom, sizeof c->dom, "%s", f[0]);
            c->host = !!strchr(f[1], 'h'); c->secure = !!strchr(f[1], 's'); c->httponly = !!strchr(f[1], 'o');
            snprintf(c->path, sizeof c->path, "%s", f[2]);
            c->exp = atol(f[3]);
            snprintf(c->name, sizeof c->name, "%s", f[4]);
            snprintf(c->val, sizeof c->val, "%s", f[5]);
        }
    }
    free(b);
    return n;
}

static void jar_save(const struct ck *j, int n) {
    char p[160], t[176], d[160];
    jar_path(p, sizeof p, "");
    snprintf(t, sizeof t, "%s.%d", p, (int)getpid());
    snprintf(d, sizeof d, "%s", p);
    for (char *s = d + 1; (s = strchr(s, '/')); *s++ = '/') { *s = 0; mkdir(d, 0755); *s = '/'; }   /* mkdir -p of the parents */
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    char line[2600];
    for (int i = 0; i < n; i++) {
        int l = snprintf(line, sizeof line, "%s\t%s%s%s\t%s\t%ld\t%s\t%s\n", j[i].dom, j[i].host ? "h" : "", j[i].secure ? "s" : "", j[i].httponly ? "o" : "",
                         j[i].path, j[i].exp, j[i].name, j[i].val);
        if (l > 0 && write(fd, line, l) != l) break;
    }
    close(fd);
    rename(t, p);
}

/* "Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 ...", "Sun Nov  6 08:49:37 1994": unix time or -1 */
static long http_date(const char *s) {
    static const char M[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int d = 0, mo = 0, y = 0, hh = 0, mi = 0, ss = 0;
    while (*s) {
        while (*s && !isalnum((unsigned char)*s)) s++;
        const char *t = s;
        while (isalnum((unsigned char)*t) || *t == ':') t++;
        if (t == s) break;
        char tok[16];
        snprintf(tok, sizeof tok, "%.*s", (int)(t - s), s);
        if (strchr(tok, ':')) sscanf(tok, "%d:%d:%d", &hh, &mi, &ss);
        else if (isdigit((unsigned char)tok[0])) {
            int v = atoi(tok);
            if (!d && strlen(tok) <= 2) d = v;
            else { y = v; if (strlen(tok) <= 2) y += v < 70 ? 2000 : 1900; }
        } else if (!mo) for (int i = 0; i < 12; i++) if (!strncasecmp(tok, M + i * 3, 3)) mo = i + 1;
        s = t;
    }
    if (!d || !mo || !y) return -1;
    y -= mo <= 2;                                      /* days from civil (Howard Hinnant) */
    long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400L + hh * 3600L + mi * 60 + ss;
}

static int dom_match(const struct ck *c, const char *host) {
    if (!strcasecmp(host, c->dom)) return 1;
    size_t hl = strlen(host), dl = strlen(c->dom);
    return !c->host && hl > dl && host[hl - dl - 1] == '.' && !strcasecmp(host + hl - dl, c->dom);
}

static int path_match(const char *cp, const char *rp) {
    size_t n = strlen(cp);
    if (strncmp(cp, rp, n)) return 0;
    return cp[n - 1] == '/' || rp[n] == 0 || rp[n] == '/' || rp[n] == '?';
}

/* browser start: session cookies don't outlive the browser */
void cookie_init(void) {
    struct ck *j;
    int n = jar_load(&j), o = 0;
    for (int i = 0; i < n; i++) if (j[i].exp) j[o++] = j[i];
    if (o != n) jar_save(j, o);
    free(j);
}

void cookie_clear(void) { jar_save(0, 0); }

/* "a=1; b=2" for a request to host + path (path may carry ?query); from_js hides HttpOnly ones. Returns the length */
int cookie_header(const char *host, const char *path, int secure, int from_js, char *out, int cap) {
    struct ck *j;
    int n = jar_load(&j), len = 0, order[MAXCK], k = 0;
    long now = time(0);
    out[0] = 0;
    for (int i = 0; i < n; i++)
        if ((!j[i].exp || j[i].exp > now) && dom_match(&j[i], host) && path_match(j[i].path, path) && (secure || !j[i].secure) && !(from_js && j[i].httponly))
            order[k++] = i;
    for (int a = 1; a < k; a++) {                      /* longer paths first (RFC 6265 5.4), stable */
        int v = order[a], b = a;
        while (b > 0 && strlen(j[order[b - 1]].path) < strlen(j[v].path)) { order[b] = order[b - 1]; b--; }
        order[b] = v;
    }
    for (int a = 0; a < k; a++) {
        int l = snprintf(out + len, cap - len, "%s%s=%s", len ? "; " : "", j[order[a]].name, j[order[a]].val);
        if (l < 0 || len + l >= cap) { out[len] = 0; break; }
        len += l;
    }
    free(j);
    return len;
}

/* one Set-Cookie value (or a document.cookie assignment) for a response from host, request path rpath */
void cookie_store(const char *host, const char *rpath, const char *sc, int from_js, int secure_origin) {
    struct ck c = { .host = 1 };
    char *s = strdup(sc), *p = s;
    if (!s) return;
    char *semi = strchr(p, ';');
    if (semi) *semi = 0;
    char *eq = strchr(p, '=');
    if (!eq) { free(s); return; }                      /* nameless cookies: not supported */
    *eq = 0;
    char *nm = p;
    while (isspace((unsigned char)*nm)) nm++;
    char *ne = nm + strlen(nm);
    while (ne > nm && isspace((unsigned char)ne[-1])) *--ne = 0;
    char *vl = eq + 1;
    while (isspace((unsigned char)*vl)) vl++;
    char *ve = vl + strlen(vl);
    while (ve > vl && isspace((unsigned char)ve[-1])) *--ve = 0;
    if (!*nm || strlen(nm) >= sizeof c.name || strlen(vl) >= sizeof c.val) { free(s); return; }
    strcpy(c.name, nm); strcpy(c.val, vl);
    snprintf(c.dom, sizeof c.dom, "%s", host);
    int rl = strcspn(rpath, "?#");
    char dp[128];
    snprintf(dp, sizeof dp, "%.*s", rl, rpath);
    char *ls = strrchr(dp, '/');
    if (!ls || ls == dp) snprintf(c.path, sizeof c.path, "/"); else { *ls = 0; snprintf(c.path, sizeof c.path, "%s", dp); }
    long maxage = 0; int have_ma = 0; long exp = 0;
    for (char *a = semi ? semi + 1 : 0; a && *a; ) {
        char *nx = strchr(a, ';');
        if (nx) *nx++ = 0;
        while (isspace((unsigned char)*a)) a++;
        char *v = strchr(a, '=');
        if (v) *v++ = 0;
        char *ae = a + strlen(a);
        while (ae > a && isspace((unsigned char)ae[-1])) *--ae = 0;
        while (v && isspace((unsigned char)*v)) v++;
        if (!strcasecmp(a, "domain") && v && *v) {
            if (*v == '.') v++;
            size_t hl = strlen(host), dl = strlen(v);
            int ok = dl < sizeof c.dom && strchr(v, '.') && (!strcasecmp(host, v) || (hl > dl && host[hl - dl - 1] == '.' && !strcasecmp(host + hl - dl, v)));
            /* ponytail: no public-suffix list, so Domain=co.uk is accepted; a PSL table would close it */
            if (ok) { snprintf(c.dom, sizeof c.dom, "%s", v); c.host = 0; }
            else { free(s); return; }
        } else if (!strcasecmp(a, "path") && v && *v == '/') snprintf(c.path, sizeof c.path, "%s", v);
        else if (!strcasecmp(a, "expires") && v) { long e = http_date(v); if (e >= 0) exp = e ? e : 1; }
        else if (!strcasecmp(a, "max-age") && v && (isdigit((unsigned char)*v) || *v == '-')) { maxage = atol(v); have_ma = 1; }
        else if (!strcasecmp(a, "secure")) c.secure = 1;
        else if (!strcasecmp(a, "httponly")) c.httponly = 1;
        a = nx;
    }
    for (char *d = c.dom; *d; d++) *d = tolower((unsigned char)*d);
    long now = time(0);
    if (have_ma) exp = maxage <= 0 ? 1 : now + maxage;
    c.exp = exp;                                       /* 0: session; 1..now: expired = delete */
    if ((c.secure && !secure_origin) || (from_js && c.httponly)) { free(s); return; }
    free(s);
    struct ck *j;
    int n = jar_load(&j), o = 0, blocked = 0;
    for (int i = 0; i < n; i++) {                      /* drop the old twin and anything expired */
        int same = !strcmp(j[i].name, c.name) && !strcmp(j[i].dom, c.dom) && !strcmp(j[i].path, c.path) && j[i].host == c.host;
        if (same && j[i].httponly && from_js) { j[o++] = j[i]; blocked = 1; continue; }   /* script can't replace HttpOnly */
        if (!same && (!j[i].exp || j[i].exp > now)) j[o++] = j[i];
    }
    if (!blocked && !(c.exp && c.exp <= now) && o < MAXCK) j[o++] = c;
    jar_save(j, o);
    free(j);
}

/* document.cookie for the page at url: the cookies a script may see, "a=1; b=2" */
void cookie_get(const char *url, char *out, int cap) {
    char host[128], path[URL_MAX];
    int port;
    out[0] = 0;
    if (strncasecmp(url, "data:", 5) && url_split_http(url, host, sizeof host, &port, path, sizeof path) == 0)
        cookie_header(host, path, !strncasecmp(url, "https:", 6), 1, out, cap);
}

/* document.cookie = "..." on the page at url */
void cookie_set(const char *url, const char *setcookie) {
    char host[128], path[URL_MAX];
    int port;
    if (strncasecmp(url, "data:", 5) && url_split_http(url, host, sizeof host, &port, path, sizeof path) == 0)
        cookie_store(host, path, setcookie, 1, !strncasecmp(url, "https:", 6));
}
