/* css.c - the CSS engine: stylesheet parser (rules, @media, @supports, @import, @layer), declaration parser (values, units,
 * calc(), var(), colours, shorthands), the user-agent sheet, and the cascade that turns the matching declarations of one
 * element into a `struct cprops`. Selectors live in cssel.c.
 *
 * Two ways in. css_compute() is the DOM-tree API: it needs the parent's computed style and returns every property
 * computed (inherited, initial, em/rem/vw resolved, var() substituted, UA defaults applied). css_cascade() is the old
 * streaming entry (html.c): only what author rules declare, in the old encoding, no inheritance, no UA sheet.
 *
 * Speed: rules are bucketed by the rightmost compound's id / first class / tag; each element looks up its own id,
 * classes and tag, so only a handful of rules are tried. Each element's cprops carries a bloom filter of its
 * ancestors' tags/ids/classes so a descendant selector whose ancestors can't be present is rejected without a walk. */
#include "cssi.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* ---------- interned strings ---------- */
char *SP; static int sp_n, sp_cap;
static int *HT; static int ht_cap, ht_n;                 /* open addressing; entries are pool offsets (0 = empty; offset 0 is "") */

static void ht_put(int off) {
    unsigned m = ht_cap - 1, i = hash_str(SP + off) & m;
    while (HT[i]) i = (i + 1) & m;
    HT[i] = off;
}

int intern(const char *s, int n) {
    if (n <= 0) return 0;
    if (!SP) { SP = malloc(sp_cap = 1 << 16); if (!SP) return 0; SP[0] = 0; sp_n = 1; }
    if (!HT) { HT = calloc(ht_cap = 4096, sizeof *HT); if (!HT) return 0; }
    if (ht_n * 2 >= ht_cap) {                            /* grow at half load */
        int *old = HT, oc = ht_cap;
        int *nh = calloc(oc * 2, sizeof *nh);
        if (!nh) return 0;
        HT = nh; ht_cap = oc * 2;
        for (int i = 0; i < oc; i++) if (old[i]) ht_put(old[i]);
        free(old);
    }
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    unsigned m = ht_cap - 1, i = h & m;
    while (HT[i]) {
        const char *t = SP + HT[i];
        if (!strncmp(t, s, n) && !t[n]) return HT[i];
        i = (i + 1) & m;
    }
    if (sp_n + n + 1 > (16 << 20)) return 0;
    if (sp_n + n + 1 > sp_cap) {
        int nc = sp_cap * 2;
        while (nc < sp_n + n + 1) nc *= 2;
        char *p = realloc(SP, nc);
        if (!p) return 0;
        SP = p; sp_cap = nc;
    }
    int off = sp_n;
    memcpy(SP + off, s, n);
    SP[off + n] = 0;
    sp_n += n + 1;
    HT[i] = off; ht_n++;
    return off;
}
const char *css_str(int id) { return SP ? SP + id : ""; }

/* ---------- state ---------- */
struct decl { uint8_t prop, imp, kind; int v, a; };
_Static_assert(P_N <= 250, "decl.prop is a byte and P_CUSTOM/P_RAWDECL live above the property ids; cascade masks hold 256 bits");
#define P_CUSTOM 250                                    /* --name: raw value (a = name, v = value) */
#define P_RAWDECL 251                                   /* longhand/shorthand whose value has var(): a = property name, v = value text */
struct rule { int sel, d0, nd, order, next, scope; uint32_t kh; uint8_t layer, ua, bk; };   /* bk/kh: the bucket (1 id, 2 class, 3 tag, 0 any) and key hash the rule is chained under */
static struct rule *R; static int nr, rcap;
static struct decl *D; static int nd, dcap;
#define HB 4096
static int hid[HB], hcl[HB], htg[HB], huni;           /* rule index + 1 chains */
static int vpw = 960, vph = 600;
static int order_n, cur_layer = 255, cur_ua, any_layers, cur_scope, cur_nscope;
static char cur_base[URL_MAX], page_base[URL_MAX];
static int lay_name[128], nlay;
static int ua_ready, ua_nr, ua_nd, ua_si, ua_co, ua_se;
static struct cdep ua_cd;

void css_viewport(int w) { vpw = w; }
void css_viewport_size(int w, int h) { vpw = w; vph = h; }
void css_base(const char *b) { strlcpy(page_base, b ? b : "", sizeof page_base); }
int  css_attrs(void) { return attrs_used; }
int  css_any(void) { return nr > ua_nr; }
void css_dom_set(const struct css_dom *d) { DOM = d; }
void css_stats(void);

/* ---------- colours ---------- */
static const char NAMED[] =
    "aliceblue F0F8FF antiquewhite FAEBD7 aqua 00FFFF aquamarine 7FFFD4 azure F0FFFF beige F5F5DC bisque FFE4C4 black 000000 blanchedalmond FFEBCD "
    "blue 0000FF blueviolet 8A2BE2 brown A52A2A burlywood DEB887 cadetblue 5F9EA0 chartreuse 7FFF00 chocolate D2691E coral FF7F50 "
    "cornflowerblue 6495ED cornsilk FFF8DC crimson DC143C cyan 00FFFF darkblue 00008B darkcyan 008B8B darkgoldenrod B8860B darkgray A9A9A9 "
    "darkgreen 006400 darkgrey A9A9A9 darkkhaki BDB76B darkmagenta 8B008B darkolivegreen 556B2F darkorange FF8C00 darkorchid 9932CC "
    "darkred 8B0000 darksalmon E9967A darkseagreen 8FBC8F darkslateblue 483D8B darkslategray 2F4F4F darkslategrey 2F4F4F darkturquoise 00CED1 "
    "darkviolet 9400D3 deeppink FF1493 deepskyblue 00BFFF dimgray 696969 dimgrey 696969 dodgerblue 1E90FF firebrick B22222 floralwhite FFFAF0 "
    "forestgreen 228B22 fuchsia FF00FF gainsboro DCDCDC ghostwhite F8F8FF gold FFD700 goldenrod DAA520 gray 808080 green 008000 "
    "greenyellow ADFF2F grey 808080 honeydew F0FFF0 hotpink FF69B4 indianred CD5C5C indigo 4B0082 ivory FFFFF0 khaki F0E68C lavender E6E6FA "
    "lavenderblush FFF0F5 lawngreen 7CFC00 lemonchiffon FFFACD lightblue ADD8E6 lightcoral F08080 lightcyan E0FFFF lightgoldenrodyellow FAFAD2 "
    "lightgray D3D3D3 lightgreen 90EE90 lightgrey D3D3D3 lightpink FFB6C1 lightsalmon FFA07A lightseagreen 20B2AA lightskyblue 87CEFA "
    "lightslategray 778899 lightslategrey 778899 lightsteelblue B0C4DE lightyellow FFFFE0 lime 00FF00 limegreen 32CD32 linen FAF0E6 "
    "magenta FF00FF maroon 800000 mediumaquamarine 66CDAA mediumblue 0000CD mediumorchid BA55D3 mediumpurple 9370DB mediumseagreen 3CB371 "
    "mediumslateblue 7B68EE mediumspringgreen 00FA9A mediumturquoise 48D1CC mediumvioletred C71585 midnightblue 191970 mintcream F5FFFA "
    "mistyrose FFE4E1 moccasin FFE4B5 navajowhite FFDEAD navy 000080 oldlace FDF5E6 olive 808000 olivedrab 6B8E23 orange FFA500 "
    "orangered FF4500 orchid DA70D6 palegoldenrod EEE8AA palegreen 98FB98 paleturquoise AFEEEE palevioletred DB7093 papayawhip FFEFD5 "
    "peachpuff FFDAB9 peru CD853F pink FFC0CB plum DDA0DD powderblue B0E0E6 purple 800080 rebeccapurple 663399 red FF0000 rosybrown BC8F8F "
    "royalblue 4169E1 saddlebrown 8B4513 salmon FA8072 sandybrown F4A460 seagreen 2E8B57 seashell FFF5EE sienna A0522D silver C0C0C0 "
    "skyblue 87CEEB slateblue 6A5ACD slategray 708090 slategrey 708090 snow FFFAFA springgreen 00FF7F steelblue 4682B4 tan D2B48C teal 008080 "
    "thistle D8BFD8 tomato FF6347 turquoise 40E0D0 violet EE82EE wheat F5DEB3 white FFFFFF whitesmoke F5F5F5 yellow FFFF00 yellowgreen 9ACD32 "
    "canvas FFFFFF canvastext 000000 linktext 0000EE visitedtext 551A8B activetext FF0000 field FFFFFF fieldtext 000000 buttonface EFEFEF "
    "buttontext 000000 buttonborder 767676 highlight 3584E4 highlighttext FFFFFF graytext 6D6D6D mark FFFF00 marktext 000000 ";

static double hue_ch(double t) {
    if (t < 0) t += 1; else if (t > 1) t -= 1;
    return t;
}
static double hue2rgb(double p, double q, double t) {
    t = hue_ch(t);
    return t < 1. / 6 ? p + (q - p) * 6 * t : t < .5 ? q : t < 2. / 3 ? p + (q - p) * (2. / 3 - t) * 6 : p;
}
static int cl255(double f) { return f < 0 ? 0 : f > 255 ? 255 : (int)(f + .5); }

/* one number with optional unit/percent at *p; returns 0 if none. pct set when '%'; unit string (deg, turn...) copied to u */
static int cnum(const char **pp, double *val, int *pct, char *u) {
    const char *p = *pp;
    while (isspace((unsigned char)*p)) p++;
    const char *st = p;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1]))) return 0;
    double x = 0;
    while (isdigit((unsigned char)*p)) x = x * 10 + *p++ - '0';
    if (*p == '.') { p++; for (double f = .1; isdigit((unsigned char)*p); p++, f /= 10) x += (*p - '0') * f; }
    if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) || ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
        int sg = 1, ex = 0;
        p++;
        if (*p == '-') { sg = -1; p++; } else if (*p == '+') p++;
        while (isdigit((unsigned char)*p)) ex = ex * 10 + *p++ - '0';
        while (ex-- > 0) x = sg > 0 ? x * 10 : x / 10;
    }
    if (*st == '-') x = -x;
    *pct = 0;
    if (u) u[0] = 0;
    if (*p == '%') { *pct = 1; p++; }
    else if (u && isalpha((unsigned char)*p)) { int k = 0; while (isalpha((unsigned char)*p) && k < 7) u[k++] = tolower((unsigned char)*p++); u[k] = 0; }
    *val = x; *pp = p;
    return 1;
}

static int angle_deg(double v, const char *u, double *deg) {
    if (!*u || !strcmp(u, "deg")) *deg = v;
    else if (!strcmp(u, "turn")) *deg = v * 360;
    else if (!strcmp(u, "rad")) *deg = v * 57.29577951308232;
    else if (!strcmp(u, "grad")) *deg = v * .9;
    else return 0;
    return 1;
}

int css_color(const char *s, color_t *out, int legacy) {
    while (isspace((unsigned char)*s)) s++;
    int n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return 0;
    int isrgb = !strncasecmp(s, "rgb", 3), ishsl = !strncasecmp(s, "hsl", 3), ishwb = !strncasecmp(s, "hwb", 3);
    if ((isrgb || ishsl || ishwb) && (s[3] == '(' || (s[3] == 'a' && s[4] == '('))) {
        const char *p = strchr(s, '(') + 1;
        double c[3], a = 1;
        int pc[3], i;
        for (i = 0; i < 3; i++) {
            char u[8];
            double x;
            while (isspace((unsigned char)*p)) p++;
            if (!strncasecmp(p, "none", 4)) { x = 0; pc[i] = 0; p += 4; u[0] = 0; }
            else if (!cnum(&p, &x, &pc[i], u)) return 0;
            if (i == 0 && !isrgb) { double dg; if (pc[0] || !angle_deg(x, u, &dg)) { if (!pc[0] && *u) return 0; dg = x; } x = dg - 360 * (int)(dg / 360); if (x < 0) x += 360; }
            c[i] = x;
            while (isspace((unsigned char)*p)) p++;
            if (*p == ',') p++;
        }
        while (isspace((unsigned char)*p)) p++;
        if (*p == '/' || isdigit((unsigned char)*p) || *p == '.') {
            if (*p == '/') p++;
            int ap; double x; char u[8];
            if (!cnum(&p, &x, &ap, u)) return 0;
            a = ap ? x / 100 : x;
            if (a < 0) a = 0; else if (a > 1) a = 1;
        }
        while (isspace((unsigned char)*p)) p++;
        if (*p != ')') return 0;
        int r, g, b;
        if (isrgb) { r = cl255(pc[0] ? c[0] * 2.55 : c[0]); g = cl255(pc[1] ? c[1] * 2.55 : c[1]); b = cl255(pc[2] ? c[2] * 2.55 : c[2]); }
        else {
            double H = c[0] / 360, S = (c[1] < 0 ? 0 : c[1] > 100 ? 100 : c[1]) / 100, L = (c[2] < 0 ? 0 : c[2] > 100 ? 100 : c[2]) / 100, rr, gg, bb;
            if (ishwb) {
                double W = S, B = L;
                if (W + B >= 1) { rr = gg = bb = W / (W + B); }
                else {
                    double q = 1, pp = 0;
                    rr = hue2rgb(pp, q, H + 1. / 3); gg = hue2rgb(pp, q, H); bb = hue2rgb(pp, q, H - 1. / 3);
                    rr = rr * (1 - W - B) + W; gg = gg * (1 - W - B) + W; bb = bb * (1 - W - B) + W;
                }
            } else if (S == 0) rr = gg = bb = L;
            else {
                double q = L < .5 ? L * (1 + S) : L + S - L * S, pp = 2 * L - q;
                rr = hue2rgb(pp, q, H + 1. / 3); gg = hue2rgb(pp, q, H); bb = hue2rgb(pp, q, H - 1. / 3);
            }
            r = cl255(rr * 255); g = cl255(gg * 255); b = cl255(bb * 255);
        }
        int al = cl255(a * 255);
        *out = al ? (color_t)al << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b : 0;
        return 1;
    }
    if (n > 11 && !strncasecmp(s, "light-dark(", 11)) {                /* light scheme: the first colour */
        char b[64];
        const char *c = s + 11;
        int k = 0, d = 0;
        while (*c && k < 63 && (d || *c != ',')) { if (*c == '(') d++; else if (*c == ')') d--; b[k++] = *c++; }
        b[k] = 0;
        return css_color(b, out, 0);
    }
    const char *h = s;
    if (*h == '#') h++; else if (!legacy) h = 0;
    if (h) {
        int hl = s + n - h, hex = hl == 3 || hl == 4 || hl == 6 || hl == 8;
        for (int i = 0; i < hl && hex; i++) hex = isxdigit((unsigned char)h[i]);
        if (hex) {
            uint32_t v = 0;
            for (int i = 0; i < hl; i++) v = v << 4 | (isdigit((unsigned char)h[i]) ? h[i] - '0' : (h[i] | 32) - 'a' + 10);
            uint32_t a = 255;
            if (hl == 3 || hl == 4) {
                if (hl == 4) { a = (v & 15) * 17; v >>= 4; }
                v = ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17;
            } else if (hl == 8) { a = v & 255; v >>= 8; }
            *out = a ? a << 24 | v : 0;
            return 1;
        }
        if (*s == '#') return 0;
    }
    if (n == 11 && !strncasecmp(s, "transparent", 11)) { *out = 0; return 1; }
    if (n > 20) return 0;
    char w[24];
    for (int i = 0; i < n; i++) w[i] = tolower((unsigned char)s[i]);
    w[n] = 0;
    for (const char *p = NAMED; *p; ) {
        const char *e = strchr(p, ' ');
        if ((int)(e - p) == n && !strncmp(p, w, n)) { *out = 0xFF000000u | (uint32_t)strtoul(e + 1, 0, 16); return 1; }
        p = strchr(e + 1, ' ') + 1;
    }
    return 0;
}

/* ---------- lengths, calc() ---------- */
struct cn { uint8_t op, num; double px, pct; int a, b; };
enum { CN_LEAF, CN_ADD, CN_SUB, CN_MUL, CN_DIV, CN_MIN, CN_MAX };
static struct cn *CN; static int ncn, ccn;
static int ncv, ccv;

static int cn_new(struct cn n) {
    if (ncn == ccn) {
        int nc = ccn ? ccn * 2 : 64;
        struct cn *p = realloc(CN, nc * sizeof *p);
        if (!p) return -1;
        CN = p; ccn = nc;
    }
    CN[ncn] = n;
    return ncn++;
}
static int cn_leaf(double px, double pct, int num) { return cn_new((struct cn){ CN_LEAF, num, px, pct, 0, 0 }); }

static int cn_mk(int op, int a, int b) {              /* folds leaf-leaf; -1 = type error */
    if (a < 0 || b < 0) return -1;
    struct cn x = CN[a], y = CN[b];
    if (x.op == CN_LEAF && y.op == CN_LEAF) {
        switch (op) {
        case CN_ADD: case CN_SUB:
            if (x.num != y.num) return -1;
            return cn_leaf(op == CN_ADD ? x.px + y.px : x.px - y.px, op == CN_ADD ? x.pct + y.pct : x.pct - y.pct, x.num);
        case CN_MUL:
            if (x.num) return cn_leaf(y.px * x.px, y.pct * x.px, y.num);
            if (y.num) return cn_leaf(x.px * y.px, x.pct * y.px, x.num);
            return -1;
        case CN_DIV:
            if (!y.num || y.px == 0) return -1;
            return cn_leaf(x.px / y.px, x.pct / y.px, x.num);
        default:
            if (x.num != y.num) return -1;
            if (x.pct == y.pct) return cn_leaf(op == CN_MIN ? (x.px < y.px ? x.px : y.px) : (x.px > y.px ? x.px : y.px), x.pct, x.num);
        }
    } else if (op <= CN_DIV && op >= CN_MUL) {
        if (op == CN_MUL ? !(x.num || y.num || x.op != CN_LEAF || y.op != CN_LEAF) : 0) return -1;
    }
    return cn_new((struct cn){ (uint8_t)op, x.num && y.num, 0, 0, a, b });
}

struct cp { const char *s; double fs, rfs; int err; };

static void cp_ws(struct cp *c) { while (isspace((unsigned char)*c->s)) c->s++; }
static int cp_expr(struct cp *c);

static int cp_factor(struct cp *c) {
    cp_ws(c);
    if (*c->s == '(') {
        c->s++;
        int e = cp_expr(c);
        cp_ws(c);
        if (*c->s != ')') return -1;
        c->s++;
        return e;
    }
    if (*c->s == '-' && !isdigit((unsigned char)c->s[1]) && c->s[1] != '.') { c->s++; int f = cp_factor(c); return cn_mk(CN_MUL, f, cn_leaf(-1, 0, 1)); }
    if (isalpha((unsigned char)*c->s)) {
        char nm[16];
        int k = 0;
        while ((isalnum((unsigned char)*c->s) || *c->s == '-') && k < 15) nm[k++] = tolower((unsigned char)*c->s++);
        nm[k] = 0;
        if (*c->s != '(') {
            if (!strcmp(nm, "pi")) return cn_leaf(3.141592653589793, 0, 1);
            if (!strcmp(nm, "e")) return cn_leaf(2.718281828459045, 0, 1);
            return -1;
        }
        c->s++;
        int args[3], na = 0;
        for (;;) {
            if (na == 3) return -1;
            args[na++] = cp_expr(c);
            cp_ws(c);
            if (*c->s == ',') { c->s++; continue; }
            if (*c->s == ')') { c->s++; break; }
            return -1;
        }
        if (!strcmp(nm, "calc") || !*nm || !strcmp(nm, "-webkit-calc")) return na == 1 ? args[0] : -1;
        if (!strcmp(nm, "min") || !strcmp(nm, "max")) {
            int r = args[0];
            for (int i = 1; i < na; i++) r = cn_mk(nm[1] == 'i' ? CN_MIN : CN_MAX, r, args[i]);
            return r;
        }
        if (!strcmp(nm, "clamp") && na == 3) return cn_mk(CN_MAX, args[0], cn_mk(CN_MIN, args[1], args[2]));
        return -1;
    }
    double v; int pct; char u[8];
    if (!cnum(&c->s, &v, &pct, u)) return -1;
    if (pct) return cn_leaf(0, v, 0);
    if (!*u) return cn_leaf(v, 0, 1);
    double sc;
    if (!strcmp(u, "px")) sc = 1; else if (!strcmp(u, "pt")) sc = 4. / 3; else if (!strcmp(u, "pc")) sc = 16; else if (!strcmp(u, "in")) sc = 96;
    else if (!strcmp(u, "cm")) sc = 37.7953; else if (!strcmp(u, "mm")) sc = 3.77953; else if (!strcmp(u, "q")) sc = .944882;
    else if (!strcmp(u, "em")) sc = c->fs; else if (!strcmp(u, "rem")) sc = c->rfs; else if (!strcmp(u, "ex") || !strcmp(u, "ch")) sc = c->fs / 2;
    else if (!strcmp(u, "vw") || !strcmp(u, "svw") || !strcmp(u, "lvw") || !strcmp(u, "dvw") || !strcmp(u, "vi")) sc = vpw / 100.;
    else if (!strcmp(u, "vh") || !strcmp(u, "svh") || !strcmp(u, "lvh") || !strcmp(u, "dvh") || !strcmp(u, "vb")) sc = vph / 100.;
    else if (!strcmp(u, "vmin")) sc = (vpw < vph ? vpw : vph) / 100.; else if (!strcmp(u, "vmax")) sc = (vpw > vph ? vpw : vph) / 100.;
    else if (!strcmp(u, "deg") || !strcmp(u, "s") || !strcmp(u, "ms")) sc = 1;
    else return -1;
    return cn_leaf(v * sc, 0, 0);
}

static int cp_term(struct cp *c) {
    int a = cp_factor(c);
    for (;;) {
        cp_ws(c);
        if (*c->s != '*' && *c->s != '/') return a;
        int op = *c->s++ == '*' ? CN_MUL : CN_DIV;
        a = cn_mk(op, a, cp_factor(c));
    }
}
static int cp_expr(struct cp *c) {
    int a = cp_term(c);
    for (;;) {
        cp_ws(c);
        if ((*c->s != '+' && *c->s != '-') || !isspace((unsigned char)c->s[-1])) return a;   /* + and - need spaces around them */
        int op = *c->s++ == '+' ? CN_ADD : CN_SUB;
        if (!isspace((unsigned char)*c->s)) return -1;
        a = cn_mk(op, a, cp_term(c));
    }
}

static double cn_eval(int i, double base) {
    const struct cn *n = &CN[i];
    if (n->op == CN_LEAF) return n->px + n->pct * base / 100;
    double a = cn_eval(n->a, base), b = cn_eval(n->b, base);
    switch (n->op) {
    case CN_ADD: return a + b;
    case CN_SUB: return a - b;
    case CN_MUL: return a * b;
    case CN_DIV: return b ? a / b : 0;
    case CN_MIN: return a < b ? a : b;
    default: return a > b ? a : b;
    }
}

static int rnd(double x) { return (int)(x < 0 ? x - .5 : x + .5); }

/* compile a calc()/min()/max()/clamp() text; fs = em size, rfs = rem size. Result as kind/value: K_PX, K_PCT, K_CALC or K_NUM; 0 = bad */
static int calc_compile(const char *s, double fs, double rfs, int *k, int *v) {
    struct cp c = { s, fs, rfs, 0 };
    int mark = ncn;
    int r = cp_expr(&c);
    cp_ws(&c);
    if (r < 0 || *c.s) { ncn = mark; return 0; }
    const struct cn *n = &CN[r];
    if (n->op == CN_LEAF) {
        if (n->num) { *k = K_NUM; *v = rnd(n->px * 100); ncn = mark; return 1; }
        if (n->pct == 0) { *k = K_PX; *v = rnd(n->px); ncn = mark; return 1; }
        if (n->px == 0) { *k = K_PCT; *v = rnd(n->pct * 100); ncn = mark; return 1; }
    }
    *k = K_CALC; *v = r;
    return 1;
}

int css_len(const struct cprops *p, int prop, int base) {
    switch (p->k[prop]) {
    case K_PX: return p->v[prop];
    case K_PCT: return rnd(base * (double)p->v[prop] / 10000);
    case K_CALC: return rnd(cn_eval(p->v[prop], base));
    }
    return 0;
}

/* a length token. Fills kind/value: px (K_PX), % (K_PCT, *100), em/rem/ex/ch (K_EM/K_REM, *100: resolved when the font size is known),
 * calc() text (K_CALCS, v = interned text), auto (K_AUTO). Returns 0 if it isn't a length. */
static int len_parse(const char *s, int *k, int *v) {
    while (isspace((unsigned char)*s)) s++;
    if (!strncasecmp(s, "auto", 4) && !s[4]) { *k = K_AUTO; *v = 0; return 1; }
    if (!strncasecmp(s, "calc(", 5) || !strncasecmp(s, "min(", 4) || !strncasecmp(s, "max(", 4) || !strncasecmp(s, "clamp(", 6) || !strncasecmp(s, "-webkit-calc(", 13)) {
        int mk = ncn, kk, vv;
        int ok = calc_compile(s, 16, 16, &kk, &vv);       /* syntax check only; units compile again with the real font size */
        ncn = mk;
        if (!ok) return 0;
        int e = strlen(s);
        while (e && isspace((unsigned char)s[e - 1])) e--;
        *k = K_CALCS; *v = intern(s, e);
        return 1;
    }
    const char *p = s;
    double x; int pct; char u[8];
    if (!cnum(&p, &x, &pct, u)) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p) return 0;
    if (pct) { *k = K_PCT; *v = rnd(x * 100); return 1; }
    *k = K_PX;
    if (!*u) { if (x != 0) return 0; *v = 0; return 1; }
    if (!strcmp(u, "px")) *v = rnd(x);
    else if (!strcmp(u, "pt")) *v = rnd(x * 4 / 3);
    else if (!strcmp(u, "pc")) *v = rnd(x * 16);
    else if (!strcmp(u, "in")) *v = rnd(x * 96);
    else if (!strcmp(u, "cm")) *v = rnd(x * 37.7953);
    else if (!strcmp(u, "mm")) *v = rnd(x * 3.77953);
    else if (!strcmp(u, "q")) *v = rnd(x * .944882);
    else if (!strcmp(u, "em")) { *k = K_EM; *v = rnd(x * 100); }
    else if (!strcmp(u, "rem")) { *k = K_REM; *v = rnd(x * 100); }
    else if (!strcmp(u, "ex") || !strcmp(u, "ch")) { *k = K_EM; *v = rnd(x * 50); }
    else if (!strcmp(u, "vw") || !strcmp(u, "svw") || !strcmp(u, "lvw") || !strcmp(u, "dvw") || !strcmp(u, "vi")) *v = rnd(x * vpw / 100);
    else if (!strcmp(u, "vh") || !strcmp(u, "svh") || !strcmp(u, "lvh") || !strcmp(u, "dvh") || !strcmp(u, "vb")) *v = rnd(x * vph / 100);
    else if (!strcmp(u, "vmin")) *v = rnd(x * (vpw < vph ? vpw : vph) / 100);
    else if (!strcmp(u, "vmax")) *v = rnd(x * (vpw > vph ? vpw : vph) / 100);
    else return 0;
    return 1;
}

/* ---------- declarations ---------- */
static int last_nh;                                      /* rules that matched the element cascade() just styled */
static int imp_cur;                                     /* !important of the declaration being parsed */

static void put(int prop, int kind, int v) {
    if (nd == dcap) {
        int nc = dcap ? dcap * 2 : 1024;
        struct decl *n = realloc(D, nc * sizeof *n);
        if (!n) return;
        D = n; dcap = nc;
    }
    D[nd++] = (struct decl){ prop, imp_cur, kind, v, 0 };
}

/* keyword list "a|b|c:5|d": index of v (entries "name:N" are aliases for N and don't count), -1 if absent */
static int kwi(const char *list, const char *v) {
    int idx = 0;
    for (const char *p = list; *p; ) {
        const char *e = p;
        while (*e && *e != '|' && *e != ':') e++;
        int l = e - p, val = idx, alias = *e == ':';
        const char *nx = e;
        if (alias) { val = atoi(e + 1); nx = e + 1; while (*nx && *nx != '|') nx++; }
        else idx++;
        if (!strncasecmp(v, p, l) && !v[l]) return val;
        p = *nx ? nx + 1 : nx;
    }
    return -1;
}

/* split val into tokens (copied into buf) at whitespace; "(...)" and quoted strings stay whole; a top-level '/' is a token of its own */
static int toks(const char *val, char *buf, int cap, char **tok, int max) {
    int k = 0, n = 0, depth = 0;
    char q = 0, *start = 0;
    for (const char *p = val; *p && k < cap - 3; p++) {
        char c = *p;
        int brk = !q && !depth && (isspace((unsigned char)c) || c == '/');
        if (brk) {
            if (start) { buf[k++] = 0; start = 0; }
            if (c == '/') { if (n == max) break; tok[n++] = buf + k; buf[k++] = '/'; buf[k++] = 0; }
            continue;
        }
        if (!start) { if (n == max) break; start = buf + k; tok[n++] = start; }
        if (q) { if (c == '\\' && p[1]) buf[k++] = *p++; else if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')' && depth) depth--;
        buf[k++] = *p;
    }
    buf[k] = 0;
    return n;
}

static void trim(char *s) {
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
    int n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

/* len/%/auto/keywords according to flags */
#define F_AUTO 1
#define F_NEG 2
#define F_PCT 4
#define F_NONE 8          /* "none" = K_AUTO */
#define F_FIT 16
#define F_NORMAL 32       /* "normal" = 0 */
static int lenv(const char *s, int fl, int *k, int *v) {
    while (isspace((unsigned char)*s)) s++;
    if ((fl & F_NONE) && !strcasecmp(s, "none")) { *k = K_AUTO; *v = 0; return 1; }
    if ((fl & F_NORMAL) && !strcasecmp(s, "normal")) { *k = K_PX; *v = 0; return 1; }
    if (fl & F_FIT) {
        if (!strcasecmp(s, "min-content")) { *k = K_MINC; *v = 0; return 1; }
        if (!strcasecmp(s, "max-content")) { *k = K_MAXC; *v = 0; return 1; }
        if (!strncasecmp(s, "fit-content", 11) || !strcasecmp(s, "-webkit-fit-content") || !strcasecmp(s, "-moz-fit-content")) { *k = K_FIT; *v = 0; return 1; }
        if (!strcasecmp(s, "stretch") || !strcasecmp(s, "-webkit-fill-available") || !strcasecmp(s, "-moz-available")) { *k = K_PCT; *v = 10000; return 1; }
    }
    if (!len_parse(s, k, v)) return 0;
    if (*k == K_AUTO) return fl & F_AUTO;
    if (*k == K_PCT && !(fl & F_PCT)) return 0;
    if (!(fl & F_NEG) && *k != K_CALCS && *v < 0) return 0;
    return 1;
}

static int url_arg(const char *t, char *out, int cap) {     /* url(x), url("x"): the absolute URL */
    while (isspace((unsigned char)*t)) t++;
    if (strncasecmp(t, "url(", 4)) return 0;
    t += 4;
    while (isspace((unsigned char)*t)) t++;
    char q = *t == '"' || *t == '\'' ? *t++ : 0;
    char ref[URL_MAX];
    int k = 0;
    while (*t && k < URL_MAX - 1 && (q ? *t != q : *t != ')' && !isspace((unsigned char)*t))) { if (*t == '\\' && t[1]) t++; ref[k++] = *t++; }
    ref[k] = 0;
    if (!k) return 0;
    if (cur_base[0] || page_base[0]) url_resolve(cur_base[0] ? cur_base : page_base, ref, out, cap);
    else strlcpy(out, ref, cap);
    return 1;
}

static int parse_color_val(const char *v, int *k, int *val) {
    color_t c;
    if (!strcasecmp(v, "currentcolor")) { *k = K_CUR; *val = 0; return 1; }
    if (!css_color(v, &c, 0)) return 0;
    *k = K_PX; *val = (int)c;
    return 1;
}

/* ---- property table ---- */
enum { Y_COLOR, Y_KW, Y_LEN, Y_NUM, Y_INT, Y_SPEC };
struct pdef { const char *n; uint8_t p, ty, fl; const char *kw; };
#define LEN(n, p, fl) { n, p, Y_LEN, fl, 0 }
#define KW(n, p, l) { n, p, Y_KW, 0, l }
#define COL(n, p) { n, p, Y_COLOR, 0, 0 }
#define SPEC(n, p) { n, p, Y_SPEC, 0, 0 }
#define ALK "normal|start|end|center|space-between|space-around|space-evenly|stretch|baseline|flex-start:1|flex-end:2|left:1|right:2|self-start:1|self-end:2|first baseline:8|last baseline:8|safe center:3|unsafe center:3|legacy:0|safe flex-start:1|safe flex-end:2"
#define BSL "none|solid|dashed|dotted|double|groove|ridge|inset|outset|hidden:0"
static const struct pdef PD[] = {
    COL("color", P_COLOR), COL("background-color", P_BG),
    COL("border-top-color", P_BCT), COL("border-right-color", P_BCR), COL("border-bottom-color", P_BCB), COL("border-left-color", P_BCL),
    LEN("margin-top", P_MT, F_AUTO | F_NEG | F_PCT), LEN("margin-right", P_MR, F_AUTO | F_NEG | F_PCT),
    LEN("margin-bottom", P_MB, F_AUTO | F_NEG | F_PCT), LEN("margin-left", P_ML, F_AUTO | F_NEG | F_PCT),
    LEN("padding-top", P_PT, F_PCT), LEN("padding-right", P_PR, F_PCT), LEN("padding-bottom", P_PB, F_PCT), LEN("padding-left", P_PL, F_PCT),
    LEN("top", P_TOP, F_AUTO | F_NEG | F_PCT), LEN("right", P_RIGHT, F_AUTO | F_NEG | F_PCT),
    LEN("bottom", P_BOTTOM, F_AUTO | F_NEG | F_PCT), LEN("left", P_LEFT, F_AUTO | F_NEG | F_PCT),
    LEN("width", P_WIDTH, F_AUTO | F_PCT | F_FIT), LEN("height", P_HEIGHT, F_AUTO | F_PCT | F_FIT),
    LEN("min-width", P_MINW, F_AUTO | F_PCT | F_FIT), LEN("min-height", P_MINH, F_AUTO | F_PCT | F_FIT),
    LEN("max-width", P_MAXW, F_NONE | F_PCT | F_FIT), LEN("max-height", P_MAXH, F_NONE | F_PCT | F_FIT),
    LEN("border-top-left-radius", P_RTL, F_PCT), LEN("border-top-right-radius", P_RTR, F_PCT),
    LEN("border-bottom-right-radius", P_RBR, F_PCT), LEN("border-bottom-left-radius", P_RBL, F_PCT),
    LEN("letter-spacing", P_LS, F_NEG | F_NORMAL), LEN("word-spacing", P_WSP, F_NEG | F_NORMAL), LEN("text-indent", P_TIND, F_NEG | F_PCT),
    LEN("row-gap", P_RGAP, F_PCT | F_NORMAL), LEN("column-gap", P_CGAP, F_PCT | F_NORMAL),
    KW("display", P_DISPLAY, "none:1|block:2|inline:3|inline-block:4|flex:5|inline-flex:6|grid:7|inline-grid:8|table:9|inline-table:10|table-row:11|table-cell:12|"
       "table-row-group:13|table-header-group:14|table-footer-group:15|table-column:16|table-column-group:17|table-caption:18|list-item:19|contents:20|"
       "flow-root:2|-webkit-box:5|-webkit-inline-box:6|-ms-flexbox:5|-ms-inline-flexbox:6|run-in:2|ruby:3|ruby-text:3|block flow:2|inline flow:3|block flow-root:2|"
       "inline flow-root:4|block flex:5|inline flex:6|block grid:7|inline grid:8|flow list-item:19|block flow list-item:19"),
    KW("clip", P_CLIP, "auto|rect(0 0 0 0):1|rect(0,0,0,0):1|rect(0, 0, 0, 0):1|rect(0px 0px 0px 0px):1|rect(0px,0px,0px,0px):1|rect(1px 1px 1px 1px):1|rect(1px,1px,1px,1px):1|rect(1px, 1px, 1px, 1px):1"),
    KW("position", P_POSITION, "static|relative|absolute|fixed|sticky|-webkit-sticky:4"),
    KW("float", P_FLOAT, "none|left|right|inline-start:1|inline-end:2"), KW("clear", P_CLEAR, "none|left|right|both|inline-start:1|inline-end:2"),
    KW("overflow-x", P_OVX, "visible|hidden|scroll|auto|clip|overlay:3"), KW("overflow-y", P_OVY, "visible|hidden|scroll|auto|clip|overlay:3"),
    KW("box-sizing", P_BOXSZ, "content-box|border-box"), KW("visibility", P_VISIB, "visible|hidden|collapse"), KW("direction", P_DIR, "ltr|rtl"),
    KW("text-transform", P_TT, "none|uppercase|lowercase|capitalize"), KW("text-overflow", P_TOVF, "clip|ellipsis"),
    KW("flex-direction", P_FLEXDIR, "row|row-reverse|column|column-reverse"), KW("flex-wrap", P_FLEXWRAP, "nowrap|wrap|wrap-reverse"),
    KW("justify-content", P_JUSTC, ALK), KW("align-items", P_ALIGNI, ALK), KW("align-content", P_ALIGNC, ALK), KW("justify-items", P_JUSTI, ALK),
    KW("align-self", P_ALIGNS, ALK "|auto:100"), KW("justify-self", P_JUSTS, ALK "|auto:100"),
    KW("white-space", P_WS, "normal|nowrap|pre|pre-wrap|pre-line|break-spaces"),
    KW("table-layout", P_TBLAY, "auto|fixed"), KW("pointer-events", P_PEV, "auto|none|all:0|visible:0|painted:0|fill:0|stroke:0"),
    KW("object-fit", P_OBJFIT, "fill|contain|cover|none|scale-down"), KW("font-style", P_ITALIC, "normal|italic|oblique:1"),
    KW("border-top-style", P_BSTT, BSL), KW("border-right-style", P_BSTR, BSL), KW("border-bottom-style", P_BSTB, BSL), KW("border-left-style", P_BSTL, BSL),
    KW("border-collapse", P_COLLAPSE, "separate|collapse"), KW("list-style-position", P_LISTP, "outside|inside"),
    KW("text-align", P_ALIGN, "left|center|right|justify|start:0|end:2|-webkit-center:1|-moz-center:1|-webkit-left:0|-webkit-right:2|match-parent:0|justify-all:3"),
    KW("grid-auto-flow", P_GFLOW, "row|column|row dense:2|column dense:3|dense:2"),
    KW("cursor", P_CURSOR, "auto|default|pointer|text|move:4|not-allowed:4|wait:4|help:4|crosshair:4|grab:4|grabbing:4|progress:4|zoom-in:4|zoom-out:4|none:4|vertical-text:3"),
    KW("list-style-type", P_LISTT, "disc|circle|square|decimal|decimal-leading-zero|lower-alpha|upper-alpha|lower-roman|upper-roman|lower-greek|none|lower-latin:5|upper-latin:6|"
       "disclosure-closed:0|disclosure-open:0|armenian:3|georgian:3|cjk-decimal:3|hebrew:3|lower-armenian:3|upper-armenian:3|hiragana:3|katakana:3"),
    { "flex-grow", P_FGROW, Y_NUM, 0, 0 }, { "flex-shrink", P_FSHRINK, Y_NUM, 0, 0 }, { "order", P_ORDER, Y_INT, 0, 0 }, { "z-index", P_ZINDEX, Y_INT, 0, 0 },
    SPEC("font-size", P_FSIZE), SPEC("font-weight", P_FWEIGHT), SPEC("line-height", P_LH), SPEC("opacity", P_OPACITY),
    SPEC("border-top-width", P_BWT), SPEC("border-right-width", P_BWR), SPEC("border-bottom-width", P_BWB), SPEC("border-left-width", P_BWL),
    SPEC("vertical-align", P_VALIGN), SPEC("word-break", P_WBRK), SPEC("overflow-wrap", P_WBRK), SPEC("font-family", P_FAMILY),
    SPEC("content", P_CONTENT), SPEC("background-image", P_BGIMG), SPEC("background-repeat", P_BGREP), SPEC("background-position", P_BGPX),
    SPEC("background-size", P_BGSW), SPEC("flex-basis", P_FBASIS), SPEC("border-spacing", P_SPACING), SPEC("aspect-ratio", P_ASPECT),
    SPEC("grid-template-columns", P_GCOLS), SPEC("grid-template-rows", P_GROWS), SPEC("grid-auto-columns", P_GACOLS), SPEC("grid-auto-rows", P_GAROWS),
    SPEC("grid-template-areas", P_GAREAS), SPEC("grid-column-start", P_GCS), SPEC("grid-column-end", P_GCE), SPEC("grid-row-start", P_GRS),
    SPEC("grid-row-end", P_GRE), SPEC("grid-area", P_GAREA), SPEC("text-decoration-line", P_DECO),
    SPEC("outline-width", P_OUTW), KW("outline-style", P_OUTS, "none|solid|dashed|dotted|double|groove|ridge|inset|outset|hidden:0|auto:1"), COL("outline-color", P_OUTC),
    LEN("outline-offset", P_OUTO, F_NEG), SPEC("box-shadow", P_SHX), SPEC("counter-reset", P_CRESET), SPEC("counter-increment", P_CINC),
    SPEC("object-position", P_OBJPX), SPEC("transform", P_TRX), SPEC("mask-image", P_MASKIMG),
};

/* shorthands (and two-valued longhands): the properties a global keyword (inherit/initial/unset) applies to */
static const struct grp { const char *n; uint8_t p[14]; } GRP[] = {
    { "margin", { P_MT, P_MR, P_MB, P_ML, 255 } }, { "padding", { P_PT, P_PR, P_PB, P_PL, 255 } }, { "inset", { P_TOP, P_RIGHT, P_BOTTOM, P_LEFT, 255 } },
    { "border", { P_BWT, P_BWR, P_BWB, P_BWL, P_BSTT, P_BSTR, P_BSTB, P_BSTL, P_BCT, P_BCR, P_BCB, P_BCL, 255 } },
    { "border-width", { P_BWT, P_BWR, P_BWB, P_BWL, 255 } }, { "border-style", { P_BSTT, P_BSTR, P_BSTB, P_BSTL, 255 } },
    { "border-color", { P_BCT, P_BCR, P_BCB, P_BCL, 255 } },
    { "border-top", { P_BWT, P_BSTT, P_BCT, 255 } }, { "border-right", { P_BWR, P_BSTR, P_BCR, 255 } },
    { "border-bottom", { P_BWB, P_BSTB, P_BCB, 255 } }, { "border-left", { P_BWL, P_BSTL, P_BCL, 255 } },
    { "border-radius", { P_RTL, P_RTR, P_RBR, P_RBL, 255 } },
    { "background", { P_BG, P_BGIMG, P_BGREP, P_BGPX, P_BGPY, P_BGSW, P_BGSH, P_BG2IMG, 255 } },
    { "background-position", { P_BGPX, P_BGPY, 255 } }, { "background-size", { P_BGSW, P_BGSH, 255 } },
    { "font", { P_ITALIC, P_FWEIGHT, P_FSIZE, P_LH, P_FAMILY, 255 } }, { "flex", { P_FGROW, P_FSHRINK, P_FBASIS, 255 } },
    { "flex-flow", { P_FLEXDIR, P_FLEXWRAP, 255 } }, { "gap", { P_RGAP, P_CGAP, 255 } }, { "grid-gap", { P_RGAP, P_CGAP, 255 } },
    { "overflow", { P_OVX, P_OVY, 255 } }, { "list-style", { P_LISTT, P_LISTP, 255 } },
    { "place-items", { P_ALIGNI, P_JUSTI, 255 } }, { "place-content", { P_ALIGNC, P_JUSTC, 255 } }, { "place-self", { P_ALIGNS, P_JUSTS, 255 } },
    { "text-decoration", { P_DECO, 255 } }, { "grid-column", { P_GCS, P_GCE, 255 } }, { "grid-row", { P_GRS, P_GRE, 255 } },
    { "grid-area", { P_GAREA, P_GRS, P_GCS, P_GRE, P_GCE, 255 } }, { "outline", { P_OUTW, P_OUTS, P_OUTC, 255 } },
    { "mask-image", { P_MASKIMG, 255 } }, { "box-shadow", { P_SHX, P_SHY, P_SHB, P_SHS, P_SHC, P_SHI, 255 } }, { "object-position", { P_OBJPX, P_OBJPY, 255 } }, { "transform", { P_TRX, P_TRY, P_TRSX, P_TRSY, P_TRROT, 255 } }, { "border-spacing", { P_SPACING, P_SPACINGY, 255 } },
    { "grid-template", { P_GROWS, P_GCOLS, P_GAREAS, 255 } }, { "vertical-align", { P_VALIGN, P_VAOFF, 255 } },
};

static const char *const LOGICAL[][2] = {
    { "margin-inline-start", "margin-left" }, { "margin-inline-end", "margin-right" }, { "margin-block-start", "margin-top" }, { "margin-block-end", "margin-bottom" },
    { "padding-inline-start", "padding-left" }, { "padding-inline-end", "padding-right" }, { "padding-block-start", "padding-top" }, { "padding-block-end", "padding-bottom" },
    { "inset-inline-start", "left" }, { "inset-inline-end", "right" }, { "inset-block-start", "top" }, { "inset-block-end", "bottom" },
    { "border-inline-start", "border-left" }, { "border-inline-end", "border-right" }, { "border-block-start", "border-top" }, { "border-block-end", "border-bottom" },
    { "border-inline-start-width", "border-left-width" }, { "border-inline-end-width", "border-right-width" },
    { "border-block-start-width", "border-top-width" }, { "border-block-end-width", "border-bottom-width" },
    { "border-inline-start-color", "border-left-color" }, { "border-inline-end-color", "border-right-color" },
    { "border-block-start-color", "border-top-color" }, { "border-block-end-color", "border-bottom-color" },
    { "border-inline-start-style", "border-left-style" }, { "border-inline-end-style", "border-right-style" },
    { "border-block-start-style", "border-top-style" }, { "border-block-end-style", "border-bottom-style" },
    { "inline-size", "width" }, { "block-size", "height" }, { "min-inline-size", "min-width" }, { "min-block-size", "min-height" },
    { "max-inline-size", "max-width" }, { "max-block-size", "max-height" }, { "word-wrap", "overflow-wrap" }, { "grid-row-gap", "row-gap" },
    { "grid-column-gap", "column-gap" }, { "border-start-start-radius", "border-top-left-radius" }, { "border-start-end-radius", "border-top-right-radius" },
    { "border-end-start-radius", "border-bottom-left-radius" }, { "border-end-end-radius", "border-bottom-right-radius" },
};

static const struct pdef *find_pd(const char *n) {
    for (unsigned i = 0; i < sizeof PD / sizeof *PD; i++) if (!strcmp(PD[i].n, n)) return &PD[i];
    return 0;
}

static const uint8_t SIDE_M[4] = { P_MT, P_MR, P_MB, P_ML }, SIDE_P[4] = { P_PT, P_PR, P_PB, P_PL }, SIDE_I[4] = { P_TOP, P_RIGHT, P_BOTTOM, P_LEFT },
    SIDE_BW[4] = { P_BWT, P_BWR, P_BWB, P_BWL }, SIDE_BS[4] = { P_BSTT, P_BSTR, P_BSTB, P_BSTL }, SIDE_BC[4] = { P_BCT, P_BCR, P_BCB, P_BCL },
    CORNER[4] = { P_RTL, P_RTR, P_RBR, P_RBL };
static const uint8_t MAP4[5][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };

static int bw_value(const char *t, int *k, int *v);
/* 1..4 values for four sides (CSS order top right bottom left) */
static void sides4(const uint8_t *props, char *val, int fl) {
    char buf[320], *tk[5];
    int n = toks(val, buf, sizeof buf, tk, 5), k[4], v[4];
    if (n < 1 || n > 4) return;
    for (int i = 0; i < n; i++) if (!((fl & 0x100) ? bw_value(tk[i], &k[i], &v[i]) : lenv(tk[i], fl, &k[i], &v[i]))) return;
    for (int s = 0; s < 4; s++) put(props[s], k[MAP4[n][s]], v[MAP4[n][s]]);
}

static int bw_value(const char *t, int *k, int *v) {
    if (!strcasecmp(t, "thin")) { *k = K_PX; *v = 1; return 1; }
    if (!strcasecmp(t, "medium")) { *k = K_PX; *v = 3; return 1; }
    if (!strcasecmp(t, "thick")) { *k = K_PX; *v = 5; return 1; }
    return lenv(t, 0, k, v);
}

/* border shorthand for one side (-1 = all four): width style color, any order; unspecified parts reset */
static void border_sh(int side, char *val) {
    char buf[320], *tk[8];
    int n = toks(val, buf, sizeof buf, tk, 8), wk = K_PX, wv = 3, st = BS_NONE, ck = K_CUR, cv = 0;
    for (int i = 0; i < n; i++) {
        int k, v;
        if (kwi(BSL, tk[i]) >= 0) st = kwi(BSL, tk[i]);
        else if (bw_value(tk[i], &k, &v)) { wk = k; wv = v; }
        else if (parse_color_val(tk[i], &k, &v)) { ck = k; cv = v; }
        else return;
    }
    for (int s = 0; s < 4; s++) {
        if (side >= 0 && s != side) continue;
        put(SIDE_BW[s], wk, wv); put(SIDE_BS[s], K_PX, st); put(SIDE_BC[s], ck, cv);
    }
}

static int is_rep(const char *t) { return kwi("repeat|repeat-x|repeat-y|no-repeat|space:0|round:0", t) >= 0; }
static int rep_val(const char *x, const char *y) {          /* one or two keywords -> REP_* */
    int rx = !strcasecmp(x, "no-repeat") ? 0 : 1, ry = rx;
    if (!strcasecmp(x, "repeat-x")) { rx = 1; ry = 0; } else if (!strcasecmp(x, "repeat-y")) { rx = 0; ry = 1; }
    if (y) ry = strcasecmp(y, "no-repeat") ? 1 : 0;
    return rx && ry ? REP_REPEAT : rx ? REP_X : ry ? REP_Y : REP_NONE;
}

static int kwpos(const char *t) { return !strcasecmp(t, "left") ? 1 : !strcasecmp(t, "right") ? 2 : !strcasecmp(t, "top") ? 3 : !strcasecmp(t, "bottom") ? 4 : !strcasecmp(t, "center") ? 5 : 0; }
static int pos1(const char *t, int *k, int *v) {
    int kw = kwpos(t);
    if (kw) { *k = K_PCT; *v = kw == 2 || kw == 4 ? 10000 : kw == 5 ? 5000 : 0; return 1; }
    return lenv(t, F_PCT | F_NEG, k, v);
}
/* background-position tokens -> two values (kind/v each) */
static int pos_parse(char **tk, int n, int *kx, int *vx, int *ky, int *vy) {
    *kx = *ky = K_PCT; *vx = *vy = 5000;
    if (n < 1 || n > 4) return 0;
    if (n <= 2) {
        int a = kwpos(tk[0]), b = n == 2 ? kwpos(tk[1]) : 0;
        const char *xt, *yt;
        if (n == 1) { if (a == 3 || a == 4) { xt = "center"; yt = tk[0]; } else { xt = tk[0]; yt = "center"; } }
        else if (a == 3 || a == 4 || b == 1 || b == 2) { xt = tk[1]; yt = tk[0]; } else { xt = tk[0]; yt = tk[1]; }
        return pos1(xt, kx, vx) && pos1(yt, ky, vy);
    }
    for (int i = 0; i < n; i++) {                           /* "right 10px bottom 20px": keyword plus optional offset */
        int kw = kwpos(tk[i]);
        if (!kw) return 0;
        int k = K_PCT, v = kw == 2 || kw == 4 ? 10000 : kw == 5 ? 5000 : 0;
        if (kw != 5 && i + 1 < n && !kwpos(tk[i + 1])) {
            int ok, ov;
            if (!lenv(tk[i + 1], F_PCT | F_NEG, &ok, &ov)) return 0;
            if (kw == 1 || kw == 3) { k = ok; v = ov; }     /* offsets from the right/bottom edge are dropped */
            i++;
        }
        if (kw == 1 || kw == 2) { *kx = k; *vx = v; } else if (kw == 3 || kw == 4) { *ky = k; *vy = v; } else if (i < 2) { *kx = k; *vx = v; } else { *ky = k; *vy = v; }
    }
    return 1;
}

/* background shorthand, one comma-separated layer */
static void bg_layer(char *layer, int is_last, int which) {      /* which: 0 = the top image layer, 1 = the second, -1 = colour only */
    char buf[600], *tk[16];
    int n = toks(layer, buf, sizeof buf, tk, 16);
    char *pos[5]; int np = 0, inpos = 1, sz = 0;
    int ck = K_PX, cv = 0, img = K_NONE, imgv = 0, rep = REP_REPEAT, rk[2] = { K_AUTO, K_AUTO }, rv[2] = { 0, 0 };
    char *rx = 0, *ry = 0;
    char url[URL_MAX];
    for (int i = 0; i < n; i++) {
        char *t = tk[i];
        int k, v;
        if (!strcmp(t, "/")) { inpos = 0; continue; }
        if (!strncasecmp(t, "url(", 4)) { if (url_arg(t, url, sizeof url)) { img = K_STR; imgv = intern(url, strlen(url)); } }
        else if (strstr(t, "gradient(")) { img = K_GRAD; imgv = intern(t, strlen(t)); }
        else if (!strcasecmp(t, "none")) img = K_NONE;
        else if (is_rep(t)) { if (!rx) rx = t; else ry = t; }
        else if (kwi("scroll|fixed|local|border-box|padding-box|content-box|text", t) >= 0) ;
        else if (!strncasecmp(t, "image-set(", 10) || !strncasecmp(t, "-webkit-image-set(", 18) || !strncasecmp(t, "element(", 8)) ;
        else if (parse_color_val(t, &k, &v)) { ck = k; cv = v; }
        else if (inpos) { if (np < 4) pos[np++] = t; }
        else if (sz < 2) {
            if (!strcasecmp(t, "cover")) { rk[0] = K_COVER; sz = 2; }
            else if (!strcasecmp(t, "contain")) { rk[0] = K_CONTAIN; sz = 2; }
            else if (lenv(t, F_AUTO | F_PCT, &k, &v)) { rk[sz] = k; rv[sz] = v; sz++; }
        }
    }
    if (rx) rep = rep_val(rx, ry);
    if (is_last) put(P_BG, ck, cv);
    if (which >= 0) {
        static const uint8_t L0[6] = { P_BGIMG, P_BGREP, P_BGPX, P_BGPY, P_BGSW, P_BGSH }, L1[6] = { P_BG2IMG, P_BG2REP, P_BG2PX, P_BG2PY, P_BG2SW, P_BG2SH };
        const uint8_t *P = which ? L1 : L0;
        put(P[0], img, imgv); put(P[1], K_PX, rep);
        int kx = K_PCT, vx = 0, ky = K_PCT, vy = 0;
        if (np) pos_parse(pos, np, &kx, &vx, &ky, &vy);
        put(P[2], kx, vx); put(P[3], ky, vy);
        if (rk[0] == K_COVER || rk[0] == K_CONTAIN) { put(P[4], rk[0], 0); put(P[5], rk[0], 0); }
        else { put(P[4], rk[0], rv[0]); put(P[5], sz == 2 ? rk[1] : K_AUTO, rv[1]); }
    }
}

static void background_sh(char *val) {
    char *layers[8];
    int nl = 0, d = 0;
    char q = 0;
    layers[nl++] = val;
    for (char *p = val; *p; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '(') d++;
        else if (*p == ')' && d) d--;
        else if (*p == ',' && !d && nl < 8) { *p = 0; layers[nl++] = p + 1; }
    }
    /* two image layers are kept (top, then the one under it); the colour comes from the last layer */
    int img[2] = { -1, -1 }, ni = 0;
    for (int i = 0; i < nl && ni < 2; i++) if (strstr(layers[i], "url(") || strstr(layers[i], "gradient(")) img[ni++] = i;
    char tmp[700];
    strlcpy(tmp, layers[img[0] >= 0 ? img[0] : 0], sizeof tmp);
    bg_layer(tmp, 0, 0);
    if (img[1] >= 0) { strlcpy(tmp, layers[img[1]], sizeof tmp); bg_layer(tmp, 0, 1); }
    else put(P_BG2IMG, K_NONE, 0);
    strlcpy(tmp, layers[nl - 1], sizeof tmp);
    bg_layer(tmp, 1, -1);
}

static int classify_family(const char *list) {     /* FAM_*, or -1 when no listed family is one we know */
    char b[256];
    int k = 0;
    for (const char *p = list; ; p++) {
        if (*p == ',' || !*p) {
            b[k] = 0;
            char *s = b;
            while (isspace((unsigned char)*s) || *s == '"' || *s == '\'') s++;
            int c = -1;
            if (strstr(s, "material icons") || strstr(s, "material symbols") || strstr(s, "fontawesome") || strstr(s, "font awesome") || strstr(s, "glyphicons") || strstr(s, "icomoon") || strstr(s, "bootstrap-icons") || strstr(s, "ionicons") || strstr(s, "octicons")) c = FAM_ICON;
            else if (strstr(s, "mono") || strstr(s, "courier") || strstr(s, "consolas") || strstr(s, "menlo") || strstr(s, "monaco") || strstr(s, "lucida console") ||
                strstr(s, "fira code") || strstr(s, "source code") || strstr(s, "cascadia") || strstr(s, "jetbrains") || strstr(s, "inconsolata") || strstr(s, "terminal")) c = FAM_MONO;
            else if (strstr(s, "sans") || !strncmp(s, "arial", 5) || strstr(s, "helvetica") || strstr(s, "verdana") || strstr(s, "tahoma") || strstr(s, "segoe") ||
                     strstr(s, "system-ui") || strstr(s, "-apple-system") || strstr(s, "blinkmacsystemfont") || strstr(s, "roboto") || !strncmp(s, "inter", 5) ||
                     strstr(s, "lato") || strstr(s, "trebuchet") || strstr(s, "calibri") || strstr(s, "ubuntu") || strstr(s, "cursive") ||
                     strstr(s, "fantasy") || strstr(s, "emoji") || strstr(s, "montserrat") || strstr(s, "poppins")) c = FAM_SANS;
            else if (strstr(s, "serif") || strstr(s, "georgia") || strstr(s, "times") || strstr(s, "garamond") || strstr(s, "palatino") || strstr(s, "cambria") ||
                     strstr(s, "baskerville") || strstr(s, "charter") || strstr(s, "merriweather") || strstr(s, "playfair") || strstr(s, "book antiqua")) c = FAM_SERIF;
            if (c >= 0) return c;
            k = 0;
            if (!*p) return -1;
            continue;
        }
        if (k < 250) b[k++] = tolower((unsigned char)*p);
    }
}

static int weight_val(const char *v, int *k, int *w) {
    *k = K_PX;
    if (!strcasecmp(v, "normal")) *w = 400;
    else if (!strcasecmp(v, "bold")) *w = 700;
    else if (!strcasecmp(v, "bolder")) { *k = K_NUM; *w = 1; }
    else if (!strcasecmp(v, "lighter")) { *k = K_NUM; *w = 2; }
    else { char *e; double x = strtod(v, &e); if (e == v || *e || x < 1 || x > 1000) return 0; *w = (int)x; }
    return 1;
}

static int fsize_val(const char *v, int *k, int *n) {
    static const struct { const char *n; int px; } KWS[] = { { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 } };
    for (unsigned i = 0; i < sizeof KWS / sizeof *KWS; i++) if (!strcasecmp(v, KWS[i].n)) { *k = K_PX; *n = KWS[i].px; return 1; }
    if (!strcasecmp(v, "smaller")) { *k = K_PCT; *n = 8333; return 1; }
    if (!strcasecmp(v, "larger")) { *k = K_PCT; *n = 12000; return 1; }
    return lenv(v, F_PCT, k, n);
}

static int lh_val(const char *v, int *k, int *n) {
    if (!strcasecmp(v, "normal")) { *k = K_AUTO; *n = 0; return 1; }
    char *e;
    double x = strtod(v, &e);
    if (e != v && !*e && x >= 0) { *k = K_NUM; *n = rnd(x * 100); return 1; }
    return lenv(v, F_PCT, k, n);
}

static void font_sh(char *val) {
    char buf[480], *tk[24];
    int n = toks(val, buf, sizeof buf, tk, 24), i = 0, ital = 0, wk = K_PX, wv = 400, sk = -1, sv = 0, lk = K_AUTO, lv = 0;
    if (n == 1 && kwi("caption|icon|menu|message-box|small-caption|status-bar", tk[0]) >= 0) return;
    for (; i < n; i++) {
        int k, v;
        if (!strcasecmp(tk[i], "normal") || !strcasecmp(tk[i], "small-caps") || kwi("ultra-condensed|extra-condensed|condensed|semi-condensed|semi-expanded|expanded|extra-expanded|ultra-expanded", tk[i]) >= 0) continue;
        if (!strcasecmp(tk[i], "italic") || !strcasecmp(tk[i], "oblique")) { ital = 1; continue; }
        if ((isalpha((unsigned char)tk[i][0]) || strlen(tk[i]) == 3) && weight_val(tk[i], &k, &v)) { wk = k; wv = v; continue; }
        if (fsize_val(tk[i], &k, &v)) { sk = k; sv = v; i++; break; }
        return;
    }
    if (sk < 0) return;
    if (i + 1 < n && !strcmp(tk[i], "/")) { if (!lh_val(tk[i + 1], &lk, &lv)) return; i += 2; }
    char fam[300] = "";
    for (; i < n; i++) { if (fam[0]) strlcat(fam, ",", sizeof fam); strlcat(fam, tk[i], sizeof fam); }
    if (!fam[0]) return;
    put(P_ITALIC, K_PX, ital); put(P_FWEIGHT, wk, wv); put(P_FSIZE, sk, sv); put(P_LH, lk, lv);
    int f = classify_family(fam);
    put(P_FAMILY, f < 0 ? K_UNSET : K_PX, f < 0 ? 0 : f);
}

static void flex_sh(char *val) {
    char buf[200], *tk[4];
    int n = toks(val, buf, sizeof buf, tk, 4), g = 100, s = 100, bk = K_PX, bv = 0, nums = 0;
    if (n == 1 && !strcasecmp(tk[0], "none")) { g = 0; s = 0; bk = K_AUTO; }
    else if (n == 1 && !strcasecmp(tk[0], "auto")) bk = K_AUTO;
    else if (n == 1 && !strcasecmp(tk[0], "initial")) { g = 0; bk = K_AUTO; }
    else for (int i = 0; i < n; i++) {
        int k, v;
        char *e;
        double x = strtod(tk[i], &e);
        if (e != tk[i] && !*e) {                          /* a bare number: grow, then shrink, then (a third / lone 0) basis */
            if (nums == 0) g = rnd(x * 100);
            else if (nums == 1) s = rnd(x * 100);
            else { bk = K_PX; bv = 0; }
            nums++;
        } else if (!strcasecmp(tk[i], "auto")) bk = K_AUTO;
        else if (!strcasecmp(tk[i], "content")) bk = K_NONE;
        else if (lenv(tk[i], F_PCT, &k, &v)) { bk = k; bv = v; }
        else return;
    }
    put(P_FGROW, K_NUM, g); put(P_FSHRINK, K_NUM, s); put(P_FBASIS, bk, bv);
}

static int grid_line(const char *t, int *k, int *v) {
    if (!strcasecmp(t, "auto")) { *k = K_AUTO; *v = 0; return 1; }
    if (!strncasecmp(t, "span", 4) && isspace((unsigned char)t[4])) { *k = K_SPAN; *v = atoi(t + 5); if (*v < 1) *v = 1; return 1; }
    char *e;
    long x = strtol(t, &e, 10);
    if (e != t && !*e && x) { *k = K_PX; *v = x; return 1; }
    if (isalpha((unsigned char)*t) || *t == '-' || *t == '_') { *k = K_STR; *v = intern(t, strlen(t)); return 1; }
    return 0;
}

/* "a / b / c": pieces split at top-level slashes (trimmed in place); returns count */
static int slash_split(char *s, char **out, int max) {
    int n = 0, d = 0;
    out[n++] = s;
    for (char *p = s; *p; p++) {
        if (*p == '(') d++; else if (*p == ')' && d) d--;
        else if (*p == '/' && !d && n < max) { *p = 0; out[n++] = p + 1; }
    }
    for (int i = 0; i < n; i++) trim(out[i]);
    return n;
}

static void grid_track(int prop, const char *val, int auto_none) {
    if (!strcasecmp(val, "none") || (auto_none && !strcasecmp(val, "auto"))) put(prop, K_NONE, 0);
    else put(prop, K_STR, intern(val, strlen(val)));
}

static void special(const struct pdef *d, char *val) {
    int k, v, p = d->p;
    switch (p) {
    case P_FSIZE: if (fsize_val(val, &k, &v) && v >= 0) put(p, k, v); break;
    case P_FWEIGHT: if (weight_val(val, &k, &v)) put(p, k, v); break;
    case P_LH: if (lh_val(val, &k, &v)) put(p, k, v); break;
    case P_OPACITY: {
        const char *q = val;
        double x; int pc; char u[8];
        if (cnum(&q, &x, &pc, u) && !*u) { if (pc) x /= 100; put(p, K_PX, cl255(x * 255)); }
        break;
    }
    case P_BWT: case P_BWR: case P_BWB: case P_BWL: if (bw_value(val, &k, &v)) put(p, k, v); break;
    case P_VALIGN: {
        int a = kwi("baseline|top|middle|bottom|text-top|text-bottom|sub|super", val);
        if (a >= 0) { put(P_VALIGN, K_PX, a); put(P_VAOFF, K_PX, 0); }
        else if (lenv(val, F_PCT | F_NEG, &k, &v)) { put(P_VALIGN, K_PX, VA_BASELINE); put(P_VAOFF, k, v); }
        break;
    }
    case P_WBRK:                                          /* 0 normal, 1 break-word (overflow-wrap), 2 break-all, 3 keep-all */
        if (!strcmp(d->n, "overflow-wrap")) { int a = kwi("normal|break-word|anywhere:1", val); if (a >= 0) put(p, K_PX, a); }
        else { int a = kwi("normal|break-all|keep-all|break-word", val); if (a >= 0) put(p, K_PX, a == 0 ? 0 : a == 1 ? 2 : a == 2 ? 3 : 1); }
        break;
    case P_FAMILY: { int f = classify_family(val); if (f >= 0) put(p, K_PX, f); break; }
    case P_CONTENT:
        if (!strcasecmp(val, "none") || !strcasecmp(val, "normal")) put(p, K_NONE, 0);
        else put(p, K_RAW, intern(val, strlen(val)));
        break;
    case P_BGIMG: {
        char url[URL_MAX], buf[900], *tk[16];
        int n = toks(val, buf, sizeof buf, tk, 16), got = 0;
        if (!n) break;
        for (int i = 0; i < n && got < 2; i++) {
            int pr = got ? P_BG2IMG : P_BGIMG;
            if (!strncasecmp(tk[i], "url(", 4) && url_arg(tk[i], url, sizeof url)) { put(pr, K_STR, intern(url, strlen(url))); got++; }
            else if (strstr(tk[i], "gradient(")) { put(pr, K_GRAD, intern(tk[i], strlen(tk[i]))); got++; }
            else if (!strncasecmp(tk[i], "none", 4) && i == 0) { put(pr, K_NONE, 0); got++; }
        }
        if (got < 2) put(P_BG2IMG, K_NONE, 0);
        break;
    }
    case P_BGREP: {
        char buf[100], *tk[3];
        int n = toks(val, buf, sizeof buf, tk, 3);
        if (n >= 1 && is_rep(tk[0])) put(p, K_PX, rep_val(tk[0], n > 1 ? tk[1] : 0));
        break;
    }
    case P_BGPX: {
        char buf[200], *tk[6];
        int n = toks(val, buf, sizeof buf, tk, 6), kx, vx, ky, vy;
        if (pos_parse(tk, n, &kx, &vx, &ky, &vy)) { put(P_BGPX, kx, vx); put(P_BGPY, ky, vy); }
        break;
    }
    case P_BGSW: {
        char buf[100], *tk[3];
        int n = toks(val, buf, sizeof buf, tk, 3), k2, v2;
        if (n == 1 && !strcasecmp(tk[0], "cover")) { put(P_BGSW, K_COVER, 0); put(P_BGSH, K_COVER, 0); }
        else if (n == 1 && !strcasecmp(tk[0], "contain")) { put(P_BGSW, K_CONTAIN, 0); put(P_BGSH, K_CONTAIN, 0); }
        else if (n >= 1 && lenv(tk[0], F_AUTO | F_PCT, &k, &v)) {
            if (n < 2) { k2 = K_AUTO; v2 = 0; } else if (!lenv(tk[1], F_AUTO | F_PCT, &k2, &v2)) break;
            put(P_BGSW, k, v); put(P_BGSH, k2, v2);
        }
        break;
    }
    case P_FBASIS:
        if (!strcasecmp(val, "content")) put(p, K_NONE, 0);
        else if (lenv(val, F_AUTO | F_PCT | F_FIT, &k, &v)) put(p, k, v);
        break;
    case P_SPACING: {
        char buf[100], *tk[3];
        int n = toks(val, buf, sizeof buf, tk, 3), k2, v2;
        if (n >= 1 && lenv(tk[0], 0, &k, &v)) {
            if (n < 2) { k2 = k; v2 = v; } else if (!lenv(tk[1], 0, &k2, &v2)) break;
            put(P_SPACING, k, v); put(P_SPACINGY, k2, v2);
        }
        break;
    }
    case P_ASPECT: {
        char buf[100], *tk[5];
        int n = toks(val, buf, sizeof buf, tk, 5), i = 0;
        if (!n) break;
        if (!strcasecmp(tk[0], "auto")) { i = 1; if (n == 1) { put(p, K_AUTO, 0); break; } }
        if (i >= n) break;
        double a = atof(tk[i]), b = 1;
        if (i + 2 < n && !strcmp(tk[i + 1], "/")) b = atof(tk[i + 2]);
        if (a > 0 && b > 0) put(p, K_PX, rnd(a / b * 1000));
        break;
    }
    case P_OUTW: if (bw_value(val, &k, &v)) put(p, k, v); break;
    case P_CRESET: case P_CINC: if (!strcasecmp(val, "none")) put(p, K_NONE, 0); else put(p, K_STR, intern(val, strlen(val))); break;
    case P_OBJPX: {
        char buf[200], *tk[6];
        int n = toks(val, buf, sizeof buf, tk, 6), kx, vx, ky, vy;
        if (pos_parse(tk, n, &kx, &vx, &ky, &vy)) { put(P_OBJPX, kx, vx); put(P_OBJPY, ky, vy); }
        break;
    }
    case P_TRX: {                                          /* only translate() / translateX / translateY / translate3d, as an offset */
        int kx = K_PX, vx = 0, ky = K_PX, vy = 0, found = 0;
        for (const char *q = val; *q && !found; q++) {
            if (strncasecmp(q, "translate", 9) || (q > val && (isalnum((unsigned char)q[-1]) || q[-1] == '-'))) continue;
            const char *a = q + 9;
            int axis = 0;                                  /* 0 both, 1 X, 2 Y */
            if ((*a | 32) == 'x' && a[1] == '(') { axis = 1; a++; } else if ((*a | 32) == 'y' && a[1] == '(') { axis = 2; a++; }
            else if (!strncasecmp(a, "3d(", 3)) a += 2;
            else if (*a != '(') continue;
            const char *e = a + 1;
            int d = 1;
            while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
            if (d) continue;                               /* unterminated */
            char args[200];
            size_t l = (size_t)(e - 1 - (a + 1)) < sizeof args - 1 ? (size_t)(e - 1 - (a + 1)) : sizeof args - 1;
            memcpy(args, a + 1, l); args[l] = 0;
            char *c1 = args, *c2 = 0;
            int dd = 0;
            for (char *w = args; *w; w++) { if (*w == '(') dd++; else if (*w == ')') dd--; else if (*w == ',' && !dd) { *w = 0; if (!c2) c2 = w + 1; else break; } }
            trim(c1);
            int ak, av;
            if (axis == 2) { if (lenv(c1, F_PCT | F_NEG, &ak, &av)) { ky = ak; vy = av; found = 1; } }
            else if (lenv(c1, F_PCT | F_NEG, &ak, &av)) {
                kx = ak; vx = av; found = 1;
                if (axis == 0 && c2) { trim(c2); if (lenv(c2, F_PCT | F_NEG, &ak, &av)) { ky = ak; vy = av; } }
            }
        }
        int sx = 1000, sy = 1000, rot = 0;
        for (const char *q = val; *q; q++) {                  /* scale()/scaleX/scaleY and rotate(): kept as numbers for LAYOUT to approximate or ignore */
            if (q > val && (isalnum((unsigned char)q[-1]) || q[-1] == '-')) continue;
            int sc = !strncasecmp(q, "scale", 5), ro = !strncasecmp(q, "rotate(", 7);
            if (!sc && !ro) continue;
            const char *a = q + (ro ? 6 : 5);
            int axis = 0;
            if (sc && (*a | 32) == 'x') { axis = 1; a++; } else if (sc && (*a | 32) == 'y') { axis = 2; a++; }
            if (*a != '(') continue;
            a++;
            double x, y;
            int pc;
            char u[8];
            if (!cnum(&a, &x, &pc, u)) continue;
            if (ro) { double dg; if (angle_deg(x, u, &dg)) rot = rnd(dg * 10); continue; }
            y = x;
            while (isspace((unsigned char)*a)) a++;
            if (*a == ',') { a++; if (!cnum(&a, &y, &pc, u)) y = x; }
            if (axis == 1) sx = rnd(x * 1000); else if (axis == 2) sy = rnd(x * 1000); else { sx = rnd(x * 1000); sy = rnd(y * 1000); }
        }
        put(P_TRX, kx, vx); put(P_TRY, ky, vy); put(P_TRSX, K_PX, sx); put(P_TRSY, K_PX, sy); put(P_TRROT, K_PX, rot);
        break;
    }
    case P_SHX: {                                          /* first shadow layer: [inset] x y [blur [spread]] [colour] */
        if (!strcasecmp(val, "none")) { put(P_SHC, K_NONE, 0); break; }
        char first[300], buf[320], *tk[10];
        int d = 0, k2 = 0;
        for (const char *w = val; *w && k2 < 299; w++) { if (*w == '(') d++; else if (*w == ')' && d) d--; else if (*w == ',' && !d) break; first[k2++] = *w; }
        first[k2] = 0;
        int n = toks(first, buf, sizeof buf, tk, 10), nl = 0, kk[4], vv[4], inset = 0, ck = K_CUR, cv = 0;
        for (int i = 0; i < n; i++) {
            int a, b;
            if (!strcasecmp(tk[i], "inset")) inset = 1;
            else if (parse_color_val(tk[i], &a, &b)) { ck = a; cv = b; }
            else if (nl < 4 && lenv(tk[i], F_NEG, &kk[nl], &vv[nl])) nl++;
            else return;
        }
        if (nl < 2) break;
        put(P_SHX, kk[0], vv[0]); put(P_SHY, kk[1], vv[1]); put(P_SHB, nl > 2 ? kk[2] : K_PX, nl > 2 ? vv[2] : 0);
        put(P_SHS, nl > 3 ? kk[3] : K_PX, nl > 3 ? vv[3] : 0); put(P_SHC, ck, cv); put(P_SHI, K_PX, inset);
        break;
    }
    case P_MASKIMG: {
        char url[URL_MAX], buf[600], *tk[12];
        int n = toks(val, buf, sizeof buf, tk, 12);
        for (int i = 0; i < n; i++) {                      /* also the `mask` shorthand: the first image token */
            if (!strncasecmp(tk[i], "url(", 4) && url_arg(tk[i], url, sizeof url)) { put(p, K_STR, intern(url, strlen(url))); return; }
            if (strstr(tk[i], "gradient(")) { put(p, K_GRAD, intern(tk[i], strlen(tk[i]))); return; }
        }
        put(p, K_NONE, 0);
        break;
    }
    case P_GCOLS: case P_GROWS: grid_track(p, val, 0); break;
    case P_GACOLS: case P_GAROWS: grid_track(p, val, 1); break;
    case P_GAREAS: if (!strcasecmp(val, "none")) put(p, K_NONE, 0); else put(p, K_STR, intern(val, strlen(val))); break;
    case P_GCS: case P_GCE: case P_GRS: case P_GRE: if (grid_line(val, &k, &v)) put(p, k, v); break;
    case P_GAREA: {
        char *pc[4];
        int n = slash_split(val, pc, 4);
        int ident = n == 1 && (isalpha((unsigned char)*pc[0]) || *pc[0] == '_') && strncasecmp(pc[0], "span", 4) && strcasecmp(pc[0], "auto");
        if (ident) { put(P_GAREA, K_STR, intern(pc[0], strlen(pc[0]))); break; }
        static const uint8_t ord[4] = { P_GRS, P_GCS, P_GRE, P_GCE };
        for (int i = 0; i < 4; i++) {
            const char *t = i < n ? pc[i] : "auto";
            if (i >= n && i >= 2 && i - 2 < n) { const char *src = pc[i - 2]; if (isalpha((unsigned char)*src) && strncasecmp(src, "span", 4) && strcasecmp(src, "auto")) t = src; }
            if (grid_line(t, &k, &v)) put(ord[i], k, v);
        }
        put(P_GAREA, K_NONE, 0);
        break;
    }
    case P_DECO: {
        char buf[200], *tk[8];
        int n = toks(val, buf, sizeof buf, tk, 8), m = 0;
        for (int i = 0; i < n; i++) {
            if (!strcasecmp(tk[i], "underline")) m |= DECO_UNDERLINE; else if (!strcasecmp(tk[i], "line-through")) m |= DECO_LINETHROUGH;
            else if (!strcasecmp(tk[i], "overline")) m |= DECO_OVERLINE;
        }
        put(P_DECO, K_PX, m);
        break;
    }
    }
}

static void gk_apply(const uint8_t *props, int kind) { for (; *props != 255; props++) put(*props, kind, 0); }

static int has_var(const char *v) {
    for (; *v; v++) if ((*v == 'v' || *v == 'V') && !strncasecmp(v, "var(", 4)) return 1;
    return 0;
}

static void pair_sides(const uint8_t *sides, char *val, int fl, int a, int b) {    /* margin-inline: 1-2 values -> two sides */
    char buf[200], *tk[3];
    int n = toks(val, buf, sizeof buf, tk, 3), k1, v1, k2, v2;
    if (n < 1 || n > 2 || !lenv(tk[0], fl, &k1, &v1)) return;
    if (n == 2) { if (!lenv(tk[1], fl, &k2, &v2)) return; } else { k2 = k1; v2 = v1; }
    put(sides[a], k1, v1); put(sides[b], k2, v2);
}

static void place_sh(char *val, int pa, int pj) {
    char buf[200], *tk[3];
    int n = toks(val, buf, sizeof buf, tk, 3);
    if (n < 1 || n > 2) return;
    const struct pdef *a = find_pd(pa == P_ALIGNC ? "align-content" : pa == P_ALIGNI ? "align-items" : "align-self");
    int x = kwi(a->kw, tk[0]), y = kwi(a->kw, tk[n == 2 ? 1 : 0]);
    if (x < 0 || y < 0) return;
    put(pa, x == 100 ? K_AUTO : K_PX, x == 100 ? 0 : x);
    put(pj, y == 100 ? K_AUTO : K_PX, y == 100 ? 0 : y);
}

/* one declaration: name lowercase (custom properties keep case), val trimmed and mutable */
static void decl(char *name, char *val, int imp) {
    imp_cur = imp;
    if (name[0] == '-' && name[1] == '-') {                            /* custom property: raw text, var() resolved per element */
        put(P_CUSTOM, K_RAW, intern(val, strlen(val)));
        D[nd - 1].a = intern(name, strlen(name));
        return;
    }
    if (!strncmp(name, "-webkit-", 8) || !strncmp(name, "-moz-", 5) || !strncmp(name, "-ms-", 4)) {
        const char *b = strchr(name + 1, '-') + 1;
        if (find_pd(b) || !strcmp(b, "mask") || !strcmp(b, "flex") || !strcmp(b, "flex-flow") || !strcmp(b, "box-sizing") || !strcmp(b, "border-radius") || !strcmp(b, "order")) name = (char *)b;
        else if (!strcmp(name, "-webkit-line-clamp") || !strcmp(name, "-webkit-box-orient")) return;
    }
    for (unsigned i = 0; i < sizeof LOGICAL / sizeof *LOGICAL; i++) if (!strcmp(name, LOGICAL[i][0])) { name = (char *)LOGICAL[i][1]; break; }
    if (!strcmp(name, "mask")) name = (char *)"mask-image";
    if (has_var(val)) {
        put(P_RAWDECL, K_RAW, intern(val, strlen(val)));
        D[nd - 1].a = intern(name, strlen(name));
        return;
    }
    int gk = !strcasecmp(val, "inherit") ? K_INHERIT : !strcasecmp(val, "initial") ? K_INITIAL : !strcasecmp(val, "unset") || !strcasecmp(val, "revert") || !strcasecmp(val, "revert-layer") ? K_UNSET : 0;
    if (gk) {
        for (unsigned i = 0; i < sizeof GRP / sizeof *GRP; i++) if (!strcmp(GRP[i].n, name)) { gk_apply(GRP[i].p, gk); return; }
        const struct pdef *pd = find_pd(name);
        if (pd) put(pd->p, gk, 0);
        return;
    }
    const struct pdef *pd = find_pd(name);
    if (pd) {
        int k, v;
        switch (pd->ty) {
        case Y_COLOR:
            if (parse_color_val(val, &k, &v)) put(pd->p, pd->p == P_COLOR && k == K_CUR ? K_INHERIT : k, v);
            break;
        case Y_KW: {
            int i = kwi(pd->kw, val);
            if (i < 0) break;
            if (i == 100) put(pd->p, K_AUTO, 0); else put(pd->p, K_PX, i);
            break;
        }
        case Y_LEN: if (lenv(val, pd->fl, &k, &v)) put(pd->p, k, v); break;
        case Y_NUM: { char *e; double x = strtod(val, &e); if (e != val && !*e && x >= 0) put(pd->p, K_NUM, rnd(x * 100)); break; }
        case Y_INT: {
            if (!strcasecmp(val, "auto")) { if (pd->p == P_ZINDEX) put(pd->p, K_AUTO, 0); break; }
            char *e; double x = strtod(val, &e);
            if (e != val && !*e) put(pd->p, K_PX, (int)x);
            break;
        }
        case Y_SPEC: special(pd, val); break;
        }
        return;
    }
    char buf[400], *tk[6];
    if (!strcmp(name, "margin")) sides4(SIDE_M, val, F_AUTO | F_NEG | F_PCT);
    else if (!strcmp(name, "padding")) sides4(SIDE_P, val, F_PCT);
    else if (!strcmp(name, "inset")) sides4(SIDE_I, val, F_AUTO | F_NEG | F_PCT);
    else if (!strcmp(name, "margin-inline")) pair_sides(SIDE_M, val, F_AUTO | F_NEG | F_PCT, 3, 1);
    else if (!strcmp(name, "margin-block")) pair_sides(SIDE_M, val, F_AUTO | F_NEG | F_PCT, 0, 2);
    else if (!strcmp(name, "padding-inline")) pair_sides(SIDE_P, val, F_PCT, 3, 1);
    else if (!strcmp(name, "padding-block")) pair_sides(SIDE_P, val, F_PCT, 0, 2);
    else if (!strcmp(name, "inset-inline")) pair_sides(SIDE_I, val, F_AUTO | F_NEG | F_PCT, 3, 1);
    else if (!strcmp(name, "inset-block")) pair_sides(SIDE_I, val, F_AUTO | F_NEG | F_PCT, 0, 2);
    else if (!strcmp(name, "outline")) {
        int n = toks(val, buf, sizeof buf, tk, 5), wk = K_PX, wv = 3, st = BS_NONE, ck = K_CUR, cv = 0;
        for (int i = 0; i < n; i++) {
            int k, v;
            if (!strcasecmp(tk[i], "auto")) st = BS_SOLID;
            else if (kwi(BSL, tk[i]) >= 0) st = kwi(BSL, tk[i]);
            else if (bw_value(tk[i], &k, &v)) { wk = k; wv = v; }
            else if (parse_color_val(tk[i], &k, &v)) { ck = k; cv = v; }
            else if (!strcasecmp(tk[i], "invert")) ck = K_CUR;
            else return;
        }
        put(P_OUTW, wk, wv); put(P_OUTS, K_PX, st); put(P_OUTC, ck, cv);
    }
    else if (!strcmp(name, "border")) border_sh(-1, val);
    else if (!strcmp(name, "border-top")) border_sh(0, val);
    else if (!strcmp(name, "border-right")) border_sh(1, val);
    else if (!strcmp(name, "border-bottom")) border_sh(2, val);
    else if (!strcmp(name, "border-left")) border_sh(3, val);
    else if (!strcmp(name, "border-inline")) { char c[200]; strlcpy(c, val, sizeof c); border_sh(3, c); border_sh(1, val); }
    else if (!strcmp(name, "border-block")) { char c[200]; strlcpy(c, val, sizeof c); border_sh(0, c); border_sh(2, val); }
    else if (!strcmp(name, "border-width")) sides4(SIDE_BW, val, 0x100);
    else if (!strcmp(name, "border-style")) {
        int n = toks(val, buf, sizeof buf, tk, 5), s[4];
        if (n < 1 || n > 4) return;
        for (int i = 0; i < n; i++) if ((s[i] = kwi(BSL, tk[i])) < 0) return;
        for (int i = 0; i < 4; i++) put(SIDE_BS[i], K_PX, s[MAP4[n][i]]);
    }
    else if (!strcmp(name, "border-color")) {
        int n = toks(val, buf, sizeof buf, tk, 5), k[4], v[4];
        if (n < 1 || n > 4) return;
        for (int i = 0; i < n; i++) if (!parse_color_val(tk[i], &k[i], &v[i])) return;
        for (int i = 0; i < 4; i++) put(SIDE_BC[i], k[MAP4[n][i]], v[MAP4[n][i]]);
    }
    else if (!strcmp(name, "border-radius")) {
        int n = toks(val, buf, sizeof buf, tk, 6), m = 0, k[4], v[4];
        while (m < n && strcmp(tk[m], "/")) m++;                        /* the part after '/' (vertical radii) is dropped */
        if (m < 1 || m > 4) return;
        for (int i = 0; i < m; i++) if (!lenv(tk[i], F_PCT, &k[i], &v[i])) return;
        for (int i = 0; i < 4; i++) put(CORNER[i], k[MAP4[m][i]], v[MAP4[m][i]]);
    }
    else if (!strcmp(name, "background")) background_sh(val);
    else if (!strcmp(name, "font")) font_sh(val);
    else if (!strcmp(name, "flex")) flex_sh(val);
    else if (!strcmp(name, "flex-flow")) {
        int n = toks(val, buf, sizeof buf, tk, 3), d = 0, w = 0;
        for (int i = 0; i < n; i++) {
            int a = kwi("row|row-reverse|column|column-reverse", tk[i]), b = kwi("nowrap|wrap|wrap-reverse", tk[i]);
            if (a >= 0) d = a; else if (b >= 0) w = b; else return;
        }
        put(P_FLEXDIR, K_PX, d); put(P_FLEXWRAP, K_PX, w);
    }
    else if (!strcmp(name, "gap") || !strcmp(name, "grid-gap")) {
        int n = toks(val, buf, sizeof buf, tk, 3), k1, v1, k2, v2;
        if (n < 1 || n > 2 || !lenv(tk[0], F_PCT | F_NORMAL, &k1, &v1)) return;
        if (n == 2) { if (!lenv(tk[1], F_PCT | F_NORMAL, &k2, &v2)) return; } else { k2 = k1; v2 = v1; }
        put(P_RGAP, k1, v1); put(P_CGAP, k2, v2);
    }
    else if (!strcmp(name, "overflow")) {
        int n = toks(val, buf, sizeof buf, tk, 3), x, y;
        if (n < 1 || n > 2) return;
        x = kwi("visible|hidden|scroll|auto|clip|overlay:3", tk[0]); y = kwi("visible|hidden|scroll|auto|clip|overlay:3", tk[n - 1]);
        if (x < 0 || y < 0) return;
        put(P_OVX, K_PX, x); put(P_OVY, K_PX, y);
    }
    else if (!strcmp(name, "list-style")) {
        int n = toks(val, buf, sizeof buf, tk, 5), t = LT_DISC, p = 0;
        for (int i = 0; i < n; i++) {
            const struct pdef *lt = find_pd("list-style-type");
            int a = kwi("outside|inside", tk[i]), b = kwi(lt->kw, tk[i]);
            if (a >= 0) p = a; else if (b >= 0) t = b; else if (!strncasecmp(tk[i], "url(", 4) || tk[i][0] == '"' || tk[i][0] == '\'') ; else return;
        }
        put(P_LISTT, K_PX, t); put(P_LISTP, K_PX, p);
    }
    else if (!strcmp(name, "place-items")) place_sh(val, P_ALIGNI, P_JUSTI);
    else if (!strcmp(name, "place-content")) place_sh(val, P_ALIGNC, P_JUSTC);
    else if (!strcmp(name, "place-self")) place_sh(val, P_ALIGNS, P_JUSTS);
    else if (!strcmp(name, "text-decoration")) special(find_pd("text-decoration-line"), val);
    else if (!strcmp(name, "grid-column") || !strcmp(name, "grid-row")) {
        char *pc[2];
        int n = slash_split(val, pc, 2), k, v, col = name[5] == 'c';
        if (n < 1) return;
        if (grid_line(pc[0], &k, &v)) put(col ? P_GCS : P_GRS, k, v);
        if (n > 1 && grid_line(pc[1], &k, &v)) put(col ? P_GCE : P_GRE, k, v);
        else put(col ? P_GCE : P_GRE, K_AUTO, 0);
    }
    else if (!strcmp(name, "grid-template") || !strcmp(name, "grid")) {
        if (strchr(val, '"') || strchr(val, '\'') || strstr(val, "auto-flow")) return;       /* areas / auto-flow forms: not handled */
        char *pc[2];
        int n = slash_split(val, pc, 2);
        if (n == 2) { grid_track(P_GROWS, pc[0], 0); grid_track(P_GCOLS, pc[1], 0); put(P_GAREAS, K_NONE, 0); }
    }
}

/* name: value; name: value !important ... ; nested rules ({...} blocks inside) are skipped */
static void parse_decls(const char *s, size_t n) {
    char *buf = malloc(n + 1);
    if (!buf) return;
    memcpy(buf, s, n);
    buf[n] = 0;
    char *p = buf;
    while (*p) {
        char *semi = p;
        int depth = 0, nested = 0;
        char q = 0;
        for (; *semi && (q || depth || *semi != ';'); semi++) {          /* ';' inside "url(..;..)", strings, {} doesn't end it */
            if (q) { if (*semi == '\\' && semi[1]) semi++; else if (*semi == q) q = 0; }
            else if (*semi == '"' || *semi == '\'') q = *semi;
            else if (*semi == '(' || *semi == '[') depth++;
            else if ((*semi == ')' || *semi == ']') && depth) depth--;
            else if (*semi == '{') {                                     /* nested rule: drop it, whole prelude + block */
                int d = 1;
                nested = 1;
                for (semi++; *semi && d; semi++) { if (*semi == '{') d++; else if (*semi == '}') d--; }
                break;
            }
        }
        char *end = semi;
        if (nested) { p = end; continue; }
        if (*semi) *semi++ = 0;
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            char *name = p, *val = colon + 1;
            while (isspace((unsigned char)*name)) name++;
            char *ne = name + strlen(name);
            while (ne > name && isspace((unsigned char)ne[-1])) *--ne = 0;
            if (!(name[0] == '-' && name[1] == '-')) for (char *c = name; *c; c++) *c = tolower((unsigned char)*c);
            while (isspace((unsigned char)*val)) val++;
            char *ve = end;
            while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0;
            int imp = 0;
            char *bang = strrchr(val, '!');
            if (bang) {
                char *w = bang + 1;
                while (isspace((unsigned char)*w)) w++;
                if (!strncasecmp(w, "important", 9)) {
                    imp = 1;
                    *bang = 0;
                    for (ve = bang; ve > val && isspace((unsigned char)ve[-1]); ) *--ve = 0;
                }
            }
            if (*name && (*val || (name[0] == '-' && name[1] == '-'))) decl(name, val, imp);
        }
        p = semi;
    }
    free(buf);
}

/* ---------- @media / @supports ---------- */
static int mq_value(const char *s, double *v) {     /* number [unit]; "a/b" ratio; length -> px, resolution -> dppx */
    const char *p = s;
    double x, y = 1;
    int pc;
    char u[8];
    while (isspace((unsigned char)*s)) s++;
    if (!strncmp(s, "calc(", 5) || !strncmp(s, "min(", 4) || !strncmp(s, "max(", 4) || !strncmp(s, "clamp(", 6)) {
        int mk = ncn, k, kv;
        int ok = calc_compile(s, 16, 16, &k, &kv) && k == K_PX;
        ncn = mk;
        if (ok) *v = kv;
        return ok;
    }
    p = s;
    if (!cnum(&p, &x, &pc, u)) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '/') { p++; char u2[8]; int pc2; if (!cnum(&p, &y, &pc2, u2) || y == 0) return 0; x /= y; }
    while (isspace((unsigned char)*p)) p++;
    if (*p) return 0;
    double m = 1;
    if (!*u || !strcmp(u, "px") || !strcmp(u, "dppx") || !strcmp(u, "x")) m = 1;
    else if (!strcmp(u, "em") || !strcmp(u, "rem")) m = 16;
    else if (!strcmp(u, "pt")) m = 4. / 3;
    else if (!strcmp(u, "in")) m = 96;
    else if (!strcmp(u, "cm")) m = 37.7953;
    else if (!strcmp(u, "mm")) m = 3.77953;
    else if (!strcmp(u, "dpi")) m = 1. / 96;
    else if (!strcmp(u, "dpcm")) m = 2.54 / 96;
    else return 0;
    *v = x * m;
    return 1;
}

static int cmp_op(int op, double a, double b) {     /* a OP b; op: '<' 'l' (<=) '>' 'g' (>=) '=' */
    return op == '<' ? a < b : op == 'l' ? a <= b : op == '>' ? a > b : op == 'g' ? a >= b : a == b;
}

static int mq_numeric(const char *f, double *have) {
    if (!strcmp(f, "width") || !strcmp(f, "device-width") || !strcmp(f, "inline-size")) *have = vpw;
    else if (!strcmp(f, "height") || !strcmp(f, "device-height") || !strcmp(f, "block-size")) *have = vph;
    else if (!strcmp(f, "aspect-ratio") || !strcmp(f, "device-aspect-ratio")) *have = (double)vpw / vph;
    else if (!strcmp(f, "resolution")) *have = 1;
    else if (!strcmp(f, "color")) *have = 8;
    else if (!strcmp(f, "color-index") || !strcmp(f, "monochrome") || !strcmp(f, "grid")) *have = 0;
    else return 0;
    return 1;
}

static int mq_feature(const char *in) {              /* the text inside one (...) */
    char f[160];
    strlcpy(f, in, sizeof f);
    for (char *p = f; *p; p++) *p = tolower((unsigned char)*p);
    trim(f);
    char *colon = strchr(f, ':');
    double have, want;
    if (colon) {
        *colon = 0;
        char *val = colon + 1;
        trim(f); trim(val);
        int op = '=';
        char *nm = f;
        if (!strncmp(nm, "min-", 4)) { op = 'g'; nm += 4; } else if (!strncmp(nm, "max-", 4)) { op = 'l'; nm += 4; }
        if (mq_numeric(nm, &have)) return mq_value(val, &want) && cmp_op(op, have, want + (op == '=' ? 0 : 0));
        if (!strcmp(f, "orientation")) return !strcmp(val, "landscape") ? vpw >= vph : !strcmp(val, "portrait") ? vpw < vph : 0;
        if (!strcmp(f, "prefers-color-scheme")) return !strcmp(val, "light");
        if (!strcmp(f, "prefers-reduced-motion") || !strcmp(f, "prefers-contrast") || !strcmp(f, "prefers-reduced-transparency") || !strcmp(f, "prefers-reduced-data")) return !strcmp(val, "no-preference");
        if (!strcmp(f, "hover") || !strcmp(f, "any-hover")) return !strcmp(val, "hover");
        if (!strcmp(f, "pointer") || !strcmp(f, "any-pointer")) return !strcmp(val, "fine");
        if (!strcmp(f, "forced-colors") || !strcmp(f, "inverted-colors") || !strcmp(f, "prefers-contrast")) return !strcmp(val, "none");
        if (!strcmp(f, "display-mode")) return !strcmp(val, "browser");
        if (!strcmp(f, "scripting")) return !strcmp(val, "enabled");
        if (!strcmp(f, "update")) return !strcmp(val, "fast");
        if (!strcmp(f, "color-gamut")) return !strcmp(val, "srgb");
        if (!strcmp(f, "dynamic-range") || !strcmp(f, "video-dynamic-range")) return !strcmp(val, "standard");
        if (!strcmp(f, "-webkit-device-pixel-ratio") || !strcmp(f, "-webkit-min-device-pixel-ratio") || !strcmp(f, "-webkit-max-device-pixel-ratio")) {
            double w; if (!mq_value(val, &w)) return 0;
            return strstr(f, "min") ? 1 >= w : strstr(f, "max") ? 1 <= w : w == 1;
        }
        return 0;
    }
    /* range syntax: operands at the operators < <= > >= = */
    char *ops[2]; int nops = 0;
    for (char *p = f; *p && nops < 2; p++) if (*p == '<' || *p == '>' || *p == '=') { ops[nops++] = p; if ((*p == '<' || *p == '>') && p[1] == '=') p++; }
    if (nops) {
        char *opnd[3]; int opc[2];
        char *p = f;
        for (int i = 0; i < nops; i++) {
            opnd[i] = p;
            char c = *ops[i], c2 = ops[i][1] == '=' && c != '=' ? '=' : 0;
            opc[i] = c == '=' ? '=' : c2 ? (c == '<' ? 'l' : 'g') : c;
            *ops[i] = 0;
            p = ops[i] + 1 + (c2 ? 1 : 0);
        }
        opnd[nops] = p;
        for (int i = 0; i <= nops; i++) trim(opnd[i]);
        int fi = -1;
        for (int i = 0; i <= nops; i++) if (isalpha((unsigned char)opnd[i][0]) && mq_numeric(opnd[i], &have)) fi = i;
        if (fi < 0) return 0;
        int ok = 1;
        for (int i = 0; i < nops; i++) {
            double w;
            int op = opc[i];
            if (i == fi) { if (!mq_value(opnd[i + 1], &w)) return 0; ok &= cmp_op(op, have, w); }            /* name op value */
            else if (i + 1 == fi) {                                                                           /* value op name */
                if (!mq_value(opnd[i], &w)) return 0;
                ok &= cmp_op(op, w, have);
            } else return 0;
        }
        return ok;
    }
    if (mq_numeric(f, &have)) return have != 0;                                         /* (color), (width) */
    if (!strcmp(f, "hover") || !strcmp(f, "any-hover") || !strcmp(f, "pointer") || !strcmp(f, "any-pointer") || !strcmp(f, "orientation") || !strcmp(f, "scripting") || !strcmp(f, "update")) return 1;
    return 0;
}

static int mq_cond(const char **pp, const char *end);
static int mq_term(const char **pp, const char *end) {
    const char *p = *pp;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) { *pp = p; return 0; }
    if (*p == '(') {
        const char *e = p + 1;
        int d = 1;
        while (e < end && d) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
        const char *inner = p + 1, *ie = e - 1;
        *pp = e;
        while (inner < ie && isspace((unsigned char)*inner)) inner++;
        if (inner < ie && (*inner == '(' || !strncasecmp(inner, "not", 3))) {          /* ( condition ) */
            const char *q = inner;
            return mq_cond(&q, ie);
        }
        char b[160];
        size_t l = (size_t)(ie - inner) < sizeof b - 1 ? (size_t)(ie - inner) : sizeof b - 1;
        memcpy(b, inner, l); b[l] = 0;
        return mq_feature(b);
    }
    while (p < end && !isspace((unsigned char)*p) && *p != '(') p++;                  /* unknown function: false */
    if (p < end && *p == '(') { int d = 0; do { if (*p == '(') d++; else if (*p == ')') d--; p++; } while (p < end && d); }
    *pp = p;
    return 0;
}

static int word_is(const char **pp, const char *end, const char *w) {
    const char *p = *pp;
    while (p < end && isspace((unsigned char)*p)) p++;
    int l = strlen(w);
    if (end - p >= l && !strncasecmp(p, w, l) && (p + l == end || isspace((unsigned char)p[l]) || p[l] == '(')) { *pp = p + l; return 1; }
    return 0;
}

static int mq_cond(const char **pp, const char *end) {
    int v;
    if (word_is(pp, end, "not")) return !mq_term(pp, end);
    v = mq_term(pp, end);
    for (;;) {
        if (word_is(pp, end, "and")) v &= mq_term(pp, end) != 0;
        else if (word_is(pp, end, "or")) v |= mq_term(pp, end) != 0;
        else break;
    }
    return v;
}

static int media_one(const char *q, int n) {          /* [not|only] type [and condition]  |  condition */
    const char *p = q, *end = q + n;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return 1;
    const char *s = p;
    if (*s == '(') return mq_cond(&p, end);
    int neg = 0;
    if (word_is(&p, end, "not")) {
        const char *t = p;
        while (t < end && isspace((unsigned char)*t)) t++;
        if (t < end && *t == '(') { p = s; return mq_cond(&p, end); }                  /* not (feature) ... */
        neg = 1;
    } else word_is(&p, end, "only");
    int type = word_is(&p, end, "all") || word_is(&p, end, "screen");
    if (!type) {                                         /* print, speech, tv... : skip the word */
        while (p < end && isspace((unsigned char)*p)) p++;
        while (p < end && !isspace((unsigned char)*p)) p++;
    }
    int ok = type;
    if (word_is(&p, end, "and")) ok &= mq_cond(&p, end) != 0;
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
        if (e > a) { any = 1; if (media_one(q + a, e - a)) return 1; }
        a = b + 1;
    }
    return !any;
}

static int decl_known(const char *name, const char *val) {
    char n[80], v[300];
    strlcpy(n, name, sizeof n); strlcpy(v, val, sizeof v);
    trim(n); trim(v);
    if (!(n[0] == '-' && n[1] == '-')) for (char *c = n; *c; c++) *c = tolower((unsigned char)*c);
    int d0 = nd, sv = imp_cur;
    decl(n, v, 0);
    int ok = nd > d0;
    nd = d0; imp_cur = sv;
    return ok;
}

static int sup_cond(const char **pp, const char *end);
static int sup_term(const char **pp, const char *end) {
    const char *p = *pp;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return 0;
    if (!strncasecmp(p, "selector(", 9)) {
        const char *e = p + 9;
        int d = 1;
        while (e < end && d) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
        *pp = e;
        int a, b, c;
        struct sel s;
        sel_mark(&a, &b, &c);
        int ok = sel_parse(p + 9, (int)(e - 1 - (p + 9)), &s);
        sel_trunc(a, b, c);
        return ok;
    }
    if (*p == '(') {
        const char *e = p + 1;
        int d = 1;
        char q = 0;
        while (e < end && d) { if (q) { if (*e == q) q = 0; } else if (*e == '"' || *e == '\'') q = *e; else if (*e == '(') d++; else if (*e == ')') d--; e++; }
        const char *inner = p + 1, *ie = e - 1;
        *pp = e;
        while (inner < ie && isspace((unsigned char)*inner)) inner++;
        if (inner < ie && (*inner == '(' || !strncasecmp(inner, "not", 3) || !strncasecmp(inner, "selector(", 9))) { const char *t = inner; return sup_cond(&t, ie); }
        char b[400];
        size_t l = (size_t)(ie - inner) < sizeof b - 1 ? (size_t)(ie - inner) : sizeof b - 1;
        memcpy(b, inner, l); b[l] = 0;
        char *c = strchr(b, ':');
        if (!c) return 0;
        *c = 0;
        return decl_known(b, c + 1);
    }
    while (p < end && !isspace((unsigned char)*p)) p++;                                 /* font-tech(), unknown: false */
    *pp = p;
    return 0;
}
static int sup_cond(const char **pp, const char *end) {
    if (word_is(pp, end, "not")) return !sup_term(pp, end);
    int v = sup_term(pp, end);
    for (;;) {
        if (word_is(pp, end, "and")) v &= sup_term(pp, end) != 0;
        else if (word_is(pp, end, "or")) v |= sup_term(pp, end) != 0;
        else break;
    }
    return v;
}

/* ---------- rules ---------- */
static void link_rule(int i, int kind, uint32_t h) {
    int *head = kind == 1 ? &hid[h & (HB - 1)] : kind == 2 ? &hcl[h & (HB - 1)] : kind == 3 ? &htg[h & (HB - 1)] : &huni;
    R[i].next = *head; R[i].kh = h; R[i].bk = kind;
    *head = i + 1;
}
static void rebucket(int i) { link_rule(i, R[i].bk, R[i].kh); }      /* after a soft reset: same bucket as before */

/* a subject that is just :is(.a, .b, tag...) is indexed under each alternative's key instead of the "any element" list (Tailwind/Codex are full of these) */
static int alt_keys(const struct sel *s, int *kinds, uint32_t *hs, int max) {
    const struct comp *c = &CO[s->c0 + s->nc - 1];
    if (c->ns != 1 || SI[c->s0].t != T_IS || SI[c->s0].b > max) return 0;
    const struct simple *x = &SI[c->s0];
    for (int i = 0; i < x->b; i++) {
        const struct sel *q = &SE[x->a + i];
        sel_key(q, &kinds[i], &hs[i]);
        if (!kinds[i] || q->pe || q->kind) return 0;
    }
    return x->b;
}

static int rule_slot(void) {
    if (nr == rcap) {
        int nc = rcap ? rcap * 2 : 512;
        struct rule *p = realloc(R, nc * sizeof *p);
        if (!p) return 0;
        R = p; rcap = nc;
    }
    return 1;
}

static void add_rule(const struct sel *s, int d0, int ndl) {
    if (!rule_slot()) return;
    int si = sel_push(s);
    if (si < 0) return;
    R[nr] = (struct rule){ .sel = si, .d0 = d0, .nd = ndl, .order = order_n++, .scope = cur_scope, .layer = (uint8_t)(s->kind ? 0 : cur_layer), .ua = (uint8_t)cur_ua };   /* shadow-tree rules about the host / slotted nodes lose to the outer document */
    int kind, ak[8];
    uint32_t h, ah[8];
    sel_key(&SE[si], &kind, &h);
    if (SE[si].kind) { kind = 0; h = 0; }                /* :host / ::slotted rules don't match the element their last compound names */
    int na = kind ? 0 : alt_keys(&SE[si], ak, ah, 8);
    if (!na) { link_rule(nr++, kind, h); return; }
    struct rule base = R[nr];
    for (int j = 0; j < na; j++) {
        if (j && !rule_slot()) return;
        R[nr] = base;
        link_rule(nr++, ak[j], ah[j]);
    }
}

static int layer_rank(const char *name, int n) {
    int id = intern(name, n);
    for (int i = 0; i < nlay; i++) if (lay_name[i] == id) return i + 1;
    if (nlay >= 120) return 120;
    lay_name[nlay++] = id;
    any_layers = 1;
    return nlay;
}

static const char *skip_block(const char *s, const char *e) {       /* s at '{' (or just inside after); returns past the matching '}' */
    int d = 0;
    char q = 0;
    for (; s < e; s++) {
        if (q) { if (*s == '\\') s++; else if (*s == q) q = 0; continue; }
        if (*s == '"' || *s == '\'') q = *s;
        else if (*s == '{') d++;
        else if (*s == '}' && !--d) return s + 1;
    }
    return e;
}

static void sheet(const char *s, size_t m, const char *base, int depth);

/* @import "x" / url(x) [layer[(name)]] [supports(...)] [media]; the sheet comes from sheet.c (NULL while it loads: the page relayouts when it arrives) */
static void do_import(const char *pre, size_t n, const char *base, int depth) {
    char ref[URL_MAX], q[200];
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
    int saved_layer = cur_layer, ok = 1;
    for (;;) {                                           /* layer / layer(x) / supports(...) in any order, then media */
        while (i < n && isspace((unsigned char)pre[i])) i++;
        if (n - i >= 5 && !strncasecmp(pre + i, "layer", 5) && (i + 5 == n || pre[i + 5] == '(' || isspace((unsigned char)pre[i + 5]))) {
            i += 5;
            if (i < n && pre[i] == '(') {
                size_t b = ++i;
                while (i < n && pre[i] != ')') i++;
                cur_layer = layer_rank(pre + b, i - b);
                i++;
            } else cur_layer = layer_rank("(anon)", 6) + 0;
        } else if (n - i >= 9 && !strncasecmp(pre + i, "supports(", 9)) {
            size_t b = i + 8;
            int d = 0;
            i = b;
            do { if (pre[i] == '(') d++; else if (pre[i] == ')') d--; i++; } while (i < n && d);
            const char *p = pre + b, *e = pre + i;
            if (p < e && *p == '(') {                    /* supports(display: grid) wraps a bare declaration */
                char t[300];
                size_t l = (size_t)(e - p - 2) < sizeof t - 1 ? (size_t)(e - p - 2) : sizeof t - 1;
                memcpy(t, p + 1, l); t[l] = 0;
                if (strchr(t, ':') && t[0] != '(') { char *c = strchr(t, ':'); *c = 0; ok &= decl_known(t, c + 1); }
                else { const char *pp = p; ok &= sup_cond(&pp, e) != 0; }
            }
        } else break;
    }
    k = 0;
    while (i < n && k < (int)sizeof q - 1) q[k++] = pre[i++];
    q[k] = 0;
    if (ref[0] && ok && css_media(q) && depth < 4) {
        char full[URL_MAX];
        url_resolve(base ? base : "", ref, full, sizeof full);
        size_t len;
        const char *t = sheet_get(full, &len);
        if (t) { static int nest; if (nest < 4) { nest++; css_add_from(t, len, full); nest--; } }
    }
    cur_layer = saved_layer;
}

static void sheet(const char *s, size_t m, const char *base, int depth) {
    size_t i = 0;
    while (i < m) {
        while (i < m && isspace((unsigned char)s[i])) i++;
        if (i >= m) break;
        if (s[i] == '}') { i++; continue; }              /* stray close */
        if (s[i] == '@') {
            size_t ns = ++i;
            while (i < m && (isalpha((unsigned char)s[i]) || s[i] == '-')) i++;
            size_t nl = i - ns, ps = i;
            int par = 0;
            char qt = 0;
            for (; i < m && (qt || par || (s[i] != '{' && s[i] != ';')); i++) {
                if (qt) { if (s[i] == '\\') i++; else if (s[i] == qt) qt = 0; }
                else if (s[i] == '"' || s[i] == '\'') qt = s[i];
                else if (s[i] == '(') par++;
                else if (s[i] == ')' && par) par--;
            }
            size_t pe = i;
            #define IS(w) (nl == sizeof w - 1 && !strncasecmp(s + ns, w, nl))
            if (i >= m || s[i] == ';') {
                if (IS("import")) do_import(s + ps, pe - ps, base, depth);
                else if (IS("layer")) {                  /* @layer a, b;  fixes the order of the layers */
                    for (size_t a = ps; a <= pe; ) {
                        size_t b = a;
                        while (b < pe && s[b] != ',') b++;
                        while (a < b && isspace((unsigned char)s[a])) a++;
                        size_t e = b;
                        while (e > a && isspace((unsigned char)s[e - 1])) e--;
                        if (e > a) layer_rank(s + a, e - a);
                        a = b + 1;
                    }
                }
                i++;
                continue;
            }
            const char *be = skip_block(s + i, s + m);
            size_t bs = i + 1, bend = (size_t)(be - s - 1) > bs ? (size_t)(be - s - 1) : bs;
            if (be == s + m && (be[-1] != '}')) bend = m;
            char pre[400];
            size_t pl = pe - ps < sizeof pre - 1 ? pe - ps : sizeof pre - 1;
            memcpy(pre, s + ps, pl); pre[pl] = 0;
            if (depth < 8) {
                if (IS("media")) { if (css_media(pre)) sheet(s + bs, bend - bs, base, depth + 1); }
                else if (IS("container")) {                  /* [name] (condition): the container's size is unknown at style time, so the viewport stands in for it */
                    const char *q = pre;
                    while (*q && *q != '(' && strncasecmp(q, "not", 3) && strncasecmp(q, "style(", 6)) { while (*q && !isspace((unsigned char)*q) && *q != '(') q++; while (isspace((unsigned char)*q)) q++; }
                    if (mq_cond(&q, pre + pl)) sheet(s + bs, bend - bs, base, depth + 1);
                }
                else if (IS("supports")) { const char *p = pre; if (sup_cond(&p, pre + pl)) sheet(s + bs, bend - bs, base, depth + 1); }
                else if (IS("layer")) {
                    int saved = cur_layer;
                    trim(pre);
                    static int anon;
                    char nm[160];
                    if (pre[0]) strlcpy(nm, pre, sizeof nm); else { strlcpy(nm, "(anon", sizeof nm); nm[5] = '0' + anon++ % 10; nm[6] = '0' + anon / 10 % 10; nm[7] = 0; }
                    cur_layer = layer_rank(nm, strlen(nm));
                    sheet(s + bs, bend - bs, base, depth + 1);
                    cur_layer = saved;
                }
            }
            #undef IS
            i = be - s;                                  /* font-face, keyframes, page, container, ...: skipped whole */
            continue;
        }
        size_t ps = i;
        char qt = 0;
        int br = 0;
        while (i < m && (qt || br || s[i] != '{')) {
            if (qt) { if (s[i] == '\\') i++; else if (s[i] == qt) qt = 0; }
            else if (s[i] == '"' || s[i] == '\'') qt = s[i];
            else if (s[i] == '\\') i++;                      /* an escaped bracket (Tailwind: .w-\[3px\]) isn't one */
            else if (s[i] == '[' || s[i] == '(') br++;
            else if ((s[i] == ']' || s[i] == ')') && br) br--;
            else if (s[i] == ';' && !br) { ps = i + 1; }      /* junk statement before a rule: restart the prelude after it */
            i++;
        }
        if (i >= m) break;
        size_t pe = i;
        const char *be = skip_block(s + i, s + m);
        size_t bs = i + 1, bend = (size_t)(be - s - 1) > bs ? (size_t)(be - s - 1) : bs;
        if (be == s + m && be[-1] != '}') bend = m;
        i = be - s;
        int d0 = nd;
        imp_cur = 0;
        parse_decls(s + bs, bend - bs);
        if (nd == d0) continue;
        for (size_t a = ps; a <= pe; ) {                  /* comma list */
            size_t b = a;
            int dp = 0;
            char q2 = 0;
            while (b < pe && (q2 || dp || s[b] != ',')) {
                if (q2) { if (s[b] == '\\') b++; else if (s[b] == q2) q2 = 0; }
                else if (s[b] == '"' || s[b] == '\'') q2 = s[b];
                else if (s[b] == '\\') b++;
                else if (s[b] == '[' || s[b] == '(') dp++;
                else if ((s[b] == ']' || s[b] == ')') && dp) dp--;
                b++;
            }
            struct sel sl;
            if (nr < 60000 && sel_parse(s + a, b - a, &sl)) add_rule(&sl, d0, nd - d0);
            a = b + 1;
        }
    }
}

void css_add(const char *src, size_t n) { css_add_from(src, n, 0); }
void css_add_scoped(const char *src, size_t n, const char *base, int scope) { int sv = cur_scope; cur_scope = scope; css_add_from(src, n, base); cur_scope = sv; }

void css_add_from(const char *src, size_t n, const char *base) {
    char *s = malloc(n + 1);
    if (!s) return;
    size_t m = 0;
    char q = 0;
    for (size_t i = 0; i < n; ) {                       /* drop comments and <!-- --> wrappers */
        if (q) { if (src[i] == '\\' && i + 1 < n) s[m++] = src[i++]; else if (src[i] == q || src[i] == '\n') q = 0; s[m++] = src[i++]; }
        else if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
            s[m++] = ' ';
        } else if (i + 4 <= n && !strncmp(src + i, "<!--", 4)) i += 4;
        else if (i + 3 <= n && !strncmp(src + i, "-->", 3)) i += 3;
        else { if (src[i] == '"' || src[i] == '\'') q = src[i]; s[m++] = src[i++]; }
    }
    s[m] = 0;
    char sv[URL_MAX];
    strlcpy(sv, cur_base, sizeof sv);
    strlcpy(cur_base, base ? base : "", sizeof cur_base);
    sheet(s, m, base, 0);
    strlcpy(cur_base, sv, sizeof cur_base);
    free(s);
}

/* ---------- user-agent sheet ---------- */
static const char UA[] =
    "html,body,address,blockquote,center,dialog,div,figure,figcaption,footer,form,header,hr,legend,listing,main,p,plaintext,pre,search,xmp,"
    "article,aside,h1,h2,h3,h4,h5,h6,hgroup,nav,section,dir,dd,dl,dt,menu,ol,ul,details,summary,fieldset{display:block}"
    "head,link,meta,script,style,title,base,template,area,datalist,noembed,noframes,param,rp,source,track,[hidden]{display:none}"
    "slot{display:contents}"
    "li{display:list-item}"
    "body{margin:8px}"
    "p,dl,pre,listing,xmp,plaintext{margin:1em 0}"
    "blockquote,figure{margin:1em 40px}"
    "ol,ul,menu,dir{margin:1em 0;padding-left:40px}"
    "ol ol,ol ul,ul ol,ul ul,menu ul,menu ol,dir ul,dir ol{margin:0}"
    "dd{margin-left:40px}"
    "h1{font-size:2em;margin:.67em 0}h2{font-size:1.5em;margin:.83em 0}h3{font-size:1.17em;margin:1em 0}"
    "h4{margin:1.33em 0}h5{font-size:.83em;margin:1.67em 0}h6{font-size:.67em;margin:2.33em 0}"
    "h1,h2,h3,h4,h5,h6{font-weight:bold}"
    "ul,menu,dir{list-style-type:disc}ol{list-style-type:decimal}"
    "ul ul,ol ul,menu ul,dir ul{list-style-type:circle}"
    "ul ul ul,ol ul ul,ul ol ul,ol ol ul,ul ul ol,ol ul ol{list-style-type:square}ul ul ol,ol ul ol,ul ol ol,ol ol ol{list-style-type:decimal}"
    "hr{margin:.5em auto;border:1px inset;color:gray;overflow:hidden}"
    "pre,listing,xmp,plaintext{white-space:pre}"
    "pre,listing,xmp,plaintext,code,kbd,samp,tt{font-family:monospace}"
    "b,strong,th{font-weight:bolder}th{font-weight:bold}"
    "i,cite,em,var,address,dfn{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "small{font-size:smaller}big{font-size:larger}sub{vertical-align:sub;font-size:smaller}sup{vertical-align:super;font-size:smaller}"
    "mark{background-color:yellow;color:black}"
    "abbr[title],acronym[title]{text-decoration:underline dotted}"
    "a:any-link{text-decoration:underline;cursor:pointer}a:link{color:#0000ee}a:visited{color:#551a8b}"
    "center{text-align:center}nobr{white-space:nowrap}"
    "q::before{content:open-quote}q::after{content:close-quote}"
    "table{display:table;border-collapse:separate;border-spacing:2px;box-sizing:border-box;text-indent:0}"
    "caption{display:table-caption;text-align:center}"
    "colgroup{display:table-column-group}col{display:table-column}"
    "thead{display:table-header-group}tbody{display:table-row-group}tfoot{display:table-footer-group}tr{display:table-row}"
    "td,th{display:table-cell;padding:1px}"
    "thead,tbody,tfoot{vertical-align:middle}tr,td,th{vertical-align:inherit}"
    "input,select,textarea,button{display:inline-block}input[type=hidden]{display:none}textarea{white-space:pre-wrap}"
    "fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove gray}legend{padding:0 2px}"
    "summary{cursor:pointer}details:not([open])>:not(summary){display:none}"
    "dialog:not([open]){display:none}audio:not([controls]){display:none}"
    "img,video,canvas,svg,object,embed,iframe{display:inline}";

static void ua_init(void) {
    cur_ua = 1; cur_layer = 0;
    css_add_from(UA, sizeof UA - 1, 0);
    cur_ua = 0; cur_layer = 255;
    attrs_used = 0;
    ua_nr = nr; ua_nd = nd; ua_cd = CD;
    sel_mark(&ua_si, &ua_co, &ua_se);
    ua_ready = 1;
}

void css_reset(void) {
    if (!ua_ready || sp_n > (6 << 20)) {                  /* first time, or the string pool got big: start over (invalidates every computed style) */
        nr = nd = 0;
        sel_trunc(0, 0, 0);
        if (HT) memset(HT, 0, ht_cap * sizeof *HT);
        ht_n = 0; sp_n = SP ? 1 : 0;
        memset(hid, 0, sizeof hid); memset(hcl, 0, sizeof hcl); memset(htg, 0, sizeof htg); huni = 0;
        order_n = 0;
        memset(&CD, 0, sizeof CD);
        ua_init();
    } else {
        nr = ua_nr; nd = ua_nd;
        sel_trunc(ua_si, ua_co, ua_se);
        memset(hid, 0, sizeof hid); memset(hcl, 0, sizeof hcl); memset(htg, 0, sizeof htg); huni = 0;
        for (int i = 0; i < nr; i++) rebucket(i);
        order_n = nr;
        CD = ua_cd;
    }
    nlay = 0; any_layers = 0; attrs_used = 0; ncn = 0; ncv = 1;
        cur_layer = 255; cur_ua = 0; cur_scope = 0;
    cur_base[0] = 0;
}

/* ---------- custom properties and var() ---------- */
struct cvar { int name, raw, val, next, head; uint8_t st; };   /* st: 0 raw, 1 resolving, 2 done; val: interned, -1 = invalid */
static struct cvar *CV;

static int cv_new(int name, int raw, int next) {
    if (ncv >= ccv) {
        int nc = ccv ? ccv * 2 : 128;
        struct cvar *p = realloc(CV, nc * sizeof *p);
        if (!p) return 0;
        CV = p; ccv = nc;
        if (!ncv) ncv = 1;
    }
    CV[ncv] = (struct cvar){ name, raw, 0, next, 0, 0 };
    return ncv++;
}
static int cv_lookup(int head, int name) { for (int i = head; i; i = CV[i].next) if (CV[i].name == name) return i; return 0; }

static int subst(const char *in, int head, char *out, int cap, int depth);
static int cv_value(int i, int depth) {
    if (CV[i].st == 2) return CV[i].val;
    if (CV[i].st == 1 || depth > 8) return -1;
    CV[i].st = 1;
    int val;
    if (!has_var(STR(CV[i].raw))) val = CV[i].raw;
    else {
        char out[1024];
        int ok = subst(STR(CV[i].raw), CV[i].head, out, sizeof out, depth + 1);
        val = ok ? intern(out, strlen(out)) : -1;
    }
    CV[i].val = val; CV[i].st = 2;
    return val;
}

static int subst(const char *in, int head, char *out, int cap, int depth) {
    int k = 0;
    if (depth > 8) return 0;
    for (const char *p = in; *p; ) {
        if ((p[0] | 32) == 'v' && !strncasecmp(p, "var(", 4) && (p == in || !(isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_'))) {
            const char *a = p + 4, *e = a;
            int d = 1;
            char q = 0;
            while (*e && d) {
                if (q) { if (*e == q) q = 0; }
                else if (*e == '"' || *e == '\'') q = *e;
                else if (*e == '(') d++;
                else if (*e == ')') d--;
                e++;
            }
            if (d) return 0;
            const char *close = e - 1, *ns = a;
            while (isspace((unsigned char)*ns)) ns++;
            const char *ne = ns;
            while (ne < close && *ne != ',' && !isspace((unsigned char)*ne)) ne++;
            int name = intern(ns, ne - ns);
            const char *c2 = ne, *fb = 0;
            while (c2 < close && *c2 != ',') c2++;
            if (c2 < close) fb = c2 + 1;
            int ci = cv_lookup(head, name), val = ci ? cv_value(ci, depth) : -1;
            if (val >= 0) {
                int l = strlen(STR(val));
                if (k + l >= cap) return 0;
                memcpy(out + k, STR(val), l); k += l;
            } else if (fb) {
                char tmp[512];
                size_t l = (size_t)(close - fb) < sizeof tmp - 1 ? (size_t)(close - fb) : sizeof tmp - 1;
                memcpy(tmp, fb, l); tmp[l] = 0;
                if (!subst(tmp, head, out + k, cap - k, depth + 1)) return 0;
                k += strlen(out + k);
            } else return 0;
            p = e;
        } else {
            if (k >= cap - 1) return 0;
            out[k++] = *p++;
        }
    }
    out[k] = 0;
    return 1;
}

/* ---------- the document, as the matcher sees it ---------- */
/* legacy: the open-element stack (html.c, js.c). Nodes 1..n; node n+1 is a stand-in "some other sibling" so :last-child and
 * sibling combinators can't be answered (they don't match) while :first-child can. */
static const struct el *LS; static int LN;
static const char *at_find(const char *at, const char *name, int *vl) {
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
static int ls_parent(int n) { return n > 1 && n <= LN ? n - 1 : 0; }
static int ls_prev(int n) { return n <= LN && !LS[n - 1].first ? LN + 1 : 0; }
static int ls_next(int n) { return n <= LN ? LN + 1 : 0; }
static int ls_first(int n) { return 0; }
static const char *ls_tag(int n) { return n <= LN ? LS[n - 1].name : ""; }
static const char *ls_attr(int n, const char *name) {
    static char tmp[96];
    if (n > LN) return 0;
    const struct el *e = &LS[n - 1];
    if (!strcmp(name, "id")) return e->id[0] ? e->id : 0;
    if (!strcmp(name, "class")) return e->cls[0] ? e->cls : 0;
    int vl;
    const char *v = at_find(e->at, name, &vl);
    if (!v) return 0;
    if (vl >= (int)sizeof tmp) vl = sizeof tmp - 1;
    memcpy(tmp, v, vl); tmp[vl] = 0;
    return tmp;
}
static int ls_state(int n) { return n <= LN && !strcmp(LS[n - 1].name, "a") ? CS_LINK : 0; }
static int ls_text(int n) { return 1; }
static const struct css_dom LDOM = { ls_parent, ls_prev, ls_next, ls_first, ls_tag, ls_attr, ls_state, ls_text, 0, 0, 0 };

/* ---------- cascade ---------- */
struct hit { uint64_t key; int rule; };
static struct hit H[1024];

static uint32_t hash_n(const char *s, int n) { uint32_t h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u; return h; }

static const uint32_t *self_bloom;
static int try_chain(int head, uint32_t kh, int node, int pe, const uint32_t *anc, int legacy, int nh) {
    for (int r = head; r && nh < 1024; r = R[r - 1].next) {
        const struct rule *ru = &R[r - 1];
        if (kh && ru->kh != kh) continue;
        if (!ru->ua && !SE[ru->sel].kind && ru->scope != cur_nscope) continue;   /* encapsulation: a tree's rules only reach its own elements */                 /* bucket collision: a different tag/id/class */
        if (legacy && ru->ua) continue;
        const struct sel *s = &SE[ru->sel];
        if (s->pe != pe) continue;
        if (anc && !bloom_has(anc, s->mask)) continue;
        if (!bloom_has(self_bloom, s->rmask)) continue;
#ifdef CSS_STATS
        { extern int *rule_cand; rule_cand[r - 1]++; }
#endif
        if (!sel_match_a(s, node, ru->scope, anc, anc ? self_bloom : 0)) continue;
#ifdef CSS_STATS
        { extern int *rule_cand; rule_cand[r - 1] += 1000000; }
#endif
        H[nh].key = (uint64_t)ru->layer << 52 | (uint64_t)s->spec << 22 | (uint32_t)(ru->order & 0x3FFFFF);
        H[nh++].rule = r - 1;
    }
    return nh;
}

static int hit_cmp(const void *a, const void *b) {
    uint64_t x = ((const struct hit *)a)->key, y = ((const struct hit *)b)->key;
    return x < y ? -1 : x > y;
}
static void sort_hits(int nh) {
    if (nh > 24) { qsort(H, nh, sizeof *H, hit_cmp); return; }
    for (int i = 1; i < nh; i++) {
        struct hit t = H[i];
        int j = i;
        while (j && H[j - 1].key > t.key) { H[j] = H[j - 1]; j--; }
        H[j] = t;
    }
}

static int flipped;                                     /* hits are in !important layer order (layers reverse) */
static void order_hits(int nh, int imp) {
    if (!any_layers || imp == flipped) return;
    for (int h = 0; h < nh; h++) H[h].key = (H[h].key & ((1ull << 52) - 1)) | (uint64_t)(255 - (H[h].key >> 52)) << 52;
    sort_hits(nh);
    flipped = imp;
}

static void apply(struct cprops *o, uint32_t *m, int prop, int kind, int v) {
    o->k[prop] = kind; o->v[prop] = v;
    m[prop >> 5] |= 1u << (prop & 31);
}

/* run the matching rules and the inline style of `node` into sp (specified values; m = which are set) */
static void cascade(int node, int pe, const uint32_t *anc, int legacy, const char *style, int pvars, struct cprops *sp, uint32_t *m, int *vars_out) {
    const char *tag = DOM->tag(node), *id = DOM->attr(node, "id"), *cls = DOM->attr(node, "class");
    int nh = 0;
    static uint32_t sb[BLW];
    memset(sb, 0, sizeof sb);
    bloom_add(sb, bloom_key(1, tag));
    if (id && *id) bloom_add(sb, bloom_key(2, id));
    if (cls) for (const char *p = cls; *p; ) {
        while (isspace((unsigned char)*p)) p++;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e > p) bloom_add(sb, bloom_keyn(3, p, e - p));
        p = e;
    }
    self_bloom = sb;
    cur_nscope = DOM->scope ? DOM->scope(node) : 0;
    nh = try_chain(huni, 0, node, pe, anc, legacy, nh);
    uint32_t th = hash_str(tag);
    nh = try_chain(htg[th & (HB - 1)], th, node, pe, anc, legacy, nh);
    if (id && *id) nh = try_chain(hid[hash_str(id) & (HB - 1)], hash_str(id), node, pe, anc, legacy, nh);
    if (cls) for (const char *p = cls; *p; ) {
        while (isspace((unsigned char)*p)) p++;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e > p) { uint32_t ch = hash_n(p, e - p); nh = try_chain(hcl[ch & (HB - 1)], ch, node, pe, anc, legacy, nh); }
        p = e;
    }
    sort_hits(nh);
    last_nh = nh;
    int keep = nd, i0 = 0, i1 = 0;
    if (style && *style && !pe) { i0 = nd; imp_cur = 0; parse_decls(style, strlen(style)); i1 = nd; }
    /* custom properties first: later ones sit in front, so a lookup finds the winner */
    int head = pvars, first_new = ncv;
    flipped = 0;
    for (int imp = 0; imp < 2; imp++) {
        order_hits(nh, imp);
        for (int h = 0; h < nh; h++) {
            const struct rule *ru = &R[H[h].rule];
            for (int j = 0; j < ru->nd; j++) {
                const struct decl *d = &D[ru->d0 + j];
                if (d->prop == P_CUSTOM && d->imp == imp) { int c = cv_new(d->a, d->v, head); if (c) head = c; }
            }
        }
        for (int j = i0; j < i1; j++) {
            const struct decl *d = &D[j];
            if (d->prop == P_CUSTOM && d->imp == imp) { int c = cv_new(d->a, d->v, head); if (c) head = c; }
        }
    }
    for (int c = first_new; c < ncv; c++) CV[c].head = head;
    *vars_out = head;
    for (int imp = 0; imp < 2; imp++) {                 /* !important last; the inline style after the sheets */
        order_hits(nh, imp);
        for (int h = 0; h <= nh; h++) {
            int d0, dn;
            if (h < nh) { const struct rule *ru = &R[H[h].rule]; d0 = ru->d0; dn = ru->nd; } else { d0 = i0; dn = i1 - i0; }
            for (int j = 0; j < dn; j++) {
                struct decl d = D[d0 + j];
                if (d.imp != imp || d.prop == P_CUSTOM) continue;
                if (d.prop == P_RAWDECL) {
                    char out[1024], nm[80];
                    strlcpy(nm, STR(d.a), sizeof nm);
                    if (!subst(STR(d.v), head, out, sizeof out, 0)) continue;       /* invalid at computed-value time: as if not declared */
                    trim(out);
                    int sv = nd;
                    decl(nm, out, imp);
                    for (int q = sv; q < nd; q++) { struct decl e = D[q]; if (e.prop < P_N) apply(sp, m, e.prop, e.kind, e.v); }
                    nd = sv;
                } else if (d.prop < P_N) apply(sp, m, d.prop, d.kind, d.v);
            }
        }
    }
    nd = keep;
}

/* ---------- initial and inherited values ---------- */
static struct cprops INI; static int ini_ready;

static uint8_t INHT[P_N];
static int inherited_slow(int p) {
    switch (p) {
    case P_COLOR: case P_FSIZE: case P_FWEIGHT: case P_FAMILY: case P_ITALIC: case P_BOLD: case P_MONO: case P_LH: case P_LS: case P_WSP: case P_TIND:
    case P_ALIGN: case P_TT: case P_WS: case P_WBRK: case P_LISTT: case P_LISTP: case P_LIST: case P_VISIB: case P_CURSOR: case P_DIR:
    case P_COLLAPSE: case P_SPACING: case P_SPACINGY: case P_PEV:
        return 1;
    }
    return 0;
}

static inline int inherited(int p) { return INHT[p]; }

void css_initial(struct cprops *o) {
    memset(o, 0, sizeof *o);
    o->set = 0xFFFFFFFFu;
    o->v[P_COLOR] = (int)0xFF000000u; o->v[P_FSIZE] = 16; o->v[P_FWEIGHT] = 400; o->v[P_DISPLAY] = D_INLINE; o->v[P_OPACITY] = 255;
    o->k[P_WIDTH] = o->k[P_HEIGHT] = o->k[P_MINW] = o->k[P_MINH] = o->k[P_MAXW] = o->k[P_MAXH] = K_AUTO;
    o->k[P_TOP] = o->k[P_RIGHT] = o->k[P_BOTTOM] = o->k[P_LEFT] = K_AUTO;
    o->k[P_ZINDEX] = K_AUTO; o->k[P_LH] = K_AUTO; o->k[P_FBASIS] = K_AUTO; o->k[P_ALIGNS] = o->k[P_JUSTS] = K_AUTO;
    o->k[P_FGROW] = o->k[P_FSHRINK] = K_NUM; o->v[P_FSHRINK] = 100;
    o->k[P_BGIMG] = K_NONE; o->k[P_BGSW] = o->k[P_BGSH] = K_AUTO; o->k[P_BGPX] = o->k[P_BGPY] = K_PCT;
    o->k[P_CONTENT] = K_NONE; o->k[P_GCOLS] = o->k[P_GROWS] = o->k[P_GACOLS] = o->k[P_GAROWS] = o->k[P_GAREAS] = o->k[P_GAREA] = K_NONE;
    o->k[P_GCS] = o->k[P_GCE] = o->k[P_GRS] = o->k[P_GRE] = K_AUTO; o->k[P_ASPECT] = K_AUTO;
    o->k[P_BCT] = o->k[P_BCR] = o->k[P_BCB] = o->k[P_BCL] = K_CUR; o->k[P_OUTC] = K_CUR; o->v[P_OUTW] = 3;
    o->k[P_SHC] = K_NONE; o->k[P_MASKIMG] = K_NONE; o->k[P_BG2IMG] = K_NONE; o->k[P_BG2SW] = o->k[P_BG2SH] = K_AUTO; o->k[P_BG2PX] = o->k[P_BG2PY] = K_PCT; o->v[P_TRSX] = o->v[P_TRSY] = 1000; o->k[P_CRESET] = o->k[P_CINC] = K_NONE; o->k[P_OBJPX] = o->k[P_OBJPY] = K_PCT; o->v[P_OBJPX] = o->v[P_OBJPY] = 5000;
    o->v[P_BWT] = o->v[P_BWR] = o->v[P_BWB] = o->v[P_BWL] = 3;
}


/* what a length kind that needs the font size becomes; returns 0 if the value is invalid */
static int resolve_kind(int *k, int *v, double fs, double rfs, int pct_base_is_fs) {
    switch (*k) {
    case K_EM: *v = rnd(*v * fs / 100); *k = K_PX; return 1;
    case K_REM: *v = rnd(*v * rfs / 100); *k = K_PX; return 1;
    case K_CALCS: {
        int kk, vv;
        if (!calc_compile(STR(*v), fs, rfs, &kk, &vv)) return 0;
        *k = kk; *v = vv;
        return 1;
    }
    }
    (void)pct_base_is_fs;
    return 1;
}

static double root_fs = 16;


/* ---------- counters ---------- */
static int snprintf_int(char *t, int n, int pad2) {          /* decimal (zero-padded to 2 digits when pad2) */
    char r[16];
    int k = 0, q = 0, neg = n < 0;
    unsigned u = neg ? -(unsigned)n : (unsigned)n;
    do { r[q++] = '0' + u % 10; u /= 10; } while (u && q < 15);
    if (pad2 && q < 2) r[q++] = '0';
    if (neg) t[k++] = '-';
    while (q) t[k++] = r[--q];
    t[k] = 0;
    return k;
}
#define MAXCNT 96
static struct cnt { int name, val, scope; } CNT[MAXCNT];
static int ncnt;
void css_cnt_reset_all(void) { ncnt = 0; }
static struct cnt *cnt_find(int name) { for (int i = ncnt - 1; i >= 0; i--) if (CNT[i].name == name) return &CNT[i]; return 0; }
static void cnt_list(const char *s, int inc, int depth) {      /* "name [int] name [int]..." */
    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        const char *b = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        if (s == b) break;
        int name = intern(b, s - b), val = inc ? 1 : 0;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '-' || isdigit((unsigned char)*s)) { char *e; val = (int)strtol(s, &e, 10); s = e; }
        struct cnt *c = cnt_find(name);
        if (inc) {
            if (!c && ncnt < MAXCNT) { CNT[ncnt] = (struct cnt){ name, 0, depth - 1 }; c = &CNT[ncnt++]; }
            if (c) c->val += val;
        } else if (c && c->scope == depth - 1) c->val = val;
        else if (ncnt < MAXCNT) CNT[ncnt++] = (struct cnt){ name, val, depth - 1 };
    }
}
void css_cnt_enter(const struct cprops *p, int depth) {
    if (p->k[P_CRESET] == K_STR) cnt_list(STR(p->v[P_CRESET]), 0, depth);
    if (p->k[P_CINC] == K_STR) cnt_list(STR(p->v[P_CINC]), 1, depth);
}
void css_cnt_leave(int depth) { while (ncnt && CNT[ncnt - 1].scope >= depth) ncnt--; }

int css_counter_format(int lt, int n, char *o, int cap) {
    char t[48];
    int k = 0;
    static const char *const grk = "\xCE\xB1\xCE\xB2\xCE\xB3\xCE\xB4\xCE\xB5\xCE\xB6\xCE\xB7\xCE\xB8\xCE\xB9\xCE\xBA\xCE\xBB\xCE\xBC\xCE\xBD\xCE\xBE\xCE\xBF\xCF\x80\xCF\x81\xCF\x83\xCF\x84\xCF\x85\xCF\x86\xCF\x87\xCF\x88\xCF\x89";
    switch (lt) {
    case LT_DISC: strcpy(t, "\xE2\x80\xA2"); k = 3; break;
    case LT_CIRCLE: strcpy(t, "\xE2\x97\xA6"); k = 3; break;
    case LT_SQUARE: strcpy(t, "\xE2\x96\xAA"); k = 3; break;
    case LT_NONE: t[0] = 0; break;
    case LT_ZERO: k = snprintf_int(t, n, n >= 0 && n < 10 ? 2 : 0); break;
    case LT_LALPHA: case LT_UALPHA:
        if (n < 1) { k = snprintf_int(t, n, 0); break; }
        { char r[16]; int q = 0; for (int m = n; m > 0 && q < 15; m = (m - 1) / 26) r[q++] = (lt == LT_LALPHA ? 'a' : 'A') + (m - 1) % 26; while (q) t[k++] = r[--q]; t[k] = 0; }
        break;
    case LT_LROMAN: case LT_UROMAN:
        if (n < 1 || n > 3999) { k = snprintf_int(t, n, 0); break; }
        {
            static const int V[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
            static const char *const S[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
            for (int i = 0; i < 13; i++) while (n >= V[i]) { for (const char *c = S[i]; *c; c++) t[k++] = lt == LT_UROMAN ? *c - 32 : *c; n -= V[i]; }
            t[k] = 0;
        }
        break;
    case LT_LGREEK:
        if (n < 1 || n > 24) { k = snprintf_int(t, n, 0); break; }
        t[0] = grk[(n - 1) * 2]; t[1] = grk[(n - 1) * 2 + 1]; t[2] = 0; k = 2;
        break;
    default: k = snprintf_int(t, n, 0); break;
    }
    if (k >= cap) k = cap - 1;
    if (k < 0) k = 0;
    memcpy(o, t, k); o[k] = 0;
    return k;
}

static int content_text(const char *s, int node, char *o, int cap) {
    int k = 0;
    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (*s && *s != q) {
                if (*s == '\\' && s[1]) {
                    s++;
                    if (isxdigit((unsigned char)*s)) {
                        int cp = 0, d = 0;
                        while (d < 6 && isxdigit((unsigned char)*s)) { cp = cp * 16 + (isdigit((unsigned char)*s) ? *s - '0' : (*s | 32) - 'a' + 10); s++; d++; }
                        if (*s == ' ') s++;
                        char u[4];
                        int n = cp < 0x80 ? (u[0] = cp, 1) : cp < 0x800 ? (u[0] = 0xC0 | cp >> 6, u[1] = 0x80 | (cp & 63), 2) :
                                 cp < 0x10000 ? (u[0] = 0xE0 | cp >> 12, u[1] = 0x80 | (cp >> 6 & 63), u[2] = 0x80 | (cp & 63), 3) :
                                 (u[0] = 0xF0 | cp >> 18, u[1] = 0x80 | (cp >> 12 & 63), u[2] = 0x80 | (cp >> 6 & 63), u[3] = 0x80 | (cp & 63), 4);
                        if (k + n < cap) { memcpy(o + k, u, n); k += n; }
                        continue;
                    }
                    if (*s == '\n') { s++; continue; }
                }
                if (k < cap - 1) o[k++] = *s;
                s++;
            }
            if (*s) s++;
        } else if (!strncasecmp(s, "attr(", 5)) {
            const char *a = s + 5;
            while (isspace((unsigned char)*a)) a++;
            char nm[64];
            int n = 0;
            while (*a && *a != ')' && !isspace((unsigned char)*a) && n < 63) nm[n++] = tolower((unsigned char)*a++);
            nm[n] = 0;
            const char *v = DOM->attr(node, nm);
            if (v) for (; *v && k < cap - 1; v++) o[k++] = *v;
            while (*s && *s != ')') s++;
            if (*s) s++;
        } else if (!strncasecmp(s, "counter(", 8) || !strncasecmp(s, "counters(", 9)) {
            int many = s[7] == 's';
            const char *a = s + (many ? 9 : 8);
            char nm[64], sep[24] = "", sty[24] = "decimal";
            int n = 0, si = 0, st = 0;
            while (isspace((unsigned char)*a)) a++;
            while (*a && *a != ',' && *a != ')' && !isspace((unsigned char)*a) && n < 63) nm[n++] = *a++;
            nm[n] = 0;
            while (isspace((unsigned char)*a)) a++;
            if (many && *a == ',') {
                a++;
                while (isspace((unsigned char)*a)) a++;
                char q = *a == '"' || *a == '\'' ? *a++ : 0;
                while (*a && (q ? *a != q : *a != ',' && *a != ')') && si < 23) sep[si++] = *a++;
                if (q && *a) a++;
                sep[si] = 0;
                while (isspace((unsigned char)*a)) a++;
            }
            if (*a == ',') { a++; while (isspace((unsigned char)*a)) a++; while (*a && *a != ')' && st < 23) sty[st++] = *a++; sty[st] = 0; while (st && isspace((unsigned char)sty[st - 1])) sty[--st] = 0; }
            const struct pdef *lt = find_pd("list-style-type");
            int style = kwi(lt->kw, sty);
            if (style < 0) style = LT_DECIMAL;
            int id = intern(nm, strlen(nm));
            char tmp[32];
            int first = 1;
            for (int i = 0; i < ncnt; i++) {
                if (CNT[i].name != id) continue;
                if (!many) { /* innermost only: handled below */ continue; }
                if (!first) for (const char *c = sep; *c && k < cap - 1; c++) o[k++] = *c;
                first = 0;
                css_counter_format(style, CNT[i].val, tmp, sizeof tmp);
                for (const char *c = tmp; *c && k < cap - 1; c++) o[k++] = *c;
            }
            if (!many) {
                struct cnt *c = cnt_find(id);
                css_counter_format(style, c ? c->val : 0, tmp, sizeof tmp);
                for (const char *q = tmp; *q && k < cap - 1; q++) o[k++] = *q;
            }
            while (*s && *s != ')') s++;
            if (*s) s++;
        } else if (!strncasecmp(s, "open-quote", 10)) { if (k < cap - 1) o[k++] = '"'; s += 10; }
        else if (!strncasecmp(s, "close-quote", 11)) { if (k < cap - 1) o[k++] = '"'; s += 11; }
        else {                                            /* counter(), url(), no-open-quote...: nothing */
            int d = 0;
            while (*s && (d || !isspace((unsigned char)*s))) { if (*s == '(') d++; else if (*s == ')' && d) d--; s++; }
        }
    }
    o[k] = 0;
    return k;
}

int css_compute(int node, int pe, const struct cprops *parent, struct cprops *out) {
    if (!ini_ready) { css_initial(&INI); for (int p = 0; p < P_N; p++) INHT[p] = inherited_slow(p); ini_ready = 1; }
    struct cprops sp;
    uint32_t m[8] = { 0 };            /* one bit per property (P_N <= 256) */
    memset(&sp, 0, sizeof sp);
    static const uint32_t none[BLW];
    const uint32_t *anc = pe ? 0 : parent ? parent->bloom : DOM->parent(node) ? 0 : none;
    int vars;
    cascade(node, pe, anc, 0, pe ? 0 : DOM->attr(node, "style"), parent ? parent->vars : 0, &sp, m, &vars);
    const struct cprops *par = parent ? parent : &INI;
    #define ISSET(p) (m[(p) >> 5] >> ((p) & 31) & 1)
    double pfs = par->v[P_FSIZE], fs = pfs;
    if (ISSET(P_FSIZE)) {
        int k = sp.k[P_FSIZE], v = sp.v[P_FSIZE];
        if (k == K_INITIAL) fs = 16;
        else if (k == K_INHERIT || k == K_UNSET) fs = pfs;
        else {
            if (!resolve_kind(&k, &v, pfs, root_fs, 1)) { k = K_INHERIT; v = 0; }
            if (k == K_PX) fs = v; else if (k == K_PCT) fs = pfs * v / 10000; else if (k == K_CALC) fs = cn_eval(v, pfs); else fs = pfs;
        }
    }
    if (fs < 0) fs = 0;
    memset(out, 0, sizeof *out);
    out->k[P_FSIZE] = K_PX; out->v[P_FSIZE] = rnd(fs);
    if (!parent && !pe) root_fs = out->v[P_FSIZE];
    /* colour first: currentcolor needs it */
    {
        int k = ISSET(P_COLOR) ? sp.k[P_COLOR] : K_UNSET;
        out->k[P_COLOR] = K_PX;
        out->v[P_COLOR] = k == K_PX ? sp.v[P_COLOR] : k == K_INITIAL ? INI.v[P_COLOR] : par->v[P_COLOR];
    }
    for (int p = 0; p < P_N; p++) {
        if (p == P_FSIZE || p == P_COLOR) continue;
        int k = ISSET(p) ? sp.k[p] : K_UNSET, v = sp.v[p];
        if (k == K_UNSET) k = inherited(p) ? K_INHERIT : K_INITIAL;
        if (k == K_INHERIT) { out->k[p] = par->k[p]; out->v[p] = par->v[p]; }
        else if (k == K_INITIAL) { out->k[p] = INI.k[p]; out->v[p] = INI.v[p]; }
        else {
            if (!resolve_kind(&k, &v, fs, root_fs, 0)) { out->k[p] = inherited(p) ? par->k[p] : INI.k[p]; out->v[p] = inherited(p) ? par->v[p] : INI.v[p]; continue; }
            out->k[p] = k; out->v[p] = v;
        }
        if (out->k[p] == K_CUR) { out->k[p] = K_PX; out->v[p] = out->v[P_COLOR]; }
    }
    #undef ISSET
    /* relative values that need the parent / own size */
    if (out->k[P_FWEIGHT] == K_NUM) {
        int pw = par->v[P_FWEIGHT], b = out->v[P_FWEIGHT] == 1;
        out->v[P_FWEIGHT] = b ? (pw < 400 ? 400 : pw < 600 ? 700 : 900) : (pw < 600 ? 100 : pw < 800 ? 400 : 700);
        out->k[P_FWEIGHT] = K_PX;
    }
    if (out->k[P_LH] == K_PCT) { out->v[P_LH] = rnd(fs * out->v[P_LH] / 10000); out->k[P_LH] = K_PX; }
    else if (out->k[P_LH] == K_CALC) { out->v[P_LH] = rnd(cn_eval(out->v[P_LH], fs)); out->k[P_LH] = K_PX; }
    for (int s = 0; s < 4; s++) {
        static const uint8_t w[4] = { P_BWT, P_BWR, P_BWB, P_BWL }, st[4] = { P_BSTT, P_BSTR, P_BSTB, P_BSTL };
        if (out->v[st[s]] == BS_NONE) out->v[w[s]] = 0;
    }
    if (out->v[P_OUTS] == BS_NONE) out->v[P_OUTW] = 0;
    int d = out->v[P_DISPLAY];                            /* floated and positioned boxes are blockified */
    if (out->v[P_FLOAT] || out->v[P_POSITION] == POS_ABSOLUTE || out->v[P_POSITION] == POS_FIXED) {
        out->v[P_DISPLAY] = d == D_INLINE || d == D_INLINE_BLOCK || (d >= D_TABLE_ROW && d <= D_TABLE_CAPTION && d != D_TABLE_COL) ? D_BLOCK :
                            d == D_INLINE_FLEX ? D_FLEX : d == D_INLINE_GRID ? D_GRID : d == D_INLINE_TABLE ? D_TABLE : d;
        if (out->v[P_DISPLAY] == D_TABLE_COL) out->v[P_DISPLAY] = D_BLOCK;
    }
    out->v[P_BOLD] = out->v[P_FWEIGHT] >= 600; out->k[P_BOLD] = K_PX;
    out->v[P_MONO] = out->v[P_FAMILY] == FAM_MONO; out->k[P_MONO] = K_PX;
    out->v[P_LIST] = out->v[P_LISTT] == LT_NONE; out->k[P_LIST] = K_PX;
    out->set = 0xFFFFFFFFu;
    out->vars = vars;
    if (pe) memcpy(out->bloom, par->bloom, sizeof out->bloom);
    else {
        if (parent) memcpy(out->bloom, parent->bloom, sizeof out->bloom);
        bloom_add(out->bloom, bloom_key(1, DOM->tag(node)));
        const char *id = DOM->attr(node, "id"), *cls = DOM->attr(node, "class");
        if (id && *id) bloom_add(out->bloom, bloom_key(2, id));
        if (cls) for (const char *p = cls; *p; ) {
            while (isspace((unsigned char)*p)) p++;
            const char *e = p;
            while (*e && !isspace((unsigned char)*e)) e++;
            if (e > p) bloom_add(out->bloom, bloom_keyn(3, p, e - p));
            p = e;
        }
    }
    if (pe >= PE_MARKER) {
        if (out->k[P_CONTENT] == K_RAW) { char t[512]; content_text(STR(out->v[P_CONTENT]), node, t, sizeof t); out->k[P_CONTENT] = K_STR; out->v[P_CONTENT] = intern(t, strlen(t)); }
        return last_nh > 0;
    }
    if (pe) {
        if (out->k[P_CONTENT] == K_RAW) {
            char t[512];
            content_text(STR(out->v[P_CONTENT]), node, t, sizeof t);
            out->k[P_CONTENT] = K_STR; out->v[P_CONTENT] = intern(t, strlen(t));
        }
        return out->k[P_CONTENT] == K_STR && out->v[P_DISPLAY] != D_NONE;
    }
    return 1;
}

/* ---------- restyle scoping ---------- */
static int key_in(const uint32_t *b, uint32_t key) { return bloom_hasw(b, CDW, key); }
static int doc_top(int n) { while (DOM->parent(n)) n = DOM->parent(n); return n; }

/* flags: which dependency tables mention what changed (1 subject, 2 descendant context, 4 sibling context, 8 :has) -> where to restyle from */
static int scope_of(int node, int fl, int *deep) {
    *deep = 0;
    if (!fl) return 0;
    if (fl & 8) { *deep = 1; return doc_top(node); }
    if (fl & 4) { int p = DOM->parent(node); *deep = 1; return p ? p : node; }
    if (fl & 2) { *deep = 1; return node; }
    return node;
}
static int bloom_flags(uint32_t key) {
    return (key_in(CD.subj, key) ? 1 : 0) | (key_in(CD.desc, key) ? 2 : 0) | (key_in(CD.sib, key) ? 4 : 0) | (key_in(CD.has, key) ? 8 : 0);
}

int css_restyle_for_state(int node, int changed, int *deep) {
    int fl = ((changed & CD.st_subj) ? 1 : 0) | ((changed & CD.st_desc) ? 2 : 0) | ((changed & CD.st_sib) ? 4 : 0) | ((changed & CD.st_has) ? 8 : 0);
    return scope_of(node, fl, deep);
}

/* old/new: the class attribute before/after (either may be NULL); only the classes that changed matter */
int css_restyle_for_class(int node, const char *old, const char *nw, int *deep) {
    int fl = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *a = pass ? nw : old, *b = pass ? old : nw;
        for (const char *p = a ? a : ""; *p; ) {
            while (isspace((unsigned char)*p)) p++;
            const char *e = p;
            while (*e && !isspace((unsigned char)*e)) e++;
            if (e > p) {
                int in_b = 0;
                for (const char *q = b ? b : ""; *q && !in_b; ) {
                    while (isspace((unsigned char)*q)) q++;
                    const char *f = q;
                    while (*f && !isspace((unsigned char)*f)) f++;
                    if (f - q == e - p && !strncmp(p, q, e - p)) in_b = 1;
                    q = f;
                }
                if (!in_b) fl |= bloom_flags(bloom_keyn(3, p, e - p));
            }
            p = e;
        }
    }
    return scope_of(node, fl, deep);
}

int css_restyle_for_id(int node, const char *old, const char *nw, int *deep) {
    int fl = 0;
    if (old && *old) fl |= bloom_flags(bloom_key(2, old));
    if (nw && *nw) fl |= bloom_flags(bloom_key(2, nw));
    return scope_of(node, fl, deep);
}

/* some other attribute changed (not class/id): `name` lowercase. "style" only restyles the node itself. */
int css_restyle_for_attr(int node, const char *name, int *deep) {
    *deep = 0;
    if (!strcmp(name, "style")) return node;
    if (!strcmp(name, "class") || !strcmp(name, "id")) { int fl = 15; return scope_of(node, fl, deep); }
    int id = intern(name, strlen(name)), fl = 0;
    if (CD.attr_over) fl = 7;
    for (int i = 0; i < CD.nattr; i++) if (CD.attr[i] == id) fl |= CD.af[i];
    return scope_of(node, fl, deep);
}

/* an element was inserted/removed/moved under `parent`: restyle from the returned node (the parent when sibling-position selectors exist) */
int css_restyle_for_insert(int node, int *deep) {
    *deep = 1;
    if (CD.st_sib & 0x30000) { int p = DOM->parent(node); return p ? p : node; }
    return node;
}

/* do two computed styles hand children the same inherited input? If so (and `deep` was 0) the descendants need no restyle */
int css_inherit_same(const struct cprops *a, const struct cprops *b) {
    if (a->vars != b->vars) return 0;
    for (int p = 0; p < P_N; p++) if (INHT[p] && (a->v[p] != b->v[p] || a->k[p] != b->k[p])) return 0;
    return 1;
}

/* ---------- legacy entry points (streaming layout) ---------- */
static color_t legacy_col(int v) { uint32_t c = (uint32_t)v; return (c >> 24) >= 128 ? c | 0xFF000000u : 0; }

void css_cascade(const struct el *st, int n, const char *style, struct cprops *o) {
    memset(o, 0, sizeof *o);
    if (!n) return;
    const struct css_dom *saved = DOM;
    DOM = &LDOM; LS = st; LN = n;
    struct cprops sp;
    uint32_t m[8] = { 0 };            /* one bit per property (P_N <= 256) */
    int vars;
    memset(&sp, 0, sizeof sp);
    cascade(n, 0, 0, 1, style, 0, &sp, m, &vars);
    DOM = saved;
    #define ISSET(p) (m[(p) >> 5] >> ((p) & 31) & 1)
    #define SET(p, kk, vv) do { o->set |= 1u << (p); o->k[p] = (kk); o->v[p] = (vv); } while (0)
    for (int p = 0; p < 32; p++) {
        if (!ISSET(p) || p == P_BOLD || p == P_MONO || p == P_LIST || (p >= P_BWT && p <= P_BWL)) continue;
        int k = sp.k[p], v = sp.v[p];
        if (k == K_INHERIT || k == K_INITIAL || k == K_UNSET || k == K_CUR || k == K_RAW || k == K_STR || k == K_NONE || k == K_NUM || k == K_GRAD) continue;
        if (!resolve_kind(&k, &v, 16, 16, 0)) continue;
        if (k == K_CALC) { k = K_PX; v = rnd(cn_eval(v, vpw)); }
        if (k == K_FIT || k == K_MINC || k == K_MAXC) { k = K_AUTO; v = 0; }
        if (k == K_PCT) v = rnd(v / 100.);
        if (p == P_COLOR || p == P_BG || (p >= P_BCT && p <= P_BCL)) { v = (int)legacy_col(v); if (!v && p == P_COLOR) continue; }
        if (p == P_DISPLAY) v = v == D_NONE ? 1 : v == D_INLINE || v == D_INLINE_BLOCK || v == D_INLINE_FLEX || v == D_INLINE_GRID || v == D_INLINE_TABLE ? 3 : 2;
        if (p == P_ALIGN && v == TA_JUSTIFY) v = 0;
        if (p == P_VALIGN) v = v == VA_MIDDLE ? 1 : v == VA_BOTTOM || v == VA_TEXT_BOTTOM ? 2 : 0;
        if (p == P_FSIZE && k == K_PX && v < 0) continue;
        SET(p, k, v);
    }
    for (int s = 0; s < 4; s++) {                          /* a border needs a style: none (or no style at all) means no width */
        int w = ISSET(SIDE_BW[s]) ? sp.v[SIDE_BW[s]] : 3, wk = ISSET(SIDE_BW[s]) ? sp.k[SIDE_BW[s]] : K_PX;
        if (wk == K_EM || wk == K_REM || wk == K_CALCS) { int kk = wk, vv = w; if (resolve_kind(&kk, &vv, 16, 16, 0)) w = vv; }
        if (ISSET(SIDE_BS[s])) SET(P_BWT + s, K_PX, sp.v[SIDE_BS[s]] == BS_NONE ? 0 : w);
        if (ISSET(P_BCT + s) && sp.k[P_BCT + s] == K_PX) SET(P_BCT + s, K_PX, (int)legacy_col(sp.v[P_BCT + s]));
    }
    if (ISSET(P_FWEIGHT) && (sp.k[P_FWEIGHT] == K_PX || sp.k[P_FWEIGHT] == K_NUM)) SET(P_BOLD, K_PX, sp.k[P_FWEIGHT] == K_NUM ? sp.v[P_FWEIGHT] == 1 : sp.v[P_FWEIGHT] >= 600);
    if (ISSET(P_FAMILY) && sp.k[P_FAMILY] == K_PX) SET(P_MONO, K_PX, sp.v[P_FAMILY] == FAM_MONO);
    if (ISSET(P_LISTT) && sp.k[P_LISTT] == K_PX) SET(P_LIST, K_PX, sp.v[P_LISTT] == LT_NONE);
    #undef SET
    #undef ISSET
}

/* the comma list `sel` against `node` through the current DOM; -1 = nothing parsable */
static int match_list_str(const char *sel, int node) {
    int len = strlen(sel), ok = 0, a0, b0, c0;
    sel_mark(&a0, &b0, &c0);
    for (int a = 0; a <= len; ) {
        int b = a, dp = 0;
        char q = 0;
        while (b < len && (q || dp || sel[b] != ',')) {
            if (q) { if (sel[b] == '\\') b++; else if (sel[b] == q) q = 0; }
            else if (sel[b] == '"' || sel[b] == '\'') q = sel[b];
            else if (sel[b] == '\\') b++;
            else if (sel[b] == '(' || sel[b] == '[') dp++;
            else if ((sel[b] == ')' || sel[b] == ']') && dp) dp--;
            b++;
        }
        struct sel s;
        if (sel_parse(sel + a, b - a, &s) && !s.pe) { ok = 1; if (node && sel_match(&s, node)) { sel_trunc(a0, b0, c0); return 1; } }
        a = b + 1;
    }
    sel_trunc(a0, b0, c0);
    return ok ? 0 : -1;
}

int css_match(const char *sel, const struct el *st, int n) {
    const struct css_dom *saved = DOM;
    DOM = &LDOM; LS = st; LN = n;
    int r = match_list_str(sel, n);
    DOM = saved;
    return r;
}

int css_match_node(const char *sel, int node) { return match_list_str(sel, node); }

void css_stats(void) {          /* bench aid (tools/csstest/wpbench): rule bucket distribution */
    int u = 0, t = 0, mx = 0;
    for (int r = huni; r; r = R[r - 1].next) u++;
    for (int i = 0; i < HB; i++) { int c = 0; for (int r = htg[i]; r; r = R[r - 1].next) c++; t += c; if (c > mx) mx = c; }
    fprintf(stderr, "rules %d universal %d tag-bucketed %d (longest chain %d)\n", nr, u, t, mx);
    if (getenv("UNI")) for (int r = huni, n = 0; r && n < 40; r = R[r - 1].next, n++) {
        const struct sel *q = &SE[R[r - 1].sel];
        fprintf(stderr, "  uni: nc=%d kind=%d pe=%d:", q->nc, q->kind, q->pe);
        for (int c = 0; c < q->nc; c++) { fprintf(stderr, " ["); for (int k = 0; k < CO[q->c0 + c].ns; k++) { const struct simple *x = &SI[CO[q->c0 + c].s0 + k]; fprintf(stderr, "%d:%s ", x->t, x->t <= T_ATTR ? STR(x->a) : ""); } fprintf(stderr, "]%c", CO[q->c0 + c].comb ? CO[q->c0 + c].comb : ' '); }
        fprintf(stderr, "\n");
    }
}

#ifdef CSS_STATS
int *rule_cand;
void css_stats_alloc(void) { rule_cand = calloc(nr + 1, sizeof(int)); }
void css_stats_top(void) {
    for (int k = 0; k < 12; k++) {
        int best = -1, bi = 0;
        for (int i = 0; i < nr; i++) { int c = rule_cand[i] % 1000000, ok = rule_cand[i] / 1000000, fail = c - ok; if (fail > best) { best = fail; bi = i; } }
        const struct sel *q = &SE[R[bi].sel];
        fprintf(stderr, "failed %d (matched %d): nc=%d kind=%d pe=%d:", best, rule_cand[bi] / 1000000, q->nc, q->kind, q->pe);
        for (int c = 0; c < q->nc; c++) { fprintf(stderr, " ["); for (int j = 0; j < CO[q->c0 + c].ns; j++) { const struct simple *x = &SI[CO[q->c0 + c].s0 + j]; fprintf(stderr, "%d:%s ", x->t, x->t <= T_ATTR ? STR(x->a) : ""); } fprintf(stderr, "]%c", CO[q->c0 + c].comb ? CO[q->c0 + c].comb : ' '); }
        fprintf(stderr, "\n");
        rule_cand[bi] = 0;
    }
}
#endif

/* the resolved value of the custom property `name` (with its "--") in a computed style, or NULL when it isn't set (getPropertyValue('--x')) */
const char *css_custom(const struct cprops *p, const char *name) {
    int i = cv_lookup(p->vars, intern(name, strlen(name)));
    if (!i) return 0;
    int v = cv_value(i, 0);
    return v < 0 ? 0 : STR(v);
}
