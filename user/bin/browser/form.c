/* form.c - form controls: state that outlives a re-layout (indexed by control ordinal), drawing,
 * inline editing (UTF-8, selection with Shift+arrows / Ctrl+A / mouse drag, Ctrl+C/X/V), soft-wrapping textareas,
 * placeholders, required / type=email checks and the GET/POST submission string. */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAXORD 4096
#define MAXVAL 8192
static struct fstate *F;
static int nf;

void fs_reset(void) {
    for (int i = 0; i < nf; i++) free(F[i].val);
    free(F);
    F = 0; nf = 0;
}

/* background tabs keep their field values (see img_save) */
struct fs_saved { struct fstate *F; int nf; };
void *fs_save(void) {
    struct fs_saved *h = malloc(sizeof *h);
    if (!h) return 0;
    *h = (struct fs_saved){ F, nf };
    F = 0; nf = 0;
    return h;
}
void fs_restore(void *v) { struct fs_saved *h = v; fs_reset(); F = h->F; nf = h->nf; free(h); }
void fs_drop(void *v) {
    struct fs_saved *h = v;
    if (!h) return;
    for (int i = 0; i < h->nf; i++) free(h->F[i].val);
    free(h->F); free(h);
}

struct fstate *fs_ord(int ord) {
    if (ord < 0 || ord >= MAXORD) return 0;
    if (ord >= nf) {
        int n = ord + 32 > MAXORD ? MAXORD : ord + 32;
        struct fstate *nF = realloc(F, n * sizeof *F);
        if (!nF) return 0;
        memset(nF + nf, 0, (n - nf) * sizeof *F);
        F = nF; nf = n;
    }
    return &F[ord];
}

static void set_val(struct fstate *s, const char *v) {
    int n = strlen(v);
    if (n >= MAXVAL) n = MAXVAL - 1;
    if (n + 1 > s->cap) {
        char *nv = realloc(s->val, n + 64);
        if (!nv) return;
        s->val = nv; s->cap = n + 64;
    }
    memcpy(s->val, v, n);
    s->val[n] = 0;
}

void fs_set(struct fstate *s, const char *v) { set_val(s, v); }

struct fstate *fs_get(const struct doc *d, int ci) {
    static struct fstate dummy;
    const struct ctl *c = &d->ct[ci];
    struct fstate *s = fs_ord(c->ord);
    if (!s) return &dummy;
    if (!s->init) {
        s->init = 1;
        s->on = c->chk;
        s->sel = c->kind == C_SELECT ? c->rows : 0;
        set_val(s, d->txt + c->val);
    }
    return s;
}

/* ---------- editing ---------- */
static void ins(struct fstate *s, int pos, const char *t, int n) {
    int len = s->val ? strlen(s->val) : 0;
    if (len + n >= MAXVAL) n = MAXVAL - 1 - len;
    if (n <= 0) return;
    if (len + n + 1 > s->cap) {
        char *nv = realloc(s->val, len + n + 64);
        if (!nv) return;
        s->val = nv; s->cap = len + n + 64;
    }
    memmove(s->val + pos + n, s->val + pos, len - pos + 1);
    memcpy(s->val + pos, t, n);
}

static void del(struct fstate *s, int pos, int n) { memmove(s->val + pos, s->val + pos + n, strlen(s->val + pos + n) + 1); }
static int prev_ch(const char *v, int p) { if (p > 0) p--; while (p > 0 && (v[p] & 0xC0) == 0x80) p--; return p; }
static int next_ch(const char *v, int p) { if (v[p]) p++; while (v[p] && (v[p] & 0xC0) == 0x80) p++; return p; }
static int line_end(const char *v, int p) { while (v[p] && v[p] != '\n') p++; return p; }

/* soft-wrapped lines of a textarea value: st[i] = byte offset where visual line i starts (hard newlines and word/char wraps at wpx) */
#define MAXVL 1024
static int vlines(const char *v, int wpx, int *st, int max) {
    int n = 0, p = 0;
    for (;;) {
        st[n++] = p;
        int le = line_end(v, p), q = p;
        while (q < le) {
            int e = q, sp = -1, w = 0;
            while (e < le) {
                int ne = next_ch(v, e), cw = gfx_text_width_n(&font_ui, v + e, ne - e);
                if (w + cw > wpx && e > q) break;
                w += cw;
                if (v[e] == ' ') sp = e + 1;
                e = ne;
            }
            if (e < le && sp > q) e = sp;
            q = e;
            if (q < le) { if (n >= max) return n; st[n++] = q; }
        }
        if (!v[le] || n >= max) return n;
        p = le + 1;
    }
}
static int vl_of(const int *st, int n, int p) { int i = 0; while (i + 1 < n && st[i + 1] <= p) i++; return i; }
static int vl_end(const char *v, const int *st, int n, int i) { int le = line_end(v, st[i]); return i + 1 < n && st[i + 1] < le ? st[i + 1] : le; }
static int pos_at_x(const char *v, int a, int e, int x) {          /* byte in v[a..e] nearest pixel x */
    int k = a, acc = 0;
    while (k < e) {
        int nk = next_ch(v, k), cw = gfx_text_width_n(&font_ui, v + k, nk - k);
        if (x < acc + cw / 2) break;
        acc += cw; k = nk;
    }
    return k;
}
static int field_w(const struct doc *d, const struct ctl *c) { return c->item >= 0 ? d->it[c->item].w - 16 : 200; }

static int sel_range(const struct fstate *s, int p, int *a, int *b) {      /* the selection (anchor = anc1 - 1 .. caret) */
    int q = s->anc1 - 1;
    if (!s->anc1 || q == p) return 0;
    *a = q < p ? q : p; *b = q < p ? p : q;
    return 1;
}

int ctl_key(const struct doc *d, int ci, const struct gui_event *k, int *caret) {
    const struct ctl *c = &d->ct[ci];
    if (c->kind != C_TEXT && c->kind != C_PASS && c->kind != C_AREA) return 0;
    struct fstate *s = fs_get(d, ci);
    if (!s->val || (c->fl & CF_RO)) return 0;
    int area = c->kind == C_AREA, p = *caret, len = strlen(s->val);
    if (p > len) p = len;
    if (s->anc1 - 1 > len) s->anc1 = 0;
    int ctrl = k->mods & MOD_CTRL, shift = k->mods & MOD_SHIFT, a = 0, b = 0, has = sel_range(s, p, &a, &b), nav = 0, np = p;
    int st[MAXVL], nl = 0, li = 0;
    if (area) { nl = vlines(s->val, field_w(d, c), st, MAXVL); li = vl_of(st, nl, p); }
#define KILLSEL() do { if (has) { del(s, a, b - a); p = a; len -= b - a; has = 0; } s->anc1 = 0; } while (0)
    if (k->key == KEY_ENTER) {
        if (!area) return 1;
        KILLSEL();
        ins(s, p, "\n", 1); p++;
    } else if (k->key == KEY_BACKSPACE || (!ctrl && k->ch == 8)) {
        if (has) KILLSEL(); else if (p) { int q = prev_ch(s->val, p); del(s, q, p - q); p = q; }
    }
    else if (k->key == KEY_DELETE && k->ch != 8) { if (has) KILLSEL(); else { int q = next_ch(s->val, p); del(s, p, q - p); } }
    else if (k->key == KEY_LEFT) { np = prev_ch(s->val, p); nav = 1; }
    else if (k->key == KEY_RIGHT) { np = next_ch(s->val, p); nav = 1; }
    else if (k->key == KEY_HOME) { np = area ? (ctrl ? 0 : st[li]) : 0; nav = 1; }
    else if (k->key == KEY_END) {
        np = len; nav = 1;
        if (area && !ctrl) { int e = vl_end(s->val, st, nl, li); np = li + 1 < nl && st[li + 1] == e ? prev_ch(s->val, e) : e; }
    }
    else if (area && (k->key == KEY_UP || k->key == KEY_DOWN)) {
        int x = gfx_text_width_n(&font_ui, s->val + st[li], p - st[li]), tl = li + (k->key == KEY_UP ? -1 : 1);
        np = tl < 0 ? 0 : tl >= nl ? len : pos_at_x(s->val, st[tl], vl_end(s->val, st, nl, tl), x);
        nav = 1;
    }
    else if (k->ch == 21) { s->val[0] = 0; p = 0; s->anc1 = 0; has = 0; }                /* ^U */
    else if (ctrl && (k->key | 32) == 'a') { s->anc1 = 1; p = len; has = 0; }
    else if (ctrl && (k->key | 32) == 'v') {
        KILLSEL();
        char *cl = gui_clip_get(NULL);
        for (char *q = cl; q && *q; q++) {
            if (*q == '\n' && area) { ins(s, p, "\n", 1); p++; }
            else if (*q == '\n') break;
            else if ((unsigned char)*q >= 32 || *q == '\t') { char ch = *q == '\t' ? ' ' : *q; ins(s, p, &ch, 1); p++; }
        }
        free(cl);
    }
    else if (ctrl && ((k->key | 32) == 'c' || (k->key | 32) == 'x')) {
        if (has) gui_clip_set(s->val + a, b - a); else gui_clip_set(s->val, len);
        if ((k->key | 32) == 'x' && has) KILLSEL();
    }
    else if (!ctrl && k->ch >= 32 && k->ch != 127) {                       /* a character (ASCII, or a code point from a better keymap) */
        char u[4];
        int n = 1;
        if (k->ch < 128) u[0] = k->ch;
        else if (k->ch < 0x800) { u[0] = 0xC0 | k->ch >> 6; u[1] = 0x80 | (k->ch & 63); n = 2; }
        else { u[0] = 0xE0 | k->ch >> 12; u[1] = 0x80 | (k->ch >> 6 & 63); u[2] = 0x80 | (k->ch & 63); n = 3; }
        KILLSEL();
        ins(s, p, u, n); p += n;
    }
    if (nav) {                                                              /* arrows: Shift extends the selection, otherwise it collapses */
        if (shift) { if (!s->anc1) s->anc1 = p + 1; }
        else { if (has && k->key == KEY_LEFT) np = a; else if (has && k->key == KEY_RIGHT) np = b; s->anc1 = 0; }
        p = np;
    }
    *caret = p;
    return 0;
}
#undef KILLSEL

/* a required / email field that isn't acceptable: its control index and a message, else -1 */
int form_check(const struct doc *d, int form, char *msg, int cap) {
    for (int i = 0; i < d->nct; i++) {
        const struct ctl *c = &d->ct[i];
        if (c->form != form) continue;
        struct fstate *s = fs_get(d, i);
        const char *v = s->val ? s->val : "";
        if ((c->kind == C_TEXT || c->kind == C_PASS || c->kind == C_AREA) && (c->fl & (CF_REQ | CF_EMAIL))) {
            if ((c->fl & CF_REQ) && !*v) { strlcpy(msg, "Please fill out this field", cap); return i; }
            if ((c->fl & CF_EMAIL) && *v) {
                const char *at = strchr(v, '@');
                if (!at || at == v || !at[1] || strchr(at + 1, '@') || strchr(v, ' ')) { strlcpy(msg, "Please enter an email address", cap); return i; }
            }
        } else if (c->kind == C_CHECK && (c->fl & CF_REQ) && !s->on) { strlcpy(msg, "Please tick this box to continue", cap); return i; }
        else if (c->kind == C_RADIO && (c->fl & CF_REQ)) {
            int any = 0;
            for (int j = 0; j < d->nct; j++)
                if (d->ct[j].kind == C_RADIO && d->ct[j].form == form && !strcmp(d->txt + d->ct[j].name, d->txt + c->name) && fs_get(d, j)->on) any = 1;
            if (!any) { strlcpy(msg, "Please select one of these options", cap); return i; }
        }
    }
    return -1;
}

void ctl_toggle(const struct doc *d, int ci) {
    const struct ctl *c = &d->ct[ci];
    struct fstate *s = fs_get(d, ci);
    if (c->kind == C_CHECK) s->on = !s->on;
    else if (c->kind == C_RADIO) {
        for (int i = 0; i < d->nct; i++) {                                /* one per name within the form */
            const struct ctl *o = &d->ct[i];
            if (o->kind == C_RADIO && o->form == c->form && !strcmp(d->txt + o->name, d->txt + c->name)) fs_get(d, i)->on = 0;
        }
        s->on = 1;
    }
}

void form_reset(const struct doc *d, int form) {
    for (int i = 0; i < d->nct; i++) {
        if (d->ct[i].form != form) continue;
        struct fstate *s = fs_ord(d->ct[i].ord);
        if (s) { s->init = 0; s->sx = s->sy = 0; }
    }
}

/* ---------- drawing ---------- */
static int dark_page(const struct doc *d) { return (((d->bg >> 16) & 255) * 3 + ((d->bg >> 8) & 255) * 6 + (d->bg & 255)) / 10 < 110; }

/* page content keeps neutral web colours; only focus and checked marks take the theme accent, kept at 3:1 on the page */
static color_t page_accent(const struct doc *d) {
    color_t a = ui_th.accent_text, to = dark_page(d) ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
    for (int i = 0; i < 8 && theme_contrast(a, d->bg) < 300; i++) a = mix(a, to, 48);
    return a;
}

static int caret_x(const char *v, int p) { return gfx_text_width_n(&font_ui, v, p); }

void ctl_draw(struct canvas *cv, const struct doc *d, const struct ctl *t, int x, int y, int w, int h, int focused, int caret) {
    int ci = t - d->ct, dk = dark_page(d);
    struct fstate *s = fs_get(d, ci);
    color_t fill = dk ? RGB(0x23, 0x24, 0x27) : RGB(0xFF, 0xFF, 0xFF), line = dk ? RGB(0x74, 0x77, 0x7E) : RGB(0x76, 0x76, 0x76),
            txt = dk ? RGB(0xE6, 0xE7, 0xE9) : RGB(0x1A, 0x1A, 0x22), dim = dk ? RGB(0xA4, 0xA7, 0xAD) : RGB(0x6B, 0x6B, 0x73);
    color_t acc = page_accent(d), ring = focused ? acc : line;
    struct canvas c2 = *cv;
    switch (t->kind) {
    case C_TEXT: case C_PASS: case C_AREA: {
        gfx_round_fill(cv, x, y, w, h, 4, fill);
        gfx_round_rect(cv, x, y, w, h, 4, ring);
        if (focused) gfx_ring_out(cv, x, y, w, h, 4, ring);
        canvas_clip(&c2, x + 4, y + 2, w - 8, h - 4);
        const char *v = s->val ? s->val : "";
        char *shown = 0;
        int sa = 0, sb = 0, hs = focused && sel_range(s, caret, &sa, &sb);
        if (t->kind == C_PASS) { int n = strlen(v); shown = malloc(n * 3 + 1); if (shown) { for (int i = 0; i < n; i++) memcpy(shown + i * 3, "\xE2\x80\xA2", 3); shown[n * 3] = 0; v = shown; } sa *= 3; sb *= 3; }
        int cp = t->kind == C_PASS ? caret * 3 : caret;
        if (cp > (int)strlen(v)) cp = strlen(v);
        color_t selc = mix(fill, acc, 110);
        const char *ph = d->txt + t->ph;
        if (t->kind == C_AREA) {
            int rows = (h - 10) / 18, st[MAXVL], nl = vlines(v, w - 16, st, MAXVL), cl = vl_of(st, nl, focused ? cp : 0);
            if (focused) { if (cl < s->sy) s->sy = cl; if (cl >= s->sy + rows) s->sy = cl - rows + 1; }
            if (s->sy > nl - 1) s->sy = nl - 1;
            if (s->sy < 0) s->sy = 0;
            if (!*v && *ph) gfx_text_n(&c2, &font_ui, x + 6, y + 5, ph, strlen(ph), dim);
            for (int r = 0; r < rows + 1 && s->sy + r < nl; r++) {
                int li = s->sy + r, ls = st[li], le = vl_end(v, st, nl, li), ty = y + 5 + r * 18;
                if (hs && sb > ls && sa < le + (li + 1 < nl && st[li + 1] == le ? 0 : 1)) {
                    int x0 = caret_x(v + ls, (sa > ls ? sa : ls) - ls), x1 = caret_x(v + ls, (sb < le ? sb : le) - ls);
                    gfx_fill(&c2, x + 6 + x0, ty, (x1 > x0 ? x1 - x0 : 4), 18, selc);
                }
                gfx_text_n(&c2, &font_ui, x + 6, ty, v + ls, le - ls, txt);
                if (focused && cl == li) gfx_fill(&c2, x + 6 + caret_x(v + ls, cp - ls), ty + 1, 1, 15, txt);
            }
        } else {
            int cw = focused ? caret_x(v, cp) : 0;
            s->sx = focused && cw > w - 16 ? cw - (w - 16) : 0;
            if (!focused) s->sx = 0;
            int ty = y + (h - font_ui.height) / 2;
            if (!*v && *ph) gfx_text(&c2, &font_ui, x + 6, ty, ph, dim);
            if (hs) gfx_fill(&c2, x + 6 - s->sx + caret_x(v, sa), ty, caret_x(v, sb) - caret_x(v, sa), font_ui.height, selc);
            gfx_text(&c2, &font_ui, x + 6 - s->sx, ty, v, txt);
            if (focused) gfx_fill(&c2, x + 6 - s->sx + cw, ty, 1, font_ui.height, txt);
        }
        free(shown);
        break;
    }
    case C_BTN: {
        int hot = focused;
        gfx_round_fill(cv, x, y, w, h, 4, dk ? RGB(0x2C, 0x2D, 0x31) : RGB(0xEF, 0xEF, 0xF1));
        gfx_round_rect(cv, x, y, w, h, 4, ring);
        if (hot) gfx_ring_out(cv, x, y, w, h, 4, ring);
        gfx_text_center(cv, &font_ui, x, y + (h - font_ui.height) / 2, w, d->txt + t->o0, txt);
        break;
    }
    case C_CHECK:
        gfx_round_fill(cv, x, y, w, h, 3, fill);
        gfx_round_rect(cv, x, y, w, h, 3, ring);
        if (s->on) { gfx_line_aa(cv, x + 3.5f, y + 8.5f, x + 6.5f, y + 12, 2, acc); gfx_line_aa(cv, x + 6.5f, y + 12, x + 12.5f, y + 4, 2, acc); }
        break;
    case C_RADIO:
        gfx_circle_fill(cv, x + 8, y + 8, 8, ring);
        gfx_circle_fill(cv, x + 8, y + 8, 7, fill);
        if (s->on) gfx_circle_fill(cv, x + 8, y + 8, 4, acc);
        break;
    case C_SELECT: {
        gfx_round_fill(cv, x, y, w, h, 4, fill);
        gfx_round_rect(cv, x, y, w, h, 4, ring);
        canvas_clip(&c2, x + 4, y, w - 24, h);
        if (t->no) gfx_text(&c2, &font_ui, x + 8, y + (h - font_ui.height) / 2, d->txt + d->op[t->o0 + (s->sel < t->no ? s->sel : 0)].lab, txt);
        int ax = x + w - 16, ay = y + h / 2 - 2;
        for (int i = 0; i < 5; i++) gfx_fill(cv, ax + i, ay + i, 9 - 2 * i, 1, dim);
        break;
    }
    }
}

int ctl_caret_at(const struct doc *d, int ci, int rx, int ry) {
    const struct ctl *c = &d->ct[ci];
    struct fstate *s = fs_get(d, ci);
    const char *v = s->val ? s->val : "";
    int a = 0, le = strlen(v), x = rx - 6 + (c->kind == C_AREA ? 0 : s->sx);
    if (c->kind == C_AREA) {
        int st[MAXVL], nl = vlines(v, field_w(d, c), st, MAXVL), li = (ry - 5) / 18 + s->sy;
        li = li < 0 ? 0 : li >= nl ? nl - 1 : li;
        a = st[li]; le = vl_end(v, st, nl, li);
    }
    if (c->kind == C_PASS) {                                              /* bullets: one char = 3 bytes of display, 1 of value */
        int n = le - a, w = gfx_text_width(&font_ui, "\xE2\x80\xA2"), k = (x + w / 2) / (w ? w : 1);
        return a + (k < 0 ? 0 : k > n ? n : k);
    }
    return pos_at_x(v, a, le, x);
}

/* ---------- submit ---------- */
static void pct(char *o, int cap, int *n, const char *v) {
    static const char H[] = "0123456789ABCDEF";
    for (; *v; v++) {
        unsigned char ch = *v;
        if (*n + 4 >= cap) break;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o[(*n)++] = ch;
        else if (ch == ' ') o[(*n)++] = '+';
        else if (ch == '\n') { memcpy(o + *n, "%0D%0A", 6); *n += 6; }
        else if (ch == '\r') continue;
        else { o[(*n)++] = '%'; o[(*n)++] = H[ch >> 4]; o[(*n)++] = H[ch & 15]; }
    }
    o[*n] = 0;
}

static void pair(char *q, int cap, int *n, const char *name, const char *val) {
    if (*n && *n + 2 < cap) q[(*n)++] = '&';
    pct(q, cap, n, name);
    if (*n + 2 < cap) q[(*n)++] = '=';
    pct(q, cap, n, val);
}

int form_submit(const struct doc *d, int form, int btn, const char *base, char *url, int ucap, char *body, int bcap) {
    char q[2048];
    int n = 0, first_submit = -1;
    q[0] = 0;
    for (int i = 0; i < d->nct; i++) if (d->ct[i].form == form && d->ct[i].kind == C_BTN && d->ct[i].sub == 1) { first_submit = i; break; }
    if (btn < 0) btn = first_submit;
    for (int i = 0; i < d->nct; i++) {
        const struct ctl *c = &d->ct[i];
        const char *name = d->txt + c->name;
        if (c->form != form || !*name) continue;
        struct fstate *s = fs_get(d, i);
        switch (c->kind) {
        case C_TEXT: case C_PASS: case C_AREA: case C_HIDDEN: pair(q, sizeof q, &n, name, s->val ? s->val : ""); break;
        case C_CHECK: case C_RADIO: if (s->on) pair(q, sizeof q, &n, name, d->txt + c->val); break;
        case C_SELECT: if (c->no) pair(q, sizeof q, &n, name, d->txt + d->op[c->o0 + (s->sel < c->no ? s->sel : 0)].val); break;
        case C_BTN: if (i == btn && c->sub == 1) pair(q, sizeof q, &n, name, d->txt + c->val); break;
        }
    }
    const struct form *f = &d->fm[form];
    const char *act = d->txt + f->action;
    char t[URL_MAX];
    url_resolve(base, *act ? act : base, t, sizeof t);
    url_strip_frag(t);
    if (f->post) {
        strlcpy(url, t, ucap);
        strlcpy(body, q, bcap);
        return 1;
    }
    char *qm = strchr(t, '?');
    if (qm) *qm = 0;
    snprintf(url, ucap, "%s?%s", t, q);
    body[0] = 0;
    return 0;
}
