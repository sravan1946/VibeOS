/* css.c - a small CSS engine: stylesheet parser, selector matcher (type, .class, #id, descendant, child,
 * comma lists) and a cascade that boils the matching declarations of one element down to a `struct cprops`.
 * Anything it doesn't understand (pseudo-classes, attribute selectors, @rules, unknown properties, vw/calc
 * values) is skipped without disturbing the rest of the sheet. */
#include "browser.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct decl { uint8_t prop, imp, kind; int v; };
struct comp { char tag[12], id[24], cls[3][20]; uint8_t ncls, child; };   /* child: '>' to the previous compound */
struct rule { struct comp c[5]; uint8_t n; uint16_t spec; int d0, nd, order; };
static struct rule *R; static int nr, rcap;
static struct decl *D; static int nd, dcap;

void css_reset(void) { nr = nd = 0; }
int  css_any(void) { return nr > 0; }

/* ---------- colours ---------- */
int css_color(const char *s, color_t *out, int legacy) {
    static const struct { const char *n; uint32_t c; } N[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
        { "navy", 0x000080 }, { "purple", 0x800080 }, { "teal", 0x008080 }, { "olive", 0x808000 }, { "lime", 0x00FF00 },
        { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "orange", 0xFFA500 },
        { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "coral", 0xFF7F50 }, { "crimson", 0xDC143C },
        { "tomato", 0xFF6347 }, { "salmon", 0xFA8072 }, { "khaki", 0xF0E68C }, { "violet", 0xEE82EE }, { "indigo", 0x4B0082 },
        { "turquoise", 0x40E0D0 }, { "tan", 0xD2B48C }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "lavender", 0xE6E6FA },
        { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
        { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
        { "darkblue", 0x00008B }, { "darkgreen", 0x006400 }, { "darkred", 0x8B0000 }, { "lightblue", 0xADD8E6 },
        { "lightgreen", 0x90EE90 }, { "lightyellow", 0xFFFFE0 }, { "skyblue", 0x87CEEB }, { "royalblue", 0x4169E1 },
        { "steelblue", 0x4682B4 }, { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "orangered", 0xFF4500 },
        { "firebrick", 0xB22222 }, { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 }, { "midnightblue", 0x191970 },
        { "rebeccapurple", 0x663399 }, { "aliceblue", 0xF0F8FF }, { "cornflowerblue", 0x6495ED }, { "darkorange", 0xFF8C00 },
    };
    while (isspace((unsigned char)*s)) s++;
    int n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return 0;
    if (!strncasecmp(s, "rgb", 3) && (s[3] == '(' || (s[3] == 'a' && s[4] == '('))) {
        const char *p = strchr(s, '(') + 1;
        int v[3], i = 0;
        for (; i < 3; i++) {
            while (isspace((unsigned char)*p)) p++;
            if (!isdigit((unsigned char)*p) && *p != '.') return 0;
            double f = atof(p);
            while (isdigit((unsigned char)*p) || *p == '.') p++;
            if (*p == '%') { f = f * 255 / 100; p++; }
            v[i] = f < 0 ? 0 : f > 255 ? 255 : (int)(f + .5);
            while (isspace((unsigned char)*p)) p++;
            if (*p == ',' || *p == '/') p++;
        }
        while (isspace((unsigned char)*p)) p++;
        if (isdigit((unsigned char)*p) || *p == '.') {              /* alpha: mostly-transparent counts as none */
            double a = atof(p);
            if (a < 0.5) { *out = 0; return 1; }
        }
        *out = RGB(v[0], v[1], v[2]);
        return 1;
    }
    const char *h = s;
    if (*h == '#') h++; else if (!legacy) h = 0;
    if (h) {
        int hl = s + n - h, hex = hl == 3 || hl == 6;
        for (int i = 0; i < hl && hex; i++) hex = isxdigit((unsigned char)h[i]);
        if (hex) {
            uint32_t v = 0;
            for (int i = 0; i < hl; i++) v = v << 4 | (isdigit((unsigned char)h[i]) ? h[i] - '0' : (h[i] | 32) - 'a' + 10);
            if (hl == 3) v = ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17;
            *out = 0xFF000000u | v;
            return 1;
        }
    }
    if (n == 11 && !strncasecmp(s, "transparent", 11)) { *out = 0; return 1; }
    for (unsigned i = 0; i < sizeof N / sizeof *N; i++)
        if ((int)strlen(N[i].n) == n && !strncasecmp(s, N[i].n, n)) { *out = 0xFF000000u | N[i].c; return 1; }
    return 0;
}

/* ---------- values ---------- */
static int parse_len(const char *s, int *v, int *k) {
    while (isspace((unsigned char)*s)) s++;
    if (!strncasecmp(s, "auto", 4)) { *k = K_AUTO; *v = 0; return 1; }
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (!isdigit((unsigned char)*s) && *s != '.') return 0;
    long x = 0;                                         /* value * 100 */
    while (isdigit((unsigned char)*s)) x = x * 10 + *s++ - '0', x = x > 10000000 ? 10000000 : x;
    x *= 100;
    if (*s == '.') {
        s++;
        for (int f = 10; isdigit((unsigned char)*s); s++, f /= 10) x += (*s - '0') * f;
    }
    int scale = 100, pct = 0;                           /* px per unit * 100 */
    if (!strncasecmp(s, "px", 2)) scale = 100;
    else if (!strncasecmp(s, "pt", 2)) scale = 133;
    else if (!strncasecmp(s, "rem", 3) || !strncasecmp(s, "em", 2)) scale = 1600;
    else if (!strncasecmp(s, "ex", 2)) scale = 800;
    else if (!strncasecmp(s, "pc", 2)) scale = 1600;
    else if (!strncasecmp(s, "cm", 2)) scale = 3780;
    else if (!strncasecmp(s, "mm", 2)) scale = 378;
    else if (!strncasecmp(s, "in", 2)) scale = 9600;
    else if (*s == '%') pct = 1;
    else if (isalpha((unsigned char)*s)) return 0;      /* vw, vh, ch, calc... */
    *k = pct ? K_PCT : K_PX;
    long r = pct ? x / 100 : x * scale / 10000;
    *v = neg ? -r : r;
    return 1;
}

static void put(int prop, int kind, int v, int imp) {
    if (nd == dcap) {
        int nc = dcap ? dcap * 2 : 256;
        struct decl *n = realloc(D, nc * sizeof *n);
        if (!n) return;
        D = n; dcap = nc;
    }
    D[nd++] = (struct decl){ prop, imp, kind, v };
}

/* split a value into whitespace-separated tokens, keeping "rgb(1, 2, 3)" together; returns count */
static int tokens(char *s, char **tok, int max) {
    int n = 0;
    while (*s && n < max) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        tok[n++] = s;
        int depth = 0;
        while (*s && (depth || !isspace((unsigned char)*s))) { if (*s == '(') depth++; else if (*s == ')' && depth) depth--; s++; }
        if (*s) *s++ = 0;
    }
    return n;
}

static int is_bold(const char *t) { return !strcasecmp(t, "bold") || !strcasecmp(t, "bolder") || (isdigit((unsigned char)t[0]) && atoi(t) >= 600); }

static int side_of(const char *s, int n) {              /* top right bottom left -> 0..3 */
    static const char *const S[] = { "top", "right", "bottom", "left" };
    for (int i = 0; i < 4; i++) if ((int)strlen(S[i]) == n && !strncmp(s, S[i], n)) return i;
    return -1;
}

static void box4(int base, char *val, int imp, int allow_auto) {   /* margin / padding shorthand */
    char *tk[4];
    int n = tokens(val, tk, 4), v[4], k[4];
    if (!n) return;
    for (int i = 0; i < n; i++) {
        if (!parse_len(tk[i], &v[i], &k[i]) || (k[i] == K_AUTO && !allow_auto)) return;
    }
    static const uint8_t map[5][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
    for (int s = 0; s < 4; s++) { int i = map[n][s]; put(base + s, k[i], v[i], imp); }   /* base + side: T R B L */
}

static void border(int side, char *val, int imp) {      /* side -1 = all */
    char *tk[6];
    int n = tokens(val, tk, 6), w = 3, none = 0;
    color_t c = 0;
    int hasc = 0;
    for (int i = 0; i < n; i++) {
        int v, k;
        if (!strcasecmp(tk[i], "none") || !strcasecmp(tk[i], "hidden")) none = 1;
        else if (!strcasecmp(tk[i], "thin")) w = 1;
        else if (!strcasecmp(tk[i], "medium")) w = 3;
        else if (!strcasecmp(tk[i], "thick")) w = 5;
        else if (parse_len(tk[i], &v, &k) && k == K_PX) w = v;
        else if (css_color(tk[i], &c, 0)) hasc = 1;
    }
    if (none) w = 0;
    else if (w < 1) w = 1;
    if (w > 6) w = 6;
    for (int s = 0; s < 4; s++) {
        if (side >= 0 && s != side) continue;
        put(P_BWT + s, K_PX, w, imp);
        if (hasc) put(P_BCT + s, K_PX, c, imp);
    }
}

static void decl(char *name, char *val, int imp) {
    char *tk[8];
    color_t c;
    int v, k;
    if (!strcmp(name, "color")) { if (css_color(val, &c, 0) && c) put(P_COLOR, K_PX, c, imp); }
    else if (!strcmp(name, "background-color")) { if (css_color(val, &c, 0)) put(P_BG, K_PX, c, imp); }
    else if (!strcmp(name, "background")) {
        int n = tokens(val, tk, 8), found = 0;
        for (int i = 0; i < n; i++) if (css_color(tk[i], &c, 0)) { put(P_BG, K_PX, c, imp); found = 1; }
        if (!found && n && !strcasecmp(tk[0], "none")) put(P_BG, K_PX, 0, imp);
    }
    else if (!strcmp(name, "font-weight")) put(P_BOLD, K_PX, is_bold(val) ? 1 : 0, imp);
    else if (!strcmp(name, "font-style")) put(P_ITALIC, K_PX, !strncasecmp(val, "italic", 6) || !strncasecmp(val, "oblique", 7), imp);
    else if (!strcmp(name, "font-family")) { for (char *p = val; *p; p++) *p = tolower((unsigned char)*p); put(P_MONO, K_PX, strstr(val, "mono") || strstr(val, "courier") ? 1 : 0, imp); }
    else if (!strcmp(name, "font")) {
        for (char *p = val; *p; p++) *p = tolower((unsigned char)*p);
        int n = tokens(val, tk, 8);
        for (int i = 0; i < n; i++) {
            if (is_bold(tk[i])) put(P_BOLD, K_PX, 1, imp);
            else if (strstr(tk[i], "mono") || strstr(tk[i], "courier")) put(P_MONO, K_PX, 1, imp);
            else if (parse_len(tk[i], &v, &k) && k != K_AUTO && (v > 5 || k == K_PCT)) put(P_FSIZE, K_PX, k == K_PCT ? v * 16 / 100 : v, imp);
        }
    }
    else if (!strcmp(name, "font-size")) {
        static const struct { const char *n; int px; } KW[] = { { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 },
            { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 }, { "larger", 20 }, { "smaller", 13 } };
        int done = 0;
        for (unsigned i = 0; i < sizeof KW / sizeof *KW; i++) if (!strcasecmp(val, KW[i].n)) { put(P_FSIZE, K_PX, KW[i].px, imp); done = 1; }
        if (!done && parse_len(val, &v, &k) && k != K_AUTO) put(P_FSIZE, K_PX, k == K_PCT ? v * 16 / 100 : v, imp);
    }
    else if (!strcmp(name, "text-align")) put(P_ALIGN, K_PX, !strcasecmp(val, "center") ? 1 : !strcasecmp(val, "right") || !strcasecmp(val, "end") ? 2 : 0, imp);
    else if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line")) {
        int m = 0, n = tokens(val, tk, 8);
        for (int i = 0; i < n; i++) { if (!strcasecmp(tk[i], "underline")) m |= 1; else if (!strcasecmp(tk[i], "line-through")) m |= 2; }
        put(P_DECO, K_PX, m, imp);
    }
    else if (!strcmp(name, "display")) {
        int d = 0;
        if (!strcasecmp(val, "none")) d = D_NONE;
        else if (!strncasecmp(val, "inline", 6)) d = D_INLINE;
        else if (*val) d = D_BLOCK;
        if (d) put(P_DISPLAY, K_PX, d, imp);
    }
    else if (!strcmp(name, "margin")) box4(P_MT, val, imp, 1);
    else if (!strcmp(name, "padding")) box4(P_PT, val, imp, 0);
    else if (!strncmp(name, "margin-", 7) || !strncmp(name, "padding-", 8)) {
        int m = name[0] == 'm', s = side_of(name + (m ? 7 : 8), strlen(name) - (m ? 7 : 8));
        if (s >= 0 && parse_len(val, &v, &k) && (m || k != K_AUTO)) put((m ? P_MT : P_PT) + s, k, v, imp);
    }
    else if (!strcmp(name, "width") || !strcmp(name, "max-width") || !strcmp(name, "height")) {
        int p = name[0] == 'w' ? P_WIDTH : name[0] == 'h' ? P_HEIGHT : P_MAXW;
        if (!strcasecmp(val, "none")) put(p, K_AUTO, 0, imp);
        else if (parse_len(val, &v, &k)) put(p, k, v, imp);
    }
    else if (!strcmp(name, "border")) border(-1, val, imp);
    else if (!strncmp(name, "border-", 7)) {
        const char *r = name + 7;
        const char *dash = strchr(r, '-');
        int s = side_of(r, dash ? (int)(dash - r) : (int)strlen(r));
        if (s >= 0 && !dash) border(s, val, imp);
        else if (s >= 0 && !strcmp(dash, "-width")) { if (parse_len(val, &v, &k) && k == K_PX) put(P_BWT + s, K_PX, v ? (v < 1 ? 1 : v > 6 ? 6 : v) : 0, imp); }
        else if (s >= 0 && !strcmp(dash, "-color")) { if (css_color(val, &c, 0)) put(P_BCT + s, K_PX, c, imp); }
        else if (s >= 0 && !strcmp(dash, "-style")) { if (!strcasecmp(val, "none") || !strcasecmp(val, "hidden")) put(P_BWT + s, K_PX, 0, imp); }
        else if (s < 0 && !strcmp(r, "color")) { if (css_color(val, &c, 0)) for (int i = 0; i < 4; i++) put(P_BCT + i, K_PX, c, imp); }
        else if (s < 0 && !strcmp(r, "width")) { if (parse_len(val, &v, &k) && k == K_PX) for (int i = 0; i < 4; i++) put(P_BWT + i, K_PX, v ? (v < 1 ? 1 : v > 6 ? 6 : v) : 0, imp); }
        else if (s < 0 && !strcmp(r, "style")) { if (!strcasecmp(val, "none") || !strcasecmp(val, "hidden")) for (int i = 0; i < 4; i++) put(P_BWT + i, K_PX, 0, imp); }
    }
    else if (!strcmp(name, "list-style") || !strcmp(name, "list-style-type")) put(P_LIST, K_PX, strstr(val, "none") != 0, imp);
}

/* name: value; name: value !important ... */
static void parse_decls(const char *s, size_t n) {
    char *buf = malloc(n + 1);
    if (!buf) return;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *p = buf;
    while (*p) {
        char *semi = p;
        int depth = 0;
        char q = 0;
        for (; *semi && (q || depth || *semi != ';'); semi++) {          /* ';' inside "url(..;..)" or strings doesn't end it */
            if (q) { if (*semi == q) q = 0; }
            else if (*semi == '"' || *semi == '\'') q = *semi;
            else if (*semi == '(') depth++;
            else if (*semi == ')' && depth) depth--;
        }
        char *end = semi;
        if (*semi) *semi++ = 0;
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            char *name = p, *val = colon + 1;
            while (isspace((unsigned char)*name)) name++;
            char *ne = name + strlen(name);
            while (ne > name && isspace((unsigned char)ne[-1])) *--ne = 0;
            for (char *c = name; *c; c++) *c = tolower((unsigned char)*c);
            while (isspace((unsigned char)*val)) val++;
            char *ve = end;
            while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0;
            int imp = 0;
            char *bang = strrchr(val, '!');
            if (bang && !strncasecmp(bang + 1, "important", 9)) {
                imp = 1;
                *bang = 0;
                for (ve = bang; ve > val && isspace((unsigned char)ve[-1]); ) *--ve = 0;
            }
            if (*name && *val) decl(name, val, imp);
        }
        p = semi;
    }
    free(buf);
}

/* ---------- selectors ---------- */
static int parse_sel(const char *s, int n, struct rule *r) {
    memset(r, 0, sizeof *r);
    int i = 0, spec = 0, child = 0;
    while (i < n) {
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i >= n) break;
        if (s[i] == '>') { child = 1; i++; continue; }
        if (r->n == 5) return 0;
        struct comp *c = &r->c[r->n];
        c->child = child; child = 0;
        int any = 0;
        while (i < n && !isspace((unsigned char)s[i]) && s[i] != '>') {
            char ch = s[i];
            if (ch == '*') { i++; any = 1; continue; }
            if (ch == '#' || ch == '.' || isalpha((unsigned char)ch) || ch == '_' || ch == '-') {
                int j = i + (ch == '#' || ch == '.');
                int st = j;
                while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) j++;
                int l = j - st;
                if (!l) return 0;
                if (ch == '#') { if (l >= (int)sizeof c->id) return 0; memcpy(c->id, s + st, l); spec += 100; }
                else if (ch == '.') { if (c->ncls == 3 || l >= (int)sizeof c->cls[0]) return 0; memcpy(c->cls[c->ncls++], s + st, l); spec += 10; }
                else { if (l >= (int)sizeof c->tag) return 0; for (int k = 0; k < l; k++) c->tag[k] = tolower((unsigned char)s[st + k]); spec += 1; }
                any = 1;
                i = j;
                continue;
            }
            return 0;                                   /* : [ + ~ and friends: not supported */
        }
        if (!any) return 0;
        r->n++;
    }
    if (!r->n || child) return 0;
    r->spec = spec > 1023 ? 1023 : spec;
    return 1;
}

static int has_class(const char *list, const char *cls) {
    int l = strlen(cls);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e - p == l && !strncmp(p, cls, l)) return 1;
        p = e;
    }
    return 0;
}

static int comp_match(const struct comp *c, const struct el *e) {
    if (c->tag[0] && strcmp(c->tag, e->name)) return 0;
    if (c->id[0] && strcmp(c->id, e->id)) return 0;
    for (int i = 0; i < c->ncls; i++) if (!has_class(e->cls, c->cls[i])) return 0;
    return 1;
}

/* match compounds c[0..i] with c[i] against st[j]; ancestors to the left */
static int match_from(const struct rule *r, int i, const struct el *st, int j) {
    if (!comp_match(&r->c[i], &st[j])) return 0;
    if (!i) return 1;
    if (r->c[i].child) return j > 0 && match_from(r, i - 1, st, j - 1);
    for (int a = j - 1; a >= 0; a--) if (match_from(r, i - 1, st, a)) return 1;
    return 0;
}

/* ---------- stylesheet ---------- */
void css_add(const char *src, size_t n) {
    char *s = malloc(n + 1);
    if (!s) return;
    size_t m = 0;
    for (size_t i = 0; i < n; ) {                       /* drop comments and <!-- --> wrappers */
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
        } else if (i + 4 <= n && !strncmp(src + i, "<!--", 4)) i += 4;
        else if (i + 3 <= n && !strncmp(src + i, "-->", 3)) i += 3;
        else s[m++] = src[i++];
    }
    s[m] = 0;
    size_t i = 0;
    while (i < m) {
        while (i < m && isspace((unsigned char)s[i])) i++;
        if (i >= m) break;
        if (s[i] == '@') {                              /* skip @media/@import/@font-face ... */
            while (i < m && s[i] != '{' && s[i] != ';') i++;
            if (i < m && s[i] == ';') { i++; continue; }
            int depth = 0;
            for (; i < m; i++) { if (s[i] == '{') depth++; else if (s[i] == '}' && --depth == 0) { i++; break; } }
            continue;
        }
        size_t ps = i;
        while (i < m && s[i] != '{') i++;
        if (i >= m) break;
        size_t pe = i, bs = ++i;
        int depth = 1;
        while (i < m && depth) { if (s[i] == '{') depth++; else if (s[i] == '}') depth--; i++; }
        size_t be = depth ? i : i - 1;
        int d0 = nd;
        parse_decls(s + bs, be - bs);
        if (nd == d0) continue;
        for (size_t a = ps; a <= pe; ) {                  /* comma list */
            size_t b = a;
            int br = 0;
            while (b < pe && (br || s[b] != ',')) { if (s[b] == '[') br++; else if (s[b] == ']') br = 0; b++; }
            if (nr == rcap) {
                int nc = rcap ? rcap * 2 : 128;
                struct rule *nrr = realloc(R, nc * sizeof *nrr);
                if (!nrr) break;
                R = nrr; rcap = nc;
            }
            if (nr < 20000 && parse_sel(s + a, b - a, &R[nr])) { R[nr].d0 = d0; R[nr].nd = nd - d0; R[nr].order = nr; nr++; }
            a = b + 1;
        }
    }
    free(s);
}

static void apply(struct cprops *o, const struct decl *d, int imp) {
    if (d->imp != imp) return;
    o->set |= 1u << d->prop;
    o->v[d->prop] = d->v;
    o->k[d->prop] = d->kind;
}

void css_cascade(const struct el *st, int n, const char *style, struct cprops *o) {
    memset(o, 0, sizeof *o);
    int hit[256], nh = 0;
    uint32_t key[256];
    for (int i = 0; i < nr && nh < 256; i++) {
        if (!match_from(&R[i], R[i].n - 1, st, n - 1)) continue;
        uint32_t k = (uint32_t)R[i].spec << 20 | (uint32_t)R[i].order;
        int p = nh++;
        while (p && key[p - 1] > k) { key[p] = key[p - 1]; hit[p] = hit[p - 1]; p--; }
        key[p] = k; hit[p] = i;
    }
    int keep = nd, has_style = style && *style;
    if (has_style) parse_decls(style, strlen(style));
    for (int imp = 0; imp < 2; imp++) {                 /* !important last; inline style after the sheet */
        for (int h = 0; h < nh; h++)
            for (int j = 0; j < R[hit[h]].nd; j++) apply(o, &D[R[hit[h]].d0 + j], imp);
        for (int j = keep; j < nd; j++) apply(o, &D[j], imp);
    }
    nd = keep;
}
