/* browser - Vibe Browser: address bar, history, loading state, scrolling, built-in pages.
 * Networking lives in fetch.c (helper process), rendering in html.c. */
#include "browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <syscall.h>

#define TB 48              /* toolbar height */
#define SB 12              /* scrollbar column */
#define STH 24             /* status bar height */
#define AX 214             /* address box x */
#define HMAX 100
#define TIMEOUT_MS 20000
#define MAX_REDIRECTS 10

static struct gui_window *win;
static struct doc doc;
static struct { char *raw; size_t n; int html, have; } pg;
static char cur_url[URL_MAX], pend_url[URL_MAX], addr[URL_MAX];
static char status[200];
static bool status_bad;
static struct { char url[URL_MAX]; int scroll; } hist[HMAX];
static int nhist, hcur = -1;
static struct fetch fe;
static bool loading;
static int pend_push, pend_restore = -1, hops, layout_w;
static int scroll, target;
static uint32_t load_t0;

static void say(bool bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_bad = bad;
}

/* ---------- growable string for generated pages ---------- */
struct sb { char *p; size_t n, cap; };
static void sb_add(struct sb *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->n + l + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}
static void sb_esc(struct sb *b, const char *s) {
    char t[2] = { 0, 0 };
    for (; *s; s++) {
        if (*s == '&') sb_add(b, "&amp;");
        else if (*s == '<') sb_add(b, "&lt;");
        else if (*s == '>') sb_add(b, "&gt;");
        else if (*s == '"') sb_add(b, "&quot;");
        else { t[0] = *s; sb_add(b, t); }
    }
}

static const char HOME_HTML[] =
    "<html><head><title>Vibe Browser</title></head>"
    "<body bgcolor=\"#16152A\" text=\"#EDEBFF\" link=\"#B388FF\">"
    "<center><h1>Vibe Browser</h1>"
    "<p>A small web browser for VibeOS &mdash; plain HTTP, HTML without scripts, no frills.</p></center>"
    "<hr>"
    "<h2>Try the web</h2>"
    "<ul>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> &ndash; the first website ever made"
    "<li><a href=\"http://example.com/\">example.com</a> &ndash; the canonical example domain"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a> &ndash; plain HTTP, on purpose"
    "<li><a href=\"https://www.wikipedia.org/\">wikipedia.org</a> &ndash; HTTPS, which shows the friendly &ldquo;not yet&rdquo; page"
    "</ul>"
    "<h2>On this computer</h2>"
    "<ul>"
    "<li><a href=\"file:///home/Documents/test.html\">Rendering test page</a> &ndash; lists, tables, code, quotes"
    "<li><a href=\"file:///home/welcome.txt\">welcome.txt</a> &ndash; plain text in a mono font"
    "<li><a href=\"file:///home/\">/home</a> &ndash; a directory listing"
    "<li><a href=\"about:blank\">about:blank</a>"
    "</ul>"
    "<h2>Keys</h2>"
    "<blockquote>"
    "<b>Ctrl+L</b> address bar &middot; <b>Ctrl+R</b> or <b>F5</b> reload &middot; <b>Alt+&larr;</b> / <b>Alt+&rarr;</b> history<br>"
    "<b>Space</b>, <b>PgDn</b>, <b>PgUp</b>, <b>Home</b>, <b>End</b>, arrows and the wheel scroll &middot; <b>Esc</b> stops loading"
    "</blockquote>"
    "<hr><center><em>made on VibeOS</em></center>"
    "</body></html>";

/* ---------- page lifecycle ---------- */
static int view_w(void) { return win->w - SB; }
static int view_h(void) { return win->h - TB - STH; }
static int max_scroll(void) { int m = doc.height - view_h(); return m > 0 ? m : 0; }
static void clamp_scroll(void) {
    int m = max_scroll();
    if (target > m) target = m;
    if (target < 0) target = 0;
    if (scroll > m) scroll = m;
    if (scroll < 0) scroll = 0;
}

static const char *base_name(const char *url) {
    const char *e = url + strcspn(url, "?#"), *s = e;
    while (s > url && s[-1] != '/') s--;
    return s;
}

static void commit(const char *url, char *raw, size_t n, int html) {
    free(pg.raw);
    pg.raw = raw; pg.n = raw ? n : 0; pg.html = html; pg.have = 1;
    if (pend_push && !(hcur >= 0 && !strcmp(hist[hcur].url, url))) {
        if (hcur >= 0) hist[hcur].scroll = scroll;
        nhist = hcur + 1;
        if (nhist == HMAX) { memmove(hist, hist + 1, (HMAX - 1) * sizeof *hist); nhist--; }
        strlcpy(hist[nhist].url, url, sizeof hist[0].url);
        hist[nhist].scroll = 0;
        hcur = nhist++;
    }
    strlcpy(cur_url, url, sizeof cur_url);
    doc.title[0] = 0;
    layout_w = view_w();
    doc_layout(&doc, pg.raw, pg.n, html, layout_w);
    if (!doc.title[0]) {
        const char *b = base_name(url);
        strlcpy(doc.title, *b ? b : url, sizeof doc.title);
    }
    scroll = target = 0;
    const char *frag = strchr(url, '#');
    int y;
    if (pend_restore >= 0) scroll = target = pend_restore;
    else if (frag && (y = doc_anchor(&doc, frag + 1)) >= 0) scroll = target = y > 8 ? y - 8 : 0;
    pend_restore = -1;
    clamp_scroll();
    loading = false;
}

static void show_msg(const char *url, const char *title, const char *heading, const char *text, const char *hint) {
    struct sb b = { 0 };
    sb_add(&b, "<html><head><title>"); sb_esc(&b, title); sb_add(&b, "</title></head><body><h1>");
    sb_esc(&b, heading); sb_add(&b, "</h1><p>"); sb_esc(&b, text); sb_add(&b, "</p>");
    sb_add(&b, hint);
    sb_add(&b, "</body></html>");
    commit(url, b.p, b.n, 1);
}

static const char *HOME_LINK = "<p><a href=\"about:home\">Go to the start page</a></p>";

static void page_https(const char *url) {
    struct sb h = { 0 };
    char httpurl[URL_MAX];
    snprintf(httpurl, sizeof httpurl, "http%s", url + 5);
    sb_add(&h, "<ul><li>Try the <a href=\"");
    sb_esc(&h, httpurl);
    sb_add(&h, "\">http:// version</a> of this address (many sites will just redirect back to https).<li>Open a plain-HTTP site such as "
               "<a href=\"http://example.com/\">example.com</a> or <a href=\"http://info.cern.ch/\">info.cern.ch</a>.</ul>");
    sb_add(&h, HOME_LINK);
    char t[URL_MAX + 80];
    snprintf(t, sizeof t, "This browser can't open %s because it doesn't speak TLS yet, so encrypted pages can't be loaded.", url);
    show_msg(url, "HTTPS isn't supported yet", "HTTPS isn't supported yet", t, h.p ? h.p : "");
    free(h.p);
    say(true, "HTTPS isn't supported yet");
}

static void net_error(const char *url, const char *err) {
    char heading[96], text[400], kind[16];
    const char *detail = strchr(err, ':');
    snprintf(kind, sizeof kind, "%.*s", detail ? (int)(detail - err) : 0, err);
    detail = detail ? detail + 1 : err;
    if (!strcmp(kind, "dns")) {
        strlcpy(heading, "Can't find the server", sizeof heading);
        snprintf(text, sizeof text, "The address \"%s\" couldn't be resolved. Check the spelling, or your network connection.", detail);
    } else if (!strcmp(kind, "connect")) {
        strlcpy(heading, "Can't connect to the server", sizeof heading);
        snprintf(text, sizeof text, "The connection failed: %s.", detail);
    } else if (!strcmp(kind, "timeout")) {
        strlcpy(heading, "The server took too long to respond", sizeof heading);
        snprintf(text, sizeof text, "No data arrived for %d seconds.", TIMEOUT_MS / 1000);
    } else if (!strcmp(kind, "enc")) {
        strlcpy(heading, "Unsupported content encoding", sizeof heading);
        snprintf(text, sizeof text, "The server sent %s-compressed data, which this browser can't decode.", detail);
    } else {
        strlcpy(heading, "Couldn't load the page", sizeof heading);
        snprintf(text, sizeof text, "%s.", detail);
    }
    struct sb h = { 0 };
    sb_add(&h, "<p><a href=\"");
    sb_esc(&h, url);
    sb_add(&h, "\">Try again</a> &middot; <a href=\"about:home\">Start page</a></p>");
    show_msg(url, heading, heading, text, h.p ? h.p : "");
    free(h.p);
    say(true, "%s", heading);
}

static void unsupported(const char *url, const char *what, size_t n) {
    char t[300];
    snprintf(t, sizeof t, "%s (%lu bytes) can't be shown here: Vibe Browser renders HTML and plain text only.", what, (unsigned long)n);
    show_msg(url, "Can't display this", "Can't display this file", t, HOME_LINK);
    say(true, "Unsupported content");
}

/* ---------- file:// ---------- */
static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void load_dir(const char *url, const char *path) {
    DIR *d = opendir(path);
    if (!d) { show_msg(url, "Not found", "Can't open this folder", path, HOME_LINK); say(true, "Can't open %s", path); return; }
    char **names = malloc(2000 * sizeof *names);
    int n = 0;
    struct dirent *de;
    while (names && (de = readdir(d)) && n < 2000) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[PATH_MAX + 64];
        snprintf(full, sizeof full, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", de->d_name);
        struct stat st;
        char *nm = malloc(strlen(de->d_name) + 2);
        if (!nm) break;
        strcpy(nm, de->d_name);
        if (!stat(full, &st) && S_ISDIR(st.st_mode)) strcat(nm, "/");
        names[n++] = nm;
    }
    closedir(d);
    if (names) qsort(names, n, sizeof *names, cmp_name);
    struct sb b = { 0 };
    sb_add(&b, "<html><head><title>Index of "); sb_esc(&b, path);
    sb_add(&b, "</title></head><body><h1>Index of "); sb_esc(&b, path); sb_add(&b, "</h1><ul>");
    if (strcmp(path, "/")) sb_add(&b, "<li><a href=\"../\">../</a>");
    for (int i = 0; i < n; i++) {
        char enc[PATH_MAX];
        int o = 0;
        for (const char *s = names[i]; *s && o < (int)sizeof enc - 4; s++) {
            if (*s == ' ' || *s == '%' || *s == '#' || *s == '?') o += snprintf(enc + o, 4, "%%%02X", *s);
            else enc[o++] = *s;
        }
        enc[o] = 0;
        sb_add(&b, "<li><a href=\""); sb_esc(&b, enc); sb_add(&b, "\">"); sb_esc(&b, names[i]); sb_add(&b, "</a>");
        free(names[i]);
    }
    free(names);
    sb_add(&b, "</ul></body></html>");
    commit(url, b.p, b.n, 1);
    say(false, "%d items", n);
}

static int ext_is(const char *p, const char *list) {          /* list: ".a.b.c" */
    const char *dot = strrchr(p, '.');
    if (!dot || strchr(dot, '/')) return 0;
    size_t l = strlen(dot);
    for (const char *s = list; (s = strstr(s, dot)); s++)
        if (s[l] == '.' || s[l] == 0) return 1;
    return 0;
}

static void load_file(const char *url) {
    const char *p = url + 5;
    if (p[0] == '/' && p[1] == '/') { p += 2; p += strcspn(p, "/"); }
    char path[PATH_MAX], raw[URL_MAX];
    snprintf(raw, sizeof raw, "%.*s", (int)strcspn(p, "?#"), p);
    url_unescape(raw, path, sizeof path);
    if (!path[0]) strcpy(path, "/");
    struct stat st;
    if (stat(path, &st) < 0) {
        char t[PATH_MAX + 40];
        snprintf(t, sizeof t, "There is no file or folder at %s.", path);
        show_msg(url, "Not found", "File not found", t, HOME_LINK);
        say(true, "File not found: %s", path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char u[URL_MAX];
        snprintf(u, sizeof u, "%s%s", url, url[strlen(url) - 1] == '/' ? "" : "/");
        load_dir(u, path);
        return;
    }
    int fd = open(path, O_RDONLY);
    size_t size = st.st_size > MAX_PAGE ? MAX_PAGE : (size_t)st.st_size, got = 0;
    char *buf = fd >= 0 ? malloc(size + 1) : 0;
    while (buf && got < size) {
        ssize_t r = read(fd, buf + got, size - got);
        if (r <= 0) break;
        got += r;
    }
    if (fd >= 0) close(fd);
    if (!buf) { show_msg(url, "Error", "Can't read this file", path, HOME_LINK); say(true, "Can't read %s", path); return; }
    buf[got] = 0;
    for (size_t i = 0; i < got && i < 512; i++)
        if (!buf[i]) { free(buf); unsupported(url, "This looks like a binary file", got); return; }
    int html;
    if (ext_is(path, ".html.htm.xhtml")) html = 1;
    else if (ext_is(path, ".txt.md.c.h.css.js.json.conf.log.sh.py.xml.csv.ini")) html = 0;
    else { const char *s = buf; while (isspace((unsigned char)*s)) s++; html = *s == '<'; }
    commit(url, buf, got, html);
    say(false, "%s - %lu bytes%s", path, (unsigned long)got, st.st_size > MAX_PAGE ? " (truncated at 2 MiB)" : "");
}

/* ---------- http ---------- */
static void load_url(const char *url);

static void start_fetch(const char *url) {
    strlcpy(pend_url, url, sizeof pend_url);
    if (fetch_start(&fe, url) < 0) { net_error(url, "net:couldn't start the network helper"); return; }
    loading = true;
    load_t0 = fe.last;
    char host[128], path[8];
    int port;
    url_split_http(url, host, sizeof host, &port, path, sizeof path);
    say(false, "Looking up %s...", host);
}

static void finish_fetch(void) {
    struct response rs;
    fetch_finish(&fe, &rs);
    loading = false;
    if (rs.err[0]) { net_error(pend_url, rs.err); return; }
    if (rs.status >= 300 && rs.status < 400 && rs.loc[0]) {
        char next[URL_MAX];
        url_resolve(pend_url, rs.loc, next, sizeof next);
        free(rs.body);
        if (++hops > MAX_REDIRECTS) { net_error(pend_url, "net:too many redirects"); return; }
        load_url(next);
        return;
    }
    int html = -1;
    const char *ct = rs.ctype;
    if (!strncmp(ct, "image/", 6) || !strncmp(ct, "audio/", 6) || !strncmp(ct, "video/", 6)) { unsupported(pend_url, ct, rs.blen); free(rs.body); return; }
    if (strstr(ct, "html")) html = 1;
    else if (!ct[0]) { const char *s = rs.body; while (isspace((unsigned char)*s)) s++; html = *s == '<'; }
    else if (!strncmp(ct, "text/", 5) || strstr(ct, "json") || strstr(ct, "xml") || strstr(ct, "javascript")) html = 0;
    for (size_t i = 0; html < 0 || (i < rs.blen && i < 512); i++) {
        if (html < 0 || !rs.body[i]) { unsupported(pend_url, ct[0] ? ct : "This content", rs.blen); free(rs.body); return; }
    }
    if (rs.latin1) {                                    /* ISO-8859-1 / cp1252 -> UTF-8 */
        char *u = malloc(rs.blen * 2 + 1);
        if (u) {
            size_t o = 0;
            for (size_t i = 0; i < rs.blen; i++) {
                unsigned char c = rs.body[i];
                if (c < 0x80) u[o++] = c; else { u[o++] = 0xC0 | c >> 6; u[o++] = 0x80 | (c & 63); }
            }
            u[o] = 0;
            free(rs.body); rs.body = u; rs.blen = o;
        }
    }
    size_t n = rs.blen;
    commit(pend_url, rs.body, n, html);
    say(rs.status >= 400, "%d %s - %lu bytes%s", rs.status, rs.text, (unsigned long)n, rs.truncated ? " (truncated at 2 MiB)" : "");
}

static void service_fetch(void) {
    int r = fetch_pump(&fe);
    uint32_t now = uptime_ms();
    if (r == 0) {
        if (now - fe.last > TIMEOUT_MS) { fetch_cancel(&fe); loading = false; net_error(pend_url, "timeout:"); }
        else if (fe.len) say(false, "Loading... %lu KB", (unsigned long)(fe.len / 1024));
        return;
    }
    if (r < 0) { fetch_cancel(&fe); loading = false; net_error(pend_url, "net:read error"); return; }
    finish_fetch();
}

static void cancel_load(void) {
    if (!loading) return;
    fetch_cancel(&fe);
    loading = false;
}

static void load_url(const char *url) {
    if (!strncasecmp(url, "http://", 7)) start_fetch(url);
    else if (!strncasecmp(url, "https://", 8)) page_https(url);
    else if (!strncasecmp(url, "file:", 5)) load_file(url);
    else if (!strcmp(url, "about:home")) { commit(url, strdup(HOME_HTML), sizeof HOME_HTML - 1, 1); strlcpy(doc.title, "Vibe Browser", sizeof doc.title); say(false, "Start page"); }
    else if (!strcmp(url, "about:blank")) { commit(url, strdup(""), 0, 1); strlcpy(doc.title, "New tab", sizeof doc.title); say(false, "Blank page"); }
    else {
        char t[URL_MAX + 60];
        snprintf(t, sizeof t, "Vibe Browser doesn't know how to open \"%s\".", url);
        show_msg(url, "Unsupported address", "Unsupported address", t, HOME_LINK);
        say(true, "Unsupported address");
    }
}

/* push: record in history on success; restore: scroll position to return to, or -1 */
static void navigate(const char *url, int push, int restore) {
    char a[URL_MAX], b[URL_MAX];
    strlcpy(a, url, sizeof a); url_strip_frag(a);
    strlcpy(b, cur_url, sizeof b); url_strip_frag(b);
    int y;
    if (strchr(url, '#') && !strcmp(a, b) && pg.have && !loading && (y = doc_anchor(&doc, strchr(url, '#') + 1)) >= 0) {
        target = y > 8 ? y - 8 : 0;                    /* same page: just scroll */
        strlcpy(cur_url, url, sizeof cur_url);
        if (hcur >= 0) strlcpy(hist[hcur].url, url, sizeof hist[0].url);
        clamp_scroll();
        return;
    }
    cancel_load();
    pend_push = push; pend_restore = restore; hops = 0;
    strlcpy(pend_url, url, sizeof pend_url);
    load_url(pend_url);
}

static void go_hist(int d) {
    int n = hcur + d;
    if (n < 0 || n >= nhist) return;
    if (!loading) hist[hcur].scroll = scroll;
    hcur = n;
    char u[URL_MAX];
    strlcpy(u, hist[hcur].url, sizeof u);
    navigate(u, 0, hist[hcur].scroll);
}

static void follow(int link) {
    char u[URL_MAX];
    url_resolve(cur_url, doc.link[link], u, sizeof u);
    if (!strncasecmp(u, "mailto:", 7) || !strncasecmp(u, "tel:", 4)) { say(true, "Can't open %s links", "mailto/tel"); return; }
    navigate(u, 1, -1);
}

static void relayout(void) {
    int old = doc.height ? doc.height : 1;
    layout_w = view_w();
    doc_layout(&doc, pg.raw, pg.n, pg.html, layout_w);
    if (!doc.title[0]) strlcpy(doc.title, base_name(cur_url), sizeof doc.title);
    scroll = (int)((long long)scroll * doc.height / old);
    target = scroll;
    clamp_scroll();
}

/* ---------- main loop ---------- */
static void drop_key(struct ui *ui, int i) { memmove(ui->keys + i, ui->keys + i + 1, (--ui->nkeys - i) * sizeof ui->keys[0]); }

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--fetch")) return fetch_main(argv[2]);
    win = gui_window_create("Vibe Browser", 960, 660, 0);
    if (!win) { fprintf(stderr, "browser: can't open a window\n"); return 1; }
    struct ui ui;
    ui_init(&ui, win);
    char start[URL_MAX] = "about:home";
    if (argc > 1) url_from_input(argv[1], start, sizeof start);
    navigate(start, 1, -1);
    char last_title[96] = "";

    for (;;) {
        int timeout = loading ? 80 : -1;
        if (scroll != target) timeout = 16;
        struct pollfd pf[2] = { { win->fd, POLLIN, 0 }, { fe.fd, POLLIN, 0 } };
        poll(pf, loading ? 2 : 1, timeout);
        if (!ui_poll(&ui, 0)) break;
        fetch_reap();
        if (loading) service_fetch();

        int W = win->w, H = win->h, vw = view_w(), vh = view_h();
        if (pg.have && vw != layout_w) relayout();
        int aw = W - AX - 10;
        uint32_t aid = ui_id(AX, 8, aw, 32);

        /* global shortcuts first: the address box would otherwise swallow them */
        for (int i = 0; i < ui.nkeys; ) {
            struct gui_event k = ui.keys[i];
            int c = k.key | 32, ctrl = k.mods & MOD_CTRL, handled = 1;
            if (ctrl && (c == 'l' || k.ch == 12)) { ui.focus = aid; addr[0] = 0; }
            else if ((ctrl && (c == 'r' || k.ch == 18)) || k.key == KEY_F5) navigate(hist[hcur].url, 0, scroll);
            else if ((k.mods & MOD_ALT) && k.key == KEY_LEFT) go_hist(-1);
            else if ((k.mods & MOD_ALT) && k.key == KEY_RIGHT) go_hist(1);
            else if (k.key == KEY_ESC) { if (loading) { cancel_load(); say(false, "Stopped"); } ui.focus = 0; }
            else handled = 0;
            if (handled) drop_key(&ui, i); else i++;
        }

        /* toolbar */
        gfx_fill(ui.c, 0, 0, W, TB, UI_PANEL);
        gfx_fill(ui.c, 0, TB - 1, W, 1, UI_BORDER);
        bool can_back = hcur > 0, can_fwd = hcur + 1 < nhist;
        if (can_back ? ui_tool(&ui, 8, 8, 32, 32, "\xE2\x86\x90", false) : (gfx_text_center(ui.c, &font_ui, 8, 8 + (32 - font_ui.height) / 2, 32, "\xE2\x86\x90", UI_TEXT_DIM), false)) go_hist(-1);
        if (can_fwd ? ui_tool(&ui, 44, 8, 32, 32, "\xE2\x86\x92", false) : (gfx_text_center(ui.c, &font_ui, 44, 8 + (32 - font_ui.height) / 2, 32, "\xE2\x86\x92", UI_TEXT_DIM), false)) go_hist(1);
        if (ui_tool(&ui, 80, 8, 64, 32, loading ? "Stop" : "Reload", false)) {
            if (loading) { cancel_load(); say(false, "Stopped"); }
            else if (hcur >= 0) navigate(hist[hcur].url, 0, scroll);
        }
        if (ui_tool(&ui, 148, 8, 56, 32, "Home", false)) navigate("about:home", 1, -1);
        if (ui.focus != aid) strlcpy(addr, loading ? pend_url : cur_url, sizeof addr);
        if (ui_textbox(&ui, AX, 8, aw, 32, addr, sizeof addr, "Search or enter address")) {
            char u[URL_MAX];
            url_from_input(addr, u, sizeof u);
            ui.focus = 0;
            navigate(u, 1, -1);
        }
        if (loading) {                                   /* indeterminate progress stripe */
            int span = W / 4, px = (int)((uptime_ms() - load_t0) / 4 % (uint32_t)(W + span)) - span;
            gfx_fill(ui.c, px < 0 ? 0 : px, TB - 3, px < 0 ? span + px : span, 2, UI_ACCENT);
        }

        /* content */
        int hover = -1;
        bool in_view = ui.mx >= 0 && ui.mx < vw && ui.my >= TB && ui.my < TB + vh;
        if (in_view) hover = doc_hit(&doc, ui.mx, ui.my - TB + scroll);
        if (ui.pressed && hover >= 0) { follow(hover); hover = -1; }
        char hover_url[URL_MAX];
        if (hover >= 0) url_resolve(cur_url, doc.link[hover], hover_url, sizeof hover_url);
        if (ui.scroll && ui.my >= TB) target += ui.scroll * 64;
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            bool shift = k.mods & MOD_SHIFT;
            switch (k.key) {
            case KEY_DOWN: target += 48; break;
            case KEY_UP: target -= 48; break;
            case KEY_PGDN: target += vh - 40; break;
            case KEY_PGUP: target -= vh - 40; break;
            case ' ': target += shift ? 40 - vh : vh - 40; break;
            case KEY_HOME: target = 0; break;
            case KEY_END: target = max_scroll(); break;
            }
        }
        clamp_scroll();
        if (scroll != target) {                          /* ease toward the target */
            int d = target - scroll, step = d / 3;
            if (!step) step = d > 0 ? 1 : -1;
            scroll += step;
        }
        doc_draw(&ui, &doc, 0, TB, vw, vh, scroll, hover);
        gfx_fill(ui.c, vw, TB, SB, vh, UI_PANEL);
        int sp = scroll;
        ui_vscroll(&ui, vw + 1, TB, vh, doc.height, vh, &sp);
        if (sp != scroll) scroll = target = sp;

        /* status bar */
        gfx_fill(ui.c, 0, H - STH, W, STH, UI_PANEL);
        gfx_fill(ui.c, 0, H - STH, W, 1, UI_BORDER);
        ui_text_fit(&ui, &font_ui, 10, H - STH + (STH - font_ui.height) / 2, W - 20, hover >= 0 ? hover_url : status,
                    hover >= 0 ? UI_ACCENT : status_bad ? UI_BAD : UI_TEXT_DIM);

        char t[96];
        snprintf(t, sizeof t, strcmp(doc.title, "Vibe Browser") ? "%s - Vibe Browser" : "%s", doc.title);
        if (strcmp(t, last_title)) { gui_set_title(win, t); strlcpy(last_title, t, sizeof last_title); }
        ui_end(&ui);
    }
    cancel_load();
    return 0;
}
