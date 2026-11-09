/* urltest - user/bin/browser/url.c on the host: RFC 3986 section 5.4 reference resolution plus our percent-encoding rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

static int fails, checks;
static void rs(const char *base, const char *ref, const char *want) {
    char out[URL_MAX];
    url_resolve(base, ref, out, sizeof out);
    checks++;
    if (strcmp(out, want)) { printf("FAIL resolve(%s, %s) = %s, want %s\n", base, ref, out, want); fails++; }
}
static void inp(const char *in, const char *want) {
    char out[URL_MAX];
    url_from_input(in, out, sizeof out);
    checks++;
    if (strcmp(out, want)) { printf("FAIL input(%s) = %s, want %s\n", in, out, want); fails++; }
}

int main(void) {
    const char *b = "http://a/b/c/d;p?q";
    /* 5.4.1 normal */
    rs(b, "g:h", "g:h"); rs(b, "g", "http://a/b/c/g"); rs(b, "./g", "http://a/b/c/g"); rs(b, "g/", "http://a/b/c/g/");
    rs(b, "/g", "http://a/g"); rs(b, "//g", "http://g/"); rs(b, "?y", "http://a/b/c/d;p?y"); rs(b, "g?y", "http://a/b/c/g?y");
    rs(b, "#s", "http://a/b/c/d;p?q#s"); rs(b, "g#s", "http://a/b/c/g#s"); rs(b, "g?y#s", "http://a/b/c/g?y#s");
    rs(b, ";x", "http://a/b/c/;x"); rs(b, "g;x", "http://a/b/c/g;x"); rs(b, "g;x?y#s", "http://a/b/c/g;x?y#s");
    rs(b, "", "http://a/b/c/d;p?q"); rs(b, ".", "http://a/b/c/"); rs(b, "./", "http://a/b/c/"); rs(b, "..", "http://a/b/");
    rs(b, "../", "http://a/b/"); rs(b, "../g", "http://a/b/g"); rs(b, "../..", "http://a/"); rs(b, "../../", "http://a/"); rs(b, "../../g", "http://a/g");
    /* 5.4.2 abnormal */
    rs(b, "../../../g", "http://a/g"); rs(b, "../../../../g", "http://a/g"); rs(b, "/./g", "http://a/g"); rs(b, "/../g", "http://a/g");
    rs(b, "g.", "http://a/b/c/g."); rs(b, ".g", "http://a/b/c/.g"); rs(b, "g..", "http://a/b/c/g.."); rs(b, "..g", "http://a/b/c/..g");
    rs(b, "./../g", "http://a/b/g"); rs(b, "./g/.", "http://a/b/c/g/"); rs(b, "g/./h", "http://a/b/c/g/h"); rs(b, "g/../h", "http://a/b/c/h");
    rs(b, "g;x=1/./y", "http://a/b/c/g;x=1/y"); rs(b, "g;x=1/../y", "http://a/b/c/y");
    rs(b, "g?y/./x", "http://a/b/c/g?y/./x"); rs(b, "g?y/../x", "http://a/b/c/g?y/../x"); rs(b, "g#s/./x", "http://a/b/c/g#s/./x"); rs(b, "g#s/../x", "http://a/b/c/g#s/../x");
    /* browser cases */
    rs("https://Example.COM:8443/a/b.html", "//cdn.example.com/x.js", "https://cdn.example.com/x.js");
    rs("https://example.com/a/b.html", "c d.png", "https://example.com/a/c%20d.png");
    rs("https://example.com/a/", "caf\xc3\xa9.png?x=\xc3\xa9#\xc3\xa9", "https://example.com/a/caf%C3%A9.png?x=%C3%A9#\xc3\xa9");
    rs("https://example.com/a/", "  x.png\n", "https://example.com/a/x.png");
    rs("http://10.0.0.1:8080/p/q", "../r", "http://10.0.0.1:8080/r");
    rs("https://example.com", "x", "https://example.com/x");
    rs("https://example.com/a?b#c", "#d", "https://example.com/a?b#d");
    rs("https://example.com/a/b", "HTTP://Other.com/Q", "http://other.com/Q");
    rs("https://example.com/", "data:image/png;base64,AAAA BBBB", "data:image/png;base64,AAAA BBBB");
    rs("https://example.com/", "javascript:void(0)", "javascript:void(0)");
    inp("example.com", "http://example.com/"); inp("localhost:8080/x", "http://localhost:8080/x");
    inp("/etc/motd", "file:///etc/motd"); inp("https://a.com/x y", "https://a.com/x%20y");
    char host[128], path[URL_MAX];
    int port;
    checks++; if (url_split_http("http://u:p@10.1.2.3:81/x?y#z", host, sizeof host, &port, path, sizeof path) || strcmp(host, "10.1.2.3") || port != 81 || strcmp(path, "/x?y")) { puts("FAIL split"); fails++; }
    checks++; if (url_split_http("https://example.com", host, sizeof host, &port, path, sizeof path) || port != 443 || strcmp(path, "/")) { puts("FAIL split2"); fails++; }
    checks++; if (!url_is_web("data:,x") || !url_is_web("HTTPS://x") || url_is_web("file:///x")) { puts("FAIL is_web"); fails++; }
    printf("urltest: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
