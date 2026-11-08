/* Benchmark of css_compute on a real page: wpbench PAGE.html SHEET.css...  (tools/csstest/wpbench.sh fetches Wikipedia).
 * A tolerant mini HTML parser builds the element tree; every element is styled in document order. */
#include "browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

void url_resolve(const char *base, const char *ref, char *out, int cap) { snprintf(out, cap, "%s", ref); }
const char *sheet_get(const char *url, size_t *len) { return 0; }

#define MAXN 200000
struct node { const char *tag; char *id, *cls, *style; char *an[6], *av[6]; int nat, par, prev, next, first, last, text; };
static struct node *N;
static int nn = 1;
static int d_parent(int n) { return N[n].par; }
static int d_prev(int n) { return N[n].prev; }
static int d_next(int n) { return N[n].next; }
static int d_first(int n) { return N[n].first; }
static const char *d_tag(int n) { return N[n].tag; }
static const char *d_attr(int n, const char *a) {
    if (!strcmp(a, "id")) return N[n].id;
    if (!strcmp(a, "class")) return N[n].cls;
    if (!strcmp(a, "style")) return N[n].style;
    for (int i = 0; i < N[n].nat; i++) if (!strcmp(N[n].an[i], a)) return N[n].av[i];
    return 0;
}
static int d_state(int n) { return 0; }
static int d_text(int n) { return N[n].text; }
static const struct css_dom DOMT = { d_parent, d_prev, d_next, d_first, d_tag, d_attr, d_state, d_text };

static char *slurp(const char *f, size_t *n) {
    FILE *fp = fopen(f, "rb");
    if (!fp) { perror(f); exit(2); }
    fseek(fp, 0, SEEK_END); *n = ftell(fp); rewind(fp);
    char *b = malloc(*n + 1);
    if (fread(b, 1, *n, fp) != *n) exit(2);
    b[*n] = 0; fclose(fp);
    return b;
}

static int is_void(const char *t) {
    static const char *V[] = { "br", "hr", "img", "input", "meta", "link", "area", "base", "col", "embed", "source", "track", "wbr", "param" };
    for (unsigned i = 0; i < sizeof V / sizeof *V; i++) if (!strcmp(t, V[i])) return 1;
    return 0;
}

static void parse_html(char *s) {
    int cur = 0;
    while (*s) {
        if (*s != '<') { char *e = strchr(s, '<'); if (!e) break; for (char *p = s; p < e; p++) if (!isspace((unsigned char)*p)) { if (cur) N[cur].text = 1; break; } s = e; continue; }
        if (!strncmp(s, "<!--", 4)) { char *e = strstr(s, "-->"); s = e ? e + 3 : s + strlen(s); continue; }
        if (s[1] == '!' || s[1] == '?') { char *e = strchr(s, '>'); s = e ? e + 1 : s + strlen(s); continue; }
        if (s[1] == '/') {
            char *e = strchr(s, '>'); if (!e) break;
            char nm[32]; int k = 0;
            for (char *p = s + 2; p < e && k < 31 && !isspace((unsigned char)*p); p++) nm[k++] = tolower((unsigned char)*p);
            nm[k] = 0;
            for (int x = cur; x; x = N[x].par) if (!strcmp(N[x].tag, nm)) { cur = N[x].par; break; }
            s = e + 1; continue;
        }
        char *p = s + 1, nm[32]; int k = 0;
        while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/' && k < 31) nm[k++] = tolower((unsigned char)*p++);
        nm[k] = 0;
        int n = nn++;
        if (n >= MAXN) { fprintf(stderr, "too many\n"); exit(2); }
        N[n].tag = strdup(nm); N[n].par = cur;
        if (cur) { if (N[cur].last) { N[N[cur].last].next = n; N[n].prev = N[cur].last; } else N[cur].first = n; N[cur].last = n; }
        for (;;) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p || *p == '>') break;
            if (*p == '/') { p++; continue; }
            char an[40]; int l = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/' && l < 39) an[l++] = tolower((unsigned char)*p++);
            an[l] = 0;
            char *val = "";
            if (*p == '=') {
                p++;
                char q = *p == '"' || *p == '\'' ? *p++ : 0;
                char *v = p;
                while (*p && (q ? *p != q : !isspace((unsigned char)*p) && *p != '>')) p++;
                val = strndup(v, p - v);
                if (q && *p) p++;
            }
            if (!strcmp(an, "id")) N[n].id = val; else if (!strcmp(an, "class")) N[n].cls = val; else if (!strcmp(an, "style")) N[n].style = val;
            else if (N[n].nat < 6) { N[n].an[N[n].nat] = strdup(an); N[n].av[N[n].nat++] = val; }
        }
        s = *p ? p + 1 : p;
        if (!strcmp(nm, "script") || !strcmp(nm, "style")) { char end[16]; snprintf(end, sizeof end, "</%s", nm); char *e = strstr(s, end); s = e ? e : s + strlen(s); continue; }
        if (!is_void(nm)) cur = n;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wpbench page.html sheet.css...\n"); return 2; }
    N = calloc(MAXN, sizeof *N);
    size_t n;
    char *html = slurp(argv[1], &n);
    parse_html(html);
    static struct cprops *cs;
    cs = calloc(nn, sizeof *cs);
    css_reset(); css_dom_set(&DOMT); css_viewport_size(1280, 720);
    clock_t t0 = clock();
    int rules0 = 0;
    for (int i = 2; i < argc; i++) { size_t m; char *c = slurp(argv[i], &m); css_add_from(c, m, "https://en.wikipedia.org/"); free(c); }
    double parse_ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    (void)rules0;
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 20;
    double best = 1e9;
    for (int r = 0; r < reps; r++) {
        clock_t t1 = clock();
        for (int i = 1; i < nn; i++) css_compute(i, 0, N[i].par ? &cs[N[i].par] : 0, &cs[i]);
        double ms = (clock() - t1) * 1000.0 / CLOCKS_PER_SEC;
        if (ms < best) best = ms;
    }
    int hid = 0;
    for (int i = 1; i < nn; i++) hid += cs[i].v[P_DISPLAY] == D_NONE;
    printf("wpbench: %d elements (%d display:none), sheets parsed in %.1f ms; css_compute %.1f ms = %.2f us/element\n", nn - 1, hid, parse_ms, best, best * 1000 / (nn - 1));
    { extern void css_stats(void); css_stats(); }
    return 0;
}
