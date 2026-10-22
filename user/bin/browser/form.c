/* form.c - form controls: state that outlives a re-layout (indexed by control ordinal), drawing,
 * inline editing and the GET/POST submission string. ponytail: ASCII input only, no selection inside
 * a field, textareas don't soft-wrap. */
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
static int line_start(const char *v, int p) { while (p > 0 && v[p - 1] != '\n') p--; return p; }
static int line_end(const char *v, int p) { while (v[p] && v[p] != '\n') p++; return p; }

int ctl_key(const struct doc *d, int ci, const struct gui_event *k, int *caret) {
    const struct ctl *c = &d->ct[ci];
    if (c->kind != C_TEXT && c->kind != C_PASS && c->kind != C_AREA) return 0;
    struct fstate *s = fs_get(d, ci);
    if (!s->val) return 0;
    int area = c->kind == C_AREA, p = *caret, len = strlen(s->val);
    if (p > len) p = len;
    int ctrl = k->mods & MOD_CTRL;
    if (k->key == KEY_ENTER) {
        if (!area) return 1;
        ins(s, p, "\n", 1); p++;
    } else if (k->key == KEY_BACKSPACE || (!ctrl && k->ch == 8)) { if (p) { int q = prev_ch(s->val, p); del(s, q, p - q); p = q; } }
    else if (k->key == KEY_DELETE && k->ch != 8) { int q = next_ch(s->val, p); del(s, p, q - p); }
    else if (k->key == KEY_LEFT) p = prev_ch(s->val, p);
    else if (k->key == KEY_RIGHT) p = next_ch(s->val, p);
    else if (k->key == KEY_HOME) p = area ? line_start(s->val, p) : 0;
    else if (k->key == KEY_END) p = area ? line_end(s->val, p) : len;
    else if (area && (k->key == KEY_UP || k->key == KEY_DOWN)) {
        int ls = line_start(s->val, p), col = p - ls;
        if (k->key == KEY_UP) {
            if (ls) { int ps = line_start(s->val, ls - 1), pl = ls - 1 - ps; p = ps + (col < pl ? col : pl); }
        } else {
            int le = line_end(s->val, p);
            if (s->val[le]) { int ns = le + 1, nl = line_end(s->val, ns) - ns; p = ns + (col < nl ? col : nl); }
        }
    }
    else if (k->ch == 21) { s->val[0] = 0; p = 0; }                       /* ^U */
    else if (ctrl && (k->key | 32) == 'v') {
        char *cl = gui_clip_get(NULL);
        for (char *q = cl; q && *q; q++) {
            if (*q == '\n' && area) { ins(s, p, "\n", 1); p++; }
            else if (*q == '\n') break;
            else if ((unsigned char)*q >= 32 || *q == '\t') { char ch = *q == '\t' ? ' ' : *q; ins(s, p, &ch, 1); p++; }
        }
        free(cl);
    }
    else if (ctrl && (k->key | 32) == 'c') gui_clip_set(s->val, strlen(s->val));
    else if (!ctrl && k->ch >= 32 && k->ch < 127) { char ch = k->ch; ins(s, p, &ch, 1); p++; }
    *caret = p;
    return 0;
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

static int caret_x(const char *v, int p) { return gfx_text_width_n(&font_ui, v, p); }

static void line_geometry(const char *v, int *lines, int p, int *cl) {     /* number of lines, line of byte p */
    int n = 1, c = 0;
    for (int i = 0; v[i]; i++) if (v[i] == '\n') { n++; if (i < p) c++; }
    *lines = n; *cl = c;
}

void ctl_draw(struct canvas *cv, const struct doc *d, const struct ctl *t, int x, int y, int w, int h, int focused, int caret) {
    int ci = t - d->ct, dk = dark_page(d);
    struct fstate *s = fs_get(d, ci);
    color_t fill = dk ? RGB(0x1E, 0x1C, 0x2D) : RGB(0xFF, 0xFF, 0xFF), line = dk ? RGB(0x5A, 0x56, 0x7A) : RGB(0x76, 0x76, 0x76),
            txt = dk ? UI_TEXT : RGB(0x1A, 0x1A, 0x22), dim = dk ? UI_TEXT_DIM : RGB(0x80, 0x80, 0x88);
    color_t ring = focused ? UI_ACCENT : line;
    struct canvas c2 = *cv;
    switch (t->kind) {
    case C_TEXT: case C_PASS: case C_AREA: {
        gfx_round_fill(cv, x, y, w, h, 4, fill);
        gfx_round_rect(cv, x, y, w, h, 4, ring);
        if (focused) gfx_round_rect(cv, x - 1, y - 1, w + 2, h + 2, 5, RGBA(0xB3, 0x88, 0xFF, 110));
        canvas_clip(&c2, x + 4, y + 2, w - 8, h - 4);
        const char *v = s->val ? s->val : "";
        char *shown = 0;
        if (t->kind == C_PASS) { int n = strlen(v); shown = malloc(n * 3 + 1); if (shown) { for (int i = 0; i < n; i++) memcpy(shown + i * 3, "\xE2\x80\xA2", 3); shown[n * 3] = 0; v = shown; } }
        int cp = t->kind == C_PASS ? caret * 3 : caret;
        if (cp > (int)strlen(v)) cp = strlen(v);
        if (t->kind == C_AREA) {
            int lines, cl, rows = (h - 10) / 18;
            line_geometry(v, &lines, focused ? cp : 0, &cl);
            if (focused) { if (cl < s->sy) s->sy = cl; if (cl >= s->sy + rows) s->sy = cl - rows + 1; }
            if (s->sy > lines - 1) s->sy = lines - 1;
            if (s->sy < 0) s->sy = 0;
            const char *p = v;
            for (int i = 0; i < s->sy && *p; i++) { p = strchr(p, '\n'); if (!p) break; p++; }
            for (int r = 0; p && r < rows + 1; r++) {
                int le = line_end(p, 0);
                gfx_text_n(&c2, &font_ui, x + 6, y + 5 + r * 18, p, le, txt);
                if (focused && cl == s->sy + r) gfx_fill(&c2, x + 6 + caret_x(p, cp - (int)(p - v)), y + 6 + r * 18, 1, 15, UI_ACCENT);
                if (!p[le]) break;
                p += le + 1;
            }
        } else {
            int cw = focused ? caret_x(v, cp) : 0;
            s->sx = focused && cw > w - 16 ? cw - (w - 16) : 0;
            if (!focused) s->sx = 0;
            int ty = y + (h - font_ui.height) / 2;
            gfx_text(&c2, &font_ui, x + 6 - s->sx, ty, v, txt);
            if (focused) gfx_fill(&c2, x + 6 - s->sx + cw, ty, 1, font_ui.height, UI_ACCENT);
        }
        free(shown);
        break;
    }
    case C_BTN: {
        int hot = focused;
        gfx_round_fill(cv, x, y, w, h, 5, dk ? (hot ? UI_HOVER : UI_SURFACE) : RGB(0xEF, 0xEF, 0xF3));
        gfx_round_rect(cv, x, y, w, h, 5, ring);
        gfx_text_center(cv, &font_ui, x, y + (h - font_ui.height) / 2, w, d->txt + t->o0, txt);
        break;
    }
    case C_CHECK:
        gfx_round_fill(cv, x, y, w, h, 3, fill);
        gfx_round_rect(cv, x, y, w, h, 3, ring);
        if (s->on) { gfx_line_aa(cv, x + 3.5f, y + 8.5f, x + 6.5f, y + 12, 2, UI_ACCENT); gfx_line_aa(cv, x + 6.5f, y + 12, x + 12.5f, y + 4, 2, UI_ACCENT); }
        break;
    case C_RADIO:
        gfx_circle_fill(cv, x + 8, y + 8, 8, ring);
        gfx_circle_fill(cv, x + 8, y + 8, 7, fill);
        if (s->on) gfx_circle_fill(cv, x + 8, y + 8, 4, UI_ACCENT);
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
    int area = c->kind == C_AREA, base = 0;
    if (area) {
        int line = (ry - 5) / 18 + s->sy;
        if (line < 0) line = 0;
        for (int i = 0; i < line; i++) { const char *n = strchr(v + base, '\n'); if (!n) break; base = n + 1 - v; }
    }
    int le = line_end(v, base), x = rx - 6 + (area ? 0 : s->sx);
    if (c->kind == C_PASS) {                                              /* bullets: one char = 3 bytes of display, 1 of value */
        int n = le - base, w = gfx_text_width(&font_ui, "\xE2\x80\xA2"), k = (x + w / 2) / (w ? w : 1);
        return base + (k < 0 ? 0 : k > n ? n : k);
    }
    int k = base, acc = 0;
    while (k < le) {
        int nk = next_ch(v, k), cw = gfx_text_width_n(&font_ui, v + k, nk - k);
        if (x < acc + cw / 2) break;
        acc += cw; k = nk;
    }
    return k;
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
