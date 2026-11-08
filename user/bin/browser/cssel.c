/* cssel.c - selectors: parser (type, #id, .class, [attr op value i], pseudo-classes incl. :not/:is/:where/:has/:nth-*,
 * ::before/::after, all four combinators) and matcher. The matcher walks the document through struct css_dom, so
 * the same code serves the DOM tree (LAYOUT, JS) and the legacy open-element stack. Rules the parser can't make
 * sense of are rejected as a whole, like a browser does with an invalid selector. */
#include "cssi.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct simple *SI; struct comp *CO; struct sel *SE;
int nsi, nco, nse;
static int csi, cco, cse;
const struct css_dom *DOM;
int attrs_used;

uint32_t hash_str(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
uint32_t bloom_key(int kind, const char *s) { return hash_str(s) * 2654435761u + (uint32_t)kind * 0x9E3779B1u; }
void bloom_add(uint32_t b[2], uint32_t h) {
    unsigned x = (h >> 7) & 63, y = (h >> 19) & 63;
    b[x >> 5] |= 1u << (x & 31); b[y >> 5] |= 1u << (y & 31);
}
int bloom_has(const uint32_t b[2], const uint32_t m[2]) { return !((m[0] & ~b[0]) | (m[1] & ~b[1])); }

#define GROW(arr, cnt, cap, want) do { if ((cnt) + (want) > (cap)) { int nc_ = (cap) ? (cap) * 2 : 256; while (nc_ < (cnt) + (want)) nc_ *= 2; \
    void *p_ = realloc(arr, nc_ * sizeof *(arr)); if (!p_) return 0; arr = p_; cap = nc_; } } while (0)

/* ---------- parsing ---------- */
static int ident_char(unsigned char c) { return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\'; }

static int utf8(int cp, char *o) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63); return 4;
}

/* identifier with CSS escapes (\: \31 \1F600 ) decoded; returns its length (0 = none) */
static int read_ident(const char *s, int n, int *pi, char *out, int cap) {
    int i = *pi, k = 0;
    while (i < n && ident_char(s[i])) {
        if (k + 5 >= cap) return 0;
        if (s[i] == '\\') {
            if (++i >= n) break;
            if (isxdigit((unsigned char)s[i])) {
                int cp = 0, d = 0;
                while (i < n && d < 6 && isxdigit((unsigned char)s[i])) { cp = cp * 16 + (isdigit((unsigned char)s[i]) ? s[i] - '0' : (s[i] | 32) - 'a' + 10); i++; d++; }
                if (i < n && isspace((unsigned char)s[i])) i++;
                k += utf8(cp ? cp : 0xFFFD, out + k);
            } else out[k++] = s[i++];
        } else out[k++] = s[i++];
    }
    out[k] = 0;
    *pi = i;
    return k;
}

static void lower(char *s) { for (; *s; s++) *s = tolower((unsigned char)*s); }

/* the text inside the parentheses starting at s[*pi] == '(' ; leaves *pi after ')' */
static int paren_body(const char *s, int n, int *pi, int *bs, int *be) {
    int i = *pi + 1, d = 1;
    char q = 0;
    *bs = i;
    for (; i < n; i++) {
        if (q) { if (s[i] == '\\') i++; else if (s[i] == q) q = 0; continue; }
        if (s[i] == '"' || s[i] == '\'') q = s[i];
        else if (s[i] == '(') d++;
        else if (s[i] == ')' && !--d) break;
    }
    if (d) return 0;
    *be = i;
    *pi = i + 1;
    return 1;
}

static int parse_complex(const char *s, int n, int rel, struct sel *out);

/* comma list inside :is()/:not()/:has(): appended to SE contiguously; returns count (0 = error). Forgiving for :is/:where would skip
 * bad entries; we skip them too (an all-bad list is an error). */
static int parse_list(const char *s, int n, int rel, int *first) {
    struct sel tmp[24];
    int cnt = 0, a = 0;
    while (a <= n) {
        int b = a, d = 0;
        char q = 0;
        while (b < n && (q || d || s[b] != ',')) {
            if (q) { if (s[b] == '\\') b++; else if (s[b] == q) q = 0; }
            else if (s[b] == '"' || s[b] == '\'') q = s[b];
            else if (s[b] == '(' || s[b] == '[') d++;
            else if ((s[b] == ')' || s[b] == ']') && d) d--;
            b++;
        }
        if (cnt < 24 && parse_complex(s + a, b - a, rel, &tmp[cnt])) cnt++;
        a = b + 1;
    }
    if (!cnt) return 0;
    if (nse + cnt > cse) {
        int nc = cse ? cse * 2 : 256;
        while (nc < nse + cnt) nc *= 2;
        void *p = realloc(SE, nc * sizeof *SE);
        if (!p) return 0;
        SE = p; cse = nc;
    }
    *first = nse;
    memcpy(SE + nse, tmp, cnt * sizeof *tmp);
    nse += cnt;
    return cnt;
}

struct spc { int a, b, c; };
static void spec_max(struct spc *d, uint32_t sp) {   /* the most specific of a list: compare packed */
    uint32_t cur = (uint32_t)d->a << 20 | (uint32_t)d->b << 10 | (uint32_t)d->c;
    if (sp > cur) { d->a = sp >> 20; d->b = sp >> 10 & 1023; d->c = sp & 1023; }
}

static int parse_nth(const char *s, int n, int *A, int *B) {     /* odd | even | An+B */
    char b[40];
    int k = 0;
    for (int i = 0; i < n && k < 39; i++) if (!isspace((unsigned char)s[i])) b[k++] = tolower((unsigned char)s[i]);
    b[k] = 0;
    if (!strcmp(b, "odd")) { *A = 2; *B = 1; return 1; }
    if (!strcmp(b, "even")) { *A = 2; *B = 0; return 1; }
    char *np = strchr(b, 'n');
    if (!np) { char *e; long v = strtol(b, &e, 10); if (e == b || *e) return 0; *A = 0; *B = v; return 1; }
    int sign = 1;
    char *p = b;
    if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
    *A = p == np ? sign : sign * (int)strtol(p, 0, 10);
    p = np + 1;
    if (!*p) { *B = 0; return 1; }
    char *e;
    long v = strtol(p, &e, 10);
    if (e == p || *e || (*p != '+' && *p != '-')) return 0;
    *B = v;
    return 1;
}

static const struct { const char *n; int pc; } PCS[] = {
    { "first-child", PC_FIRSTC }, { "last-child", PC_LASTC }, { "only-child", PC_ONLYC }, { "first-of-type", PC_FIRSTT },
    { "last-of-type", PC_LASTT }, { "only-of-type", PC_ONLYT }, { "empty", PC_EMPTY }, { "root", PC_ROOT }, { "scope", PC_ROOT },
    { "link", PC_LINK }, { "any-link", PC_LINK }, { "-webkit-any-link", PC_LINK }, { "visited", PC_NEVER }, { "hover", PC_HOVER },
    { "focus", PC_FOCUS }, { "focus-visible", PC_FOCUS }, { "active", PC_ACTIVE }, { "checked", PC_CHECKED }, { "disabled", PC_DISABLED },
    { "enabled", PC_ENABLED }, { "focus-within", PC_FOCUSWITHIN }, { "required", PC_REQUIRED }, { "optional", PC_OPTIONAL },
    { "read-only", PC_READONLY }, { "read-write", PC_READWRITE }, { "target", PC_NEVER }, { "placeholder-shown", PC_NEVER },
    { "indeterminate", PC_NEVER }, { "defined", PC_ALWAYS }, { "target-within", PC_NEVER }, { "user-invalid", PC_NEVER },
    { "valid", PC_ALWAYS }, { "invalid", PC_NEVER }, { "in-range", PC_ALWAYS }, { "out-of-range", PC_NEVER }, { "default", PC_NEVER },
    { "fullscreen", PC_NEVER }, { "autofill", PC_NEVER }, { "-webkit-autofill", PC_NEVER }, { "current", PC_NEVER }, { "past", PC_NEVER },
    { "future", PC_NEVER }, { "playing", PC_NEVER }, { "paused", PC_NEVER }, { "popover-open", PC_NEVER }, { "modal", PC_NEVER },
};

/* one compound selector at s[*pi]; simples go to t[] (max 32). Returns 1 ok, 0 error. *pe: pseudo-element seen */
static int parse_compound(const char *s, int n, int *pi, struct simple *t, int *nt, struct spc *sp, int *pe) {
    int i = *pi, any = 0;
    char id[160];
    *nt = 0;
    while (i < n) {
        char ch = s[i];
        if (isspace((unsigned char)ch) || ch == '>' || ch == '+' || ch == '~' || ch == ',') break;
        if (*nt >= 30) return 0;
        struct simple *x = &t[*nt];
        memset(x, 0, sizeof *x);
        if (*pe) return 0;                                  /* nothing may follow a pseudo-element */
        if (ch == '*') { i++; any = 1; if (i < n && s[i] == '|') return 0; continue; }
        if (ch == '#' || ch == '.') {
            i++;
            if (!read_ident(s, n, &i, id, sizeof id)) return 0;
            x->t = ch == '#' ? T_ID : T_CLASS; x->a = intern(id, strlen(id));
            if (ch == '#') sp->a++; else sp->b++;
            (*nt)++; any = 1;
            continue;
        }
        if (ch == '[') {
            int j = i + 1;
            while (j < n && isspace((unsigned char)s[j])) j++;
            char nm[64];
            if (!read_ident(s, n, &j, nm, sizeof nm)) return 0;
            if (j < n && s[j] == '|' && j + 1 < n && s[j + 1] != '=') return 0;      /* namespaces */
            lower(nm);
            x->t = T_ATTR; x->a = intern(nm, strlen(nm));
            while (j < n && isspace((unsigned char)s[j])) j++;
            if (j < n && s[j] != ']') {
                if (s[j] == '=') x->op = '=';
                else if (j + 1 < n && s[j + 1] == '=' && strchr("~|^$*", s[j])) x->op = s[j++];
                else return 0;
                j++;
                while (j < n && isspace((unsigned char)s[j])) j++;
                char v[300];
                int k = 0;
                if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    while (j < n && s[j] != q && k < 299) { if (s[j] == '\\' && j + 1 < n) j++; v[k++] = s[j++]; }
                    if (j >= n) return 0;
                    j++;
                } else if (!read_ident(s, n, &j, v, sizeof v)) {
                    if (j < n && (isdigit((unsigned char)s[j]))) { while (j < n && ident_char(s[j]) && k < 299) v[k++] = s[j++]; v[k] = 0; }
                    else return 0;
                } else k = strlen(v);
                v[k] = 0;
                x->b = intern(v, k);
                while (j < n && isspace((unsigned char)s[j])) j++;
                if (j < n && (s[j] | 32) == 'i') { x->ci = 1; j++; }
                else if (j < n && (s[j] | 32) == 's') j++;
                while (j < n && isspace((unsigned char)s[j])) j++;
            }
            if (j >= n || s[j] != ']') return 0;
            sp->b++; attrs_used = 1;
            i = j + 1; (*nt)++; any = 1;
            continue;
        }
        if (ch == ':') {
            int dbl = i + 1 < n && s[i + 1] == ':';
            int j = i + 1 + dbl;
            char nm[40];
            if (!read_ident(s, n, &j, nm, sizeof nm)) return 0;
            lower(nm);
            if (dbl || !strcmp(nm, "before") || !strcmp(nm, "after") || !strcmp(nm, "first-line") || !strcmp(nm, "first-letter")) {
                if (!strcmp(nm, "before")) *pe = PE_BEFORE;
                else if (!strcmp(nm, "after")) *pe = PE_AFTER;
                else return 0;                              /* ::marker, ::selection, ::placeholder, ::-webkit-*: not rendered */
                sp->c++;
                i = j; any = 1;
                continue;
            }
            int fn = j < n && s[j] == '(';
            int bs = 0, be = 0;
            if (fn && !paren_body(s, n, &j, &bs, &be)) return 0;
            if (fn && (!strcmp(nm, "not") || !strcmp(nm, "is") || !strcmp(nm, "where") || !strcmp(nm, "matches") || !strcmp(nm, "-webkit-any") || !strcmp(nm, "-moz-any") || !strcmp(nm, "has"))) {
                int has = !strcmp(nm, "has"), first = 0;
                int cnt = parse_list(s + bs, be - bs, has, &first);
                if (!cnt) return 0;
                x->t = has ? T_HAS : !strcmp(nm, "not") ? T_NOT : T_IS; x->a = first; x->b = cnt;
                if (strcmp(nm, "where")) {
                    struct spc m = { 0, 0, 0 };
                    for (int q = 0; q < cnt; q++) spec_max(&m, SE[first + q].spec);
                    sp->a += m.a; sp->b += m.b; sp->c += m.c;
                }
                if (has && 0) attrs_used = 1;
            } else if (fn && (!strncmp(nm, "nth-", 4))) {
                int op = !strcmp(nm, "nth-child") ? 0 : !strcmp(nm, "nth-last-child") ? 1 : !strcmp(nm, "nth-of-type") ? 2 : !strcmp(nm, "nth-last-of-type") ? 3 : -1;
                int A, B;
                if (op < 0 || !parse_nth(s + bs, be - bs, &A, &B)) return 0;
                x->t = T_NTH; x->op = op; x->a = A; x->b = B;
                sp->b++;
            } else if (fn && !strcmp(nm, "lang")) {
                x->t = T_PC; x->a = PC_LANG; x->b = intern(s + bs, be - bs);
                sp->b++;
            } else if (fn && !strcmp(nm, "dir")) {
                x->t = T_PC; x->a = PC_ALWAYS; sp->b++;
            } else if (!fn) {
                unsigned q;
                for (q = 0; q < sizeof PCS / sizeof *PCS && strcmp(PCS[q].n, nm); q++) ;
                if (q == sizeof PCS / sizeof *PCS) return 0;
                x->t = T_PC; x->a = PCS[q].pc;
                sp->b++;
            } else return 0;
            i = j; (*nt)++; any = 1;
            continue;
        }
        if (ident_char((unsigned char)ch)) {
            if (!read_ident(s, n, &i, id, sizeof id)) return 0;
            if (i < n && s[i] == '|') return 0;
            lower(id);
            x->t = T_TAG; x->a = intern(id, strlen(id));
            sp->c++; (*nt)++; any = 1;
            continue;
        }
        return 0;
    }
    *pi = i;
    /* cheap tests first (tag, id, class, attribute), pseudo-classes and nested selectors last */
    struct simple tmp[32];
    int k = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int q = 0; q < *nt; q++) if ((t[q].t <= T_ATTR) == !pass) tmp[k++] = t[q];
    memcpy(t, tmp, k * sizeof *t);
    return any;
}

static void sel_finish(struct sel *o) {
    memset(o->mask, 0, sizeof o->mask);
    for (int c = o->nc - 2; c >= 0; c--) {
        if (CO[o->c0 + c + 1].comb != ' ' && CO[o->c0 + c + 1].comb != '>') break;
        const struct comp *cp = &CO[o->c0 + c];
        for (int k = 0; k < cp->ns; k++) {
            const struct simple *x = &SI[cp->s0 + k];
            if (x->t == T_TAG) bloom_add(o->mask, bloom_key(1, STR(x->a)));
            else if (x->t == T_ID) bloom_add(o->mask, bloom_key(2, STR(x->a)));
            else if (x->t == T_CLASS) bloom_add(o->mask, bloom_key(3, STR(x->a)));
        }
    }
}

static int parse_complex(const char *s, int n, int rel, struct sel *out) {
    struct comp tc[16];
    struct simple ts[16][32];       /* per compound, copied into SI at the end */
    int tn[16], nc = 0, i = 0, pe = 0, comb = 0;
    struct spc sp = { 0, 0, 0 };
    memset(out, 0, sizeof *out);
    for (;;) {
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i >= n) break;
        if (nc == 0 && rel && (s[i] == '>' || s[i] == '+' || s[i] == '~')) { comb = s[i++]; while (i < n && isspace((unsigned char)s[i])) i++; }
        if (nc == 16) return 0;
        if (!parse_compound(s, n, &i, ts[nc], &tn[nc], &sp, &pe)) return 0;
        tc[nc].comb = nc ? comb : (rel ? (comb ? comb : ' ') : 0);
        tc[nc].ns = tn[nc];
        nc++;
        int ws = 0;
        while (i < n && isspace((unsigned char)s[i])) { i++; ws++; }
        if (i >= n) break;
        if (s[i] == '>' || s[i] == '+' || s[i] == '~') {
            if (pe) return 0;
            comb = s[i++];
            while (i < n && isspace((unsigned char)s[i])) i++;
            if (i >= n) return 0;
        } else if (ws) { if (pe) return 0; comb = ' '; }
        else return 0;
    }
    if (!nc) return 0;
    GROW(SI, nsi, csi, 0);
    int tot = 0;
    for (int c = 0; c < nc; c++) tot += tn[c];
    GROW(SI, nsi, csi, tot + 1);
    GROW(CO, nco, cco, nc);
    out->c0 = nco;
    for (int c = 0; c < nc; c++) {
        tc[c].s0 = nsi;
        memcpy(SI + nsi, ts[c], tn[c] * sizeof(struct simple));
        nsi += tn[c];
        CO[nco++] = tc[c];
    }
    out->nc = nc;
    out->pe = pe;
    out->spec = (uint32_t)(sp.a > 1023 ? 1023 : sp.a) << 20 | (uint32_t)(sp.b > 1023 ? 1023 : sp.b) << 10 | (uint32_t)(sp.c > 1023 ? 1023 : sp.c);
    sel_finish(out);
    return 1;
}

int sel_parse(const char *s, int n, struct sel *out) {
    int c0 = nco, s0 = nsi, e0 = nse;
    if (parse_complex(s, n, 0, out)) return 1;
    nco = c0; nsi = s0; nse = e0;      /* drop what a failed parse left behind */
    return 0;
}

int sel_push(const struct sel *s) {      /* append to SE (a top-level rule's selector); returns its index, -1 on OOM */
    if (nse + 1 > cse) { int nc = cse ? cse * 2 : 256; void *p = realloc(SE, nc * sizeof *SE); if (!p) return -1; SE = p; cse = nc; }
    SE[nse] = *s;
    return nse++;
}
void sel_mark(int *a, int *b, int *c) { *a = nsi; *b = nco; *c = nse; }
void sel_trunc(int a, int b, int c) { nsi = a; nco = b; nse = c; }

/* ---------- matching ---------- */
static int has_class(const char *list, const char *cls) {
    if (!list) return 0;
    int l = strlen(cls);
    if (!l) return 0;
    for (const char *p = list; *p; ) {
        while (isspace((unsigned char)*p)) p++;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e - p == l && !strncmp(p, cls, l)) return 1;
        p = e;
    }
    return 0;
}

static int attr_match(const struct simple *x, int node) {
    const char *v = DOM->attr(node, STR(x->a));
    if (!v) return 0;
    if (!x->op) return 1;
    const char *w = STR(x->b);
    int vl = strlen(v), cl = strlen(w);
#define EQ(a, b, l) (x->ci ? !strncasecmp(a, b, l) : !strncmp(a, b, l))
    switch (x->op) {
    case '=': return vl == cl && EQ(v, w, cl);
    case '^': return cl && vl >= cl && EQ(v, w, cl);
    case '$': return cl && vl >= cl && EQ(v + vl - cl, w, cl);
    case '|': return vl >= cl && EQ(v, w, cl) && (vl == cl || v[cl] == '-');
    case '*': for (int k = 0; cl && k + cl <= vl; k++) if (EQ(v + k, w, cl)) return 1; return 0;
    case '~': for (int k = 0; cl && k + cl <= vl; k++)
                  if ((!k || isspace((unsigned char)v[k - 1])) && EQ(v + k, w, cl) && (k + cl == vl || isspace((unsigned char)v[k + cl]))) return 1;
              return 0;
    }
#undef EQ
    return 0;
}

static struct { int node, idx; uint32_t th; } NC[4];     /* last :nth-* answers: walking siblings in order costs O(1) each */
static int nth_index(int n, int op) {
    int last = op & 1, oft = op & 2, cnt = 0;
    int (*step)(int) = last ? DOM->next : DOM->prev;
    uint32_t th = oft ? hash_str(DOM->tag(n)) : 0;
    const char *tg = oft ? DOM->tag(n) : 0;
    int res = 0;
    if (NC[op].node == n && NC[op].idx && NC[op].th == th) return NC[op].idx;
    for (int p = step(n); p; p = step(p)) {
        if (p == NC[op].node && NC[op].idx && NC[op].th == th) { res = NC[op].idx + cnt + 1; break; }
        if (!oft || !strcmp(DOM->tag(p), tg)) cnt++;
    }
    if (!res) res = cnt + 1;
    NC[op].node = n; NC[op].idx = res; NC[op].th = th;
    return res;
}

static int has_attr(int n, const char *a) { return DOM->attr(n, a) != 0; }
static int is_ctl(const char *t) { return !strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "select") || !strcmp(t, "textarea") || !strcmp(t, "fieldset") || !strcmp(t, "optgroup") || !strcmp(t, "option"); }

static int match_pc(int pc, const struct simple *x, int n) {
    switch (pc) {
    case PC_FIRSTC: return !DOM->prev(n);
    case PC_LASTC: return !DOM->next(n);
    case PC_ONLYC: return !DOM->prev(n) && !DOM->next(n);
    case PC_FIRSTT: return nth_index(n, 2) == 1;
    case PC_LASTT: return nth_index(n, 3) == 1;
    case PC_ONLYT: return nth_index(n, 2) == 1 && nth_index(n, 3) == 1;
    case PC_EMPTY: return !DOM->first(n) && !DOM->has_text(n);
    case PC_ROOT: return !DOM->parent(n);
    case PC_LINK: { const char *t = DOM->tag(n); return (DOM->state(n) & CS_LINK) || ((!strcmp(t, "a") || !strcmp(t, "area")) && has_attr(n, "href")); }
    case PC_HOVER: return DOM->state(n) & CS_HOVER;
    case PC_FOCUS: return DOM->state(n) & CS_FOCUS;
    case PC_ACTIVE: return DOM->state(n) & CS_ACTIVE;
    case PC_FOCUSWITHIN: return DOM->state(n) & (CS_FOCUS | CS_FOCUSWITHIN);
    case PC_CHECKED: { const char *t = DOM->tag(n); return (DOM->state(n) & CS_CHECKED) || (!strcmp(t, "input") && has_attr(n, "checked")) || (!strcmp(t, "option") && has_attr(n, "selected")); }
    case PC_DISABLED: return is_ctl(DOM->tag(n)) && ((DOM->state(n) & CS_DISABLED) || has_attr(n, "disabled"));
    case PC_ENABLED: return is_ctl(DOM->tag(n)) && !((DOM->state(n) & CS_DISABLED) || has_attr(n, "disabled"));
    case PC_REQUIRED: return has_attr(n, "required");
    case PC_OPTIONAL: { const char *t = DOM->tag(n); return (!strcmp(t, "input") || !strcmp(t, "select") || !strcmp(t, "textarea")) && !has_attr(n, "required"); }
    case PC_READONLY: { const char *t = DOM->tag(n); int ed = (!strcmp(t, "input") || !strcmp(t, "textarea")) && !has_attr(n, "readonly") && !has_attr(n, "disabled"); return !ed; }
    case PC_READWRITE: { const char *t = DOM->tag(n); return (!strcmp(t, "input") || !strcmp(t, "textarea")) && !has_attr(n, "readonly") && !has_attr(n, "disabled"); }
    case PC_ALWAYS: return 1;
    case PC_LANG: { const char *w = STR(x->b); return !strncasecmp(w, "en", 2) || !*w; }   /* ponytail: we have no lang inheritance; pretend English */
    }
    return 0;
}

static int match_chain(const struct sel *s, int ci, int n);

static int match_list(int first, int cnt, int n) {
    for (int i = 0; i < cnt; i++) if (match_chain(&SE[first + i], SE[first + i].nc - 1, n)) return 1;
    return 0;
}

/* :has(): does some node related to `from` by the relative selector match? forward search from compound i */
static int match_comp(const struct comp *c, int n);
static int has_fwd(const struct sel *s, int i, int from) {
    if (i == s->nc) return 1;
    const struct comp *c = &CO[s->c0 + i];
    int comb = c->comb ? c->comb : ' ', cand;
    if (comb == '+') { cand = DOM->next(from); return cand && match_comp(c, cand) && has_fwd(s, i + 1, cand); }
    if (comb == '~' || comb == '>') {
        for (cand = comb == '~' ? DOM->next(from) : DOM->first(from); cand; cand = DOM->next(cand))
            if (match_comp(c, cand) && has_fwd(s, i + 1, cand)) return 1;
        return 0;
    }
    for (cand = DOM->first(from); cand; ) {                 /* descendants, document order */
        if (match_comp(c, cand) && has_fwd(s, i + 1, cand)) return 1;
        int nx = DOM->first(cand);
        for (int u = cand; !nx && u && u != from; u = DOM->parent(u)) nx = DOM->next(u);
        cand = nx;
    }
    return 0;
}

static int match_simple(const struct simple *x, int n) {
    switch (x->t) {
    case T_TAG: return !strcmp(DOM->tag(n), STR(x->a));
    case T_ID: { const char *v = DOM->attr(n, "id"); return v && !strcmp(v, STR(x->a)); }
    case T_CLASS: return has_class(DOM->attr(n, "class"), STR(x->a));
    case T_ATTR: return attr_match(x, n);
    case T_PC: return match_pc(x->a, x, n);
    case T_NTH: {
        int idx = nth_index(n, x->op), A = x->a, B = x->b;
        if (!A) return idx == B;
        int d = idx - B;
        return d % A == 0 && d / A >= 0;
    }
    case T_NOT: return !match_list(x->a, x->b, n);
    case T_IS: return match_list(x->a, x->b, n);
    case T_HAS: for (int q = 0; q < x->b; q++) if (has_fwd(&SE[x->a + q], 0, n)) return 1; return 0;
    }
    return 0;
}

static int match_comp(const struct comp *c, int n) {
    for (int k = 0; k < c->ns; k++) if (!match_simple(&SI[c->s0 + k], n)) return 0;
    return 1;
}

static int match_chain(const struct sel *s, int ci, int n) {
    const struct comp *c = &CO[s->c0 + ci];
    if (!match_comp(c, n)) return 0;
    if (!ci) return 1;
    int p;
    switch (c->comb) {
    case '>': p = DOM->parent(n); return p && match_chain(s, ci - 1, p);
    case '+': p = DOM->prev(n); return p && match_chain(s, ci - 1, p);
    case '~': for (p = DOM->prev(n); p; p = DOM->prev(p)) if (match_chain(s, ci - 1, p)) return 1; return 0;
    default:  for (p = DOM->parent(n); p; p = DOM->parent(p)) if (match_chain(s, ci - 1, p)) return 1; return 0;
    }
}

int sel_match(const struct sel *s, int node) { return match_chain(s, s->nc - 1, node); }

void sel_key(const struct sel *s, int *kind, uint32_t *h) {
    const struct comp *c = &CO[s->c0 + s->nc - 1];
    int cls = -1, tag = -1;
    *kind = 0; *h = 0;
    for (int k = 0; k < c->ns; k++) {
        const struct simple *x = &SI[c->s0 + k];
        if (x->t == T_ID) { *kind = 1; *h = hash_str(STR(x->a)); return; }
        if (x->t == T_CLASS && cls < 0) cls = x->a;
        if (x->t == T_TAG) tag = x->a;
    }
    if (cls >= 0) { *kind = 2; *h = hash_str(STR(cls)); }
    else if (tag >= 0) { *kind = 3; *h = hash_str(STR(tag)); }
}
