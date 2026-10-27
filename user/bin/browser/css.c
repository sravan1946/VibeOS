/* css.c - a small CSS engine: stylesheet parser, selector matcher (type, .class, #id, [attr], :first-child, :root,
 * descendant, child, comma lists) and a cascade that boils the matching declarations of one element down to a
 * `struct cprops`. @media (screen/all, width queries against the window), @import and @layer are followed; anything
 * else it doesn't understand (other pseudo-classes, sibling combinators, other @rules, unknown properties,
 * vw/calc values) is skipped without disturbing the rest of the sheet. ponytail: :last-child needs lookahead the
 * streaming layout doesn't have. */
#include "browser.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct decl { uint8_t prop, imp, kind; int v; };
struct comp { char tag[12], id[24], cls[3][20], an[14], av[24]; uint8_t ncls, child, hasa, aop, aci, first, root; };   /* child: '>' to the previous compound; [an aop av] */
struct rule { struct comp c[5]; uint8_t n; uint16_t spec; int d0, nd, order; };
static struct rule *R; static int nr, rcap;
static struct decl *D; static int nd, dcap;
static int attrs_used, vpw = 960;

void css_reset(void) { nr = nd = 0; attrs_used = 0; }
int  css_attrs(void) { return attrs_used; }
void css_viewport(int w) { vpw = w; }
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
    else if (!strcmp(name, "vertical-align")) {
        int a = !strcasecmp(val, "top") || !strcasecmp(val, "text-top") || !strcasecmp(val, "baseline") ? 0 : !strcasecmp(val, "middle") ? 1 : !strcasecmp(val, "bottom") || !strcasecmp(val, "text-bottom") ? 2 : -1;
        if (a >= 0) put(P_VALIGN, K_PX, a, imp);
    }
    else if (!strcmp(name, "border-collapse")) put(P_COLLAPSE, K_PX, !strncasecmp(val, "collapse", 8), imp);
    else if (!strcmp(name, "border-spacing")) { if (parse_len(val, &v, &k) && k == K_PX) put(P_SPACING, K_PX, v < 0 ? 0 : v, imp); }
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
            if (ch == '[') {                            /* [name], [name=v], ~= |= ^= $= *=, optional quotes and i flag */
                int j = i + 1;
                while (j < n && isspace((unsigned char)s[j])) j++;
                int ns = j;
                while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_' || s[j] == ':')) j++;
                int nl = j - ns;
                if (!nl || nl >= (int)sizeof c->an || c->hasa) return 0;
                for (int k = 0; k < nl; k++) c->an[k] = tolower((unsigned char)s[ns + k]);
                while (j < n && isspace((unsigned char)s[j])) j++;
                if (j < n && s[j] != ']') {
                    if (s[j] == '=') c->aop = '=';
                    else if (j + 1 < n && s[j + 1] == '=' && strchr("~|^$*", s[j])) c->aop = s[j++];
                    else return 0;
                    j++;
                    while (j < n && isspace((unsigned char)s[j])) j++;
                    char q = j < n && (s[j] == '"' || s[j] == '\'') ? s[j++] : 0;
                    int vs = j;
                    while (j < n && (q ? s[j] != q : s[j] != ']' && !isspace((unsigned char)s[j]))) j++;
                    if (j - vs >= (int)sizeof c->av) return 0;
                    memcpy(c->av, s + vs, j - vs);
                    if (q && j < n) j++;
                    while (j < n && isspace((unsigned char)s[j])) j++;
                    if (j < n && (s[j] | 32) == 'i') { c->aci = 1; j++; while (j < n && isspace((unsigned char)s[j])) j++; }
                }
                if (j >= n || s[j] != ']') return 0;
                c->hasa = 1; spec += 10; attrs_used = 1; any = 1;
                i = j + 1;
                continue;
            }
            if (ch == ':') {                            /* :first-child and :root; every other pseudo-class drops the rule */
                int j = i + 1, st = j;
                while (j < n && (isalpha((unsigned char)s[j]) || s[j] == '-')) j++;
                if (j - st == 11 && !strncmp(s + st, "first-child", 11)) c->first = 1;
                else if (j - st == 4 && !strncmp(s + st, "root", 4)) c->root = 1;
                else return 0;
                spec += 10; any = 1;
                i = j;
                continue;
            }
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

static const char *at_find(const char *at, const char *name, int *vl) {      /* value of attribute `name` in el.at */
    int nl = strlen(name);
    for (const char *p = at; *p == 1; ) {
        const char *n = p + 1, *v = strchr(n, 2);
        if (!v) break;
        const char *e = v + 1;
        while (*e && *e != 1) e++;
        if ((int)(v - n) == nl && !strncmp(n, name, nl)) { *vl = e - (v + 1); return v + 1; }
        p = e;
    }
    return 0;
}

static int attr_match(const struct comp *c, const struct el *e) {
    int vl, cl = strlen(c->av);
    const char *v = at_find(e->at, c->an, &vl);
    if (!v) return 0;
    if (!c->aop) return 1;
    #define EQ(a, b, l) (c->aci ? !strncasecmp(a, b, l) : !strncmp(a, b, l))
    switch (c->aop) {
    case '=': return vl == cl && EQ(v, c->av, cl);
    case '^': return cl && vl >= cl && EQ(v, c->av, cl);
    case '$': return cl && vl >= cl && EQ(v + vl - cl, c->av, cl);
    case '|': return vl >= cl && EQ(v, c->av, cl) && (vl == cl || v[cl] == '-');
    case '*': for (int k = 0; cl && k + cl <= vl; k++) if (EQ(v + k, c->av, cl)) return 1; return 0;
    case '~': for (int k = 0; cl && k + cl <= vl; k++)
                  if ((!k || isspace((unsigned char)v[k - 1])) && EQ(v + k, c->av, cl) && (k + cl == vl || isspace((unsigned char)v[k + cl]))) return 1;
              return 0;
    }
    #undef EQ
    return 0;
}

static int comp_match(const struct comp *c, const struct el *e) {
    if (c->tag[0] && strcmp(c->tag, e->name)) return 0;
    if (c->id[0] && strcmp(c->id, e->id)) return 0;
    for (int i = 0; i < c->ncls; i++) if (!has_class(e->cls, c->cls[i])) return 0;
    if (c->first && !e->first) return 0;
    if (c->root && strcmp(e->name, "html")) return 0;
    if (c->hasa && !attr_match(c, e)) return 0;
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

/* querySelector: does the comma list `sel` match st[n-1] (its ancestors before it)? -1 when no part parses */
int css_match(const char *sel, const struct el *st, int n) {
    struct rule r;
    int ok = 0, len = strlen(sel);
    for (int a = 0; a <= len; ) {
        int b = a;
        while (b < len && sel[b] != ',') b++;
        if (parse_sel(sel + a, b - a, &r)) { ok = 1; if (match_from(&r, r.n - 1, st, n - 1)) return 1; }
        a = b + 1;
    }
    return ok ? 0 : -1;
}

/* ---------- @media ---------- */
static int cmp_len(int op, int have, int want) {    /* have OP want, for the range syntax and min-/max- */
    return op == '<' ? have < want : op == 'l' ? have <= want : op == '>' ? have > want : op == 'g' ? have >= want : have == want;
}

/* one "(feature)" without the parentheses */
static int media_feature(char *f) {
    for (char *p = f; *p; p++) *p = tolower((unsigned char)*p);
    char *colon = strchr(f, ':'), *val = "";
    int op = '=', v = 0, k;
    if (colon) { *colon = 0; val = colon + 1; }
    else {                                            /* (width >= 600px), (400px <= width) is not handled */
        char *r = f + strcspn(f, "<>=");
        if (*r) {
            op = *r == '=' ? '=' : r[1] == '=' ? (*r == '<' ? 'l' : 'g') : *r;
            val = r + 1 + (r[1] == '=' && *r != '=');
            *r = 0;
        }
    }
    char *e = f + strlen(f);
    while (e > f && isspace((unsigned char)e[-1])) *--e = 0;
    while (isspace((unsigned char)*f)) f++;
    while (isspace((unsigned char)*val)) val++;
    int num = parse_len(val, &v, &k) && k == K_PX;
    if (!strcmp(f, "width")) return num && cmp_len(op, vpw, v);
    if (!strcmp(f, "min-width")) return num && vpw >= v;
    if (!strcmp(f, "max-width")) return num && vpw <= v;
    if (!strcmp(f, "height")) return num && cmp_len(op, 600, v);          /* the window height isn't known here */
    if (!strcmp(f, "min-height")) return num && 600 >= v;
    if (!strcmp(f, "max-height")) return num && 600 <= v;
    if (!strcmp(f, "orientation")) return !strncmp(val, "landscape", 9) ? vpw >= 600 : vpw < 600;
    if (!strcmp(f, "prefers-color-scheme")) return !strncmp(val, "light", 5);
    if (!strcmp(f, "prefers-reduced-motion")) return !strncmp(val, "no-pref", 7);
    if (!strcmp(f, "hover") || !strcmp(f, "any-hover")) return !strncmp(val, "hover", 5);
    if (!strcmp(f, "pointer") || !strcmp(f, "any-pointer")) return !strncmp(val, "fine", 4);
    if (strstr(f, "resolution") || strstr(f, "pixel-ratio")) {                 /* we are always 1 dppx / 96 dpi */
        double x = atof(val);
        if (strstr(val, "dpi")) x /= 96;
        return strstr(f, "min") ? x <= 1.0001 : strstr(f, "max") ? x >= 0.9999 : x > 0.9999 && x < 1.0001;
    }
    if (!strcmp(f, "color") || !strcmp(f, "grid")) return !strcmp(f, "color");
    return 0;                                         /* unknown feature: doesn't match, like a browser */
}

static int media_one(const char *q, int n) {          /* [not|only] type [and (feature)]... */
    char b[256];
    if (n >= (int)sizeof b) n = sizeof b - 1;
    memcpy(b, q, n);
    b[n] = 0;
    char *p = b;
    int neg = 0, ok = 1;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '(') {
            char *e = strchr(p, ')');
            if (!e) return 0;
            *e = 0;
            ok &= media_feature(p + 1);
            p = e + 1;
            continue;
        }
        char *w = p;
        while (*p && !isspace((unsigned char)*p) && *p != '(') p++;
        int wl = p - w;
        if (wl == 3 && !strncasecmp(w, "not", 3)) neg = 1;
        else if (wl == 4 && !strncasecmp(w, "only", 4)) { }
        else if (wl == 3 && !strncasecmp(w, "and", 3)) { }
        else if (!((wl == 3 && !strncasecmp(w, "all", 3)) || (wl == 6 && !strncasecmp(w, "screen", 6)))) ok = 0;   /* print, speech, tv... */
    }
    return ok != neg;
}

int css_media(const char *q) {                        /* comma list: any query matching is enough; empty = all */
    if (!q) return 1;
    int len = strlen(q), any = 0;
    for (int a = 0; a <= len; ) {
        int b = a, depth = 0;
        while (b < len && (depth || q[b] != ',')) { if (q[b] == '(') depth++; else if (q[b] == ')' && depth) depth--; b++; }
        int e = b;
        while (a < e && isspace((unsigned char)q[a])) a++;
        if (e == a && len == 0) return 1;
        if (e > a) { any = 1; if (media_one(q + a, e - a)) return 1; }
        a = b + 1;
    }
    return !any;
}

/* ---------- stylesheet ---------- */
static void sheet(const char *s, size_t m, const char *base, int depth);

/* @import "x" / url(x) [media]; the sheet comes from sheet.c (NULL while it loads: the page relayouts when it arrives) */
static void do_import(const char *pre, size_t n, const char *base, int depth) {
    char ref[URL_MAX], q[160];
    size_t i = 0;
    while (i < n && isspace((unsigned char)pre[i])) i++;
    int k = 0;
    if (i + 4 <= n && !strncasecmp(pre + i, "url(", 4)) {
        i += 4;
        while (i < n && isspace((unsigned char)pre[i])) i++;
        char c = i < n && (pre[i] == '"' || pre[i] == '\'') ? pre[i++] : 0;
        while (i < n && k < (int)sizeof ref - 1 && (c ? pre[i] != c : pre[i] != ')' && !isspace((unsigned char)pre[i]))) ref[k++] = pre[i++];
        while (i < n && pre[i] != ')') i++;
        i++;
    } else if (i < n && (pre[i] == '"' || pre[i] == '\'')) {
        char c = pre[i++];
        while (i < n && pre[i] != c && k < (int)sizeof ref - 1) ref[k++] = pre[i++];
        i++;
    } else return;
    ref[k] = 0;
    k = 0;
    while (i < n && isspace((unsigned char)pre[i])) i++;
    if (i + 5 <= n && (!strncasecmp(pre + i, "layer", 5) || !strncasecmp(pre + i, "suppo", 5))) i = n;   /* import layers/supports: ignore the condition */
    while (i < n && k < (int)sizeof q - 1) q[k++] = pre[i++];
    q[k] = 0;
    if (!ref[0] || !css_media(q) || depth >= 4) return;
    char full[URL_MAX];
    url_resolve(base ? base : "", ref, full, sizeof full);
    size_t len;
    const char *t = sheet_get(full, &len);
    if (t) { static int nest; if (nest < 4) { nest++; css_add_from(t, len, full); nest--; } }
}

static void sheet(const char *s, size_t m, const char *base, int depth) {
    size_t i = 0;
    while (i < m) {
        while (i < m && isspace((unsigned char)s[i])) i++;
        if (i >= m) break;
        if (s[i] == '@') {
            size_t ns = ++i;
            while (i < m && (isalpha((unsigned char)s[i]) || s[i] == '-')) i++;
            size_t nl = i - ns, ps = i;
            int par = 0;
            char qt = 0;
            for (; i < m && (qt || par || (s[i] != '{' && s[i] != ';')); i++) {
                if (qt) { if (s[i] == qt) qt = 0; }
                else if (s[i] == '"' || s[i] == '\'') qt = s[i];
                else if (s[i] == '(') par++;
                else if (s[i] == ')' && par) par--;
            }
            size_t pe = i;
            if (i >= m) break;
            if (s[i] == ';') {
                if (nl == 6 && !strncasecmp(s + ns, "import", 6)) do_import(s + ps, pe - ps, base, depth);
                i++;
                continue;
            }
            size_t bs = ++i;
            int d = 1;
            while (i < m && d) { if (s[i] == '{') d++; else if (s[i] == '}') d--; i++; }
            size_t be = d ? i : i - 1;
            if (depth < 6 && ((nl == 5 && !strncasecmp(s + ns, "media", 5)) || (nl == 5 && !strncasecmp(s + ns, "layer", 5)))) {
                char mq[256];
                size_t l = pe - ps < sizeof mq - 1 ? pe - ps : sizeof mq - 1;
                memcpy(mq, s + ps, l);
                mq[l] = 0;
                if (nl == 5 && (s[ns] | 32) == 'l') mq[0] = 0;                 /* @layer name { ... }: always applies */
                if (css_media(mq)) sheet(s + bs, be - bs, base, depth + 1);
            }
            continue;
        }
        size_t ps = i;
        while (i < m && s[i] != '{') i++;
        if (i >= m) break;
        size_t pe = i, bs = ++i;
        int depth2 = 1;
        while (i < m && depth2) { if (s[i] == '{') depth2++; else if (s[i] == '}') depth2--; i++; }
        size_t be = depth2 ? i : i - 1;
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
}

void css_add(const char *src, size_t n) { css_add_from(src, n, 0); }

void css_add_from(const char *src, size_t n, const char *base) {
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
    sheet(s, m, base, 0);
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
