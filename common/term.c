/* term.c - VT100/xterm-style terminal emulator core.  See term.h.
 * Freestanding: no floats, no libc beyond the prototypes below. */
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"
#include "font.h"
#include "term.h"

void *kmalloc(size_t n);
void kfree(void *p);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);

static void xfree(void *p) { if (p) kfree(p); }

#define SCROLLBACK 1000
#define MAXPAR 16
#define OSCMAX 96

/* colour encoding in cells: 0 = default, 0x01000000|i = palette i, 0xFF000000|rgb = truecolor.
 * Zero-filled memory is therefore a blank default cell. */
#define COL_PAL(i) (0x01000000u | (uint32_t)(i))
#define COL_RGB(r, g, b) RGB(r, g, b)

struct cell { uint32_t ch, fg, bg; uint8_t attr; };

struct saved { int cx, cy, wrap, g0gfx; uint32_t fg, bg; uint8_t attr; };

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET, ST_SKIP1 };

struct term {
    int cols, rows;
    struct cell *pri;                 /* ring of SCROLLBACK+rows lines */
    int cap, head, hist, view;        /* hist = lines of scrollback, view = lines scrolled up */
    struct cell *alt;
    int alt_on;
    int cx, cy, wrap, top, bot;
    uint32_t pfg, pbg; uint8_t pattr; /* pen */
    int autowrap, curvis, ins, g0gfx;
    struct saved sv, sv49;
    /* parser */
    int st, unr; uint32_t ucp;
    int par[MAXPAR], pi, pany, priv, csel;
    char osc[OSCMAX]; int olen;
    /* render state */
    int16_t *dmin, *dmax;
    int phase, focused;
    int lstyle, lcx, lcy;
    /* host interface */
    void (*reply)(void *, const char *, int); void *rctx;
    char title[64];
    int bell;
};

/* ---- palette ---- */

static const uint32_t pal16[16] = {   /* Tokyo Night-ish */
    RGB(0x2A, 0x2E, 0x42), RGB(0xF7, 0x76, 0x8E), RGB(0x9E, 0xCE, 0x6A), RGB(0xE0, 0xAF, 0x68),
    RGB(0x7A, 0xA2, 0xF7), RGB(0xBB, 0x9A, 0xF7), RGB(0x7D, 0xCF, 0xFF), RGB(0xA9, 0xB1, 0xD6),
    RGB(0x4E, 0x55, 0x75), RGB(0xFF, 0x89, 0x9D), RGB(0xB3, 0xE0, 0x7E), RGB(0xF5, 0xC2, 0x7C),
    RGB(0x8D, 0xB0, 0xFF), RGB(0xC7, 0xA9, 0xFF), RGB(0xA4, 0xDA, 0xFF), RGB(0xE6, 0xEA, 0xFA),
};

static color_t pal_rgb(int i) {
    if (i < 16) return pal16[i];
    if (i < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        i -= 16;
        return RGB(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
    }
    i = 8 + (i - 232) * 10;
    return RGB(i, i, i);
}

static color_t resolve(uint32_t c, int fg, int bold) {
    if (c == 0) return fg ? (bold ? 0xFFF2F4FBu : TERM_DEFAULT_FG) : TERM_DEFAULT_BG;
    if (c >> 24 == 0xFF) return c;
    int i = c & 0xFF;
    if (fg && bold && i < 8) i += 8;
    return pal_rgb(i);
}

/* ---- DEC special graphics (ESC ( 0), 0x6A..0x78 -> box drawing ---- */

static uint32_t dec_gfx(uint32_t c) {
    switch (c) {
    case 'j': return 0x2518; case 'k': return 0x2510; case 'l': return 0x250C; case 'm': return 0x2514;
    case 'n': return 0x253C; case 'q': return 0x2500; case 't': return 0x251C; case 'u': return 0x2524;
    case 'v': return 0x2534; case 'w': return 0x252C; case 'x': return 0x2502; case 'a': return 0x2592;
    case '`': return 0x25C6; case 'f': return 0xB0; case 'g': return 0xB1; case '~': return 0xB7;
    }
    return c;
}

/* ---- metrics ---- */

int term_cell_w(void) { return font_mono.g['M' - 32].adv; }
int term_cell_h(void) { return font_mono.height; }

void term_size_for_pixels(int pw, int ph, int *cols, int *rows) {
    int c = pw / term_cell_w(), r = ph / term_cell_h();
    *cols = c < 1 ? 1 : c;
    *rows = r < 1 ? 1 : r;
}

/* ---- storage helpers ---- */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static struct cell *lnp(struct term *t, int i) { return t->pri + (size_t)((t->head + i) % t->cap) * t->cols; }
static struct cell *rowp(struct term *t, int r) { return t->alt_on ? t->alt + (size_t)r * t->cols : lnp(t, t->hist + r); }

static void mark(struct term *t, int r, int c0, int c1) {
    if (r < 0 || r >= t->rows) return;
    if (c0 < t->dmin[r]) t->dmin[r] = c0;
    if (c1 > t->dmax[r]) t->dmax[r] = c1;
}
static void mark_rows(struct term *t, int r0, int r1) { for (int r = r0; r <= r1; r++) mark(t, r, 0, t->cols - 1); }
static void mark_all(struct term *t) { mark_rows(t, 0, t->rows - 1); }

static void blank(struct term *t, struct cell *p, int n) {
    for (int i = 0; i < n; i++) { p[i].ch = 0; p[i].fg = 0; p[i].bg = t->pbg; p[i].attr = 0; }
}

static void blank_row(struct term *t, int r) { blank(t, rowp(t, r), t->cols); mark(t, r, 0, t->cols - 1); }

/* ---- scrolling ---- */

static void scroll_up(struct term *t, int top, int bot, int n) {
    n = imin(n, bot - top + 1);
    if (n <= 0) return;
    if (!t->alt_on && top == 0 && bot == t->rows - 1) {        /* ring: old top line goes to scrollback */
        while (n--) {
            if (t->hist < SCROLLBACK) t->hist++; else t->head = (t->head + 1) % t->cap;
            blank_row(t, t->rows - 1);
        }
        mark_all(t);
        return;
    }
    for (int r = top; r + n <= bot; r++) memcpy(rowp(t, r), rowp(t, r + n), (size_t)t->cols * sizeof(struct cell));
    for (int r = bot - n + 1; r <= bot; r++) blank_row(t, r);
    mark_rows(t, top, bot);
}

static void scroll_down(struct term *t, int top, int bot, int n) {
    n = imin(n, bot - top + 1);
    if (n <= 0) return;
    for (int r = bot; r - n >= top; r--) memcpy(rowp(t, r), rowp(t, r - n), (size_t)t->cols * sizeof(struct cell));
    for (int r = top; r < top + n; r++) blank_row(t, r);
    mark_rows(t, top, bot);
}

static void index_down(struct term *t) {
    t->wrap = 0;
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void index_up(struct term *t) {
    t->wrap = 0;
    if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0) t->cy--;
}

/* ---- character output ---- */

static void put_char(struct term *t, uint32_t cp) {
    if (t->wrap && t->autowrap) { t->cx = 0; index_down(t); }
    t->wrap = 0;
    struct cell *row = rowp(t, t->cy);
    if (t->ins && t->cx < t->cols - 1) {
        memmove(&row[t->cx + 1], &row[t->cx], (size_t)(t->cols - 1 - t->cx) * sizeof *row);
        mark(t, t->cy, t->cx, t->cols - 1);
    }
    struct cell *e = &row[t->cx];
    e->ch = cp; e->fg = t->pfg; e->bg = t->pbg; e->attr = t->pattr;
    mark(t, t->cy, t->cx, t->cx);
    if (t->cx == t->cols - 1) t->wrap = 1; else t->cx++;
}

static void erase_cells(struct term *t, int r, int c0, int c1) {   /* inclusive */
    if (c0 < 0) c0 = 0;
    if (c1 >= t->cols) c1 = t->cols - 1;
    if (c0 > c1) return;
    blank(t, rowp(t, r) + c0, c1 - c0 + 1);
    mark(t, r, c0, c1);
}

static void reply_str(struct term *t, const char *s) {
    int n = 0;
    while (s[n]) n++;
    if (t->reply) t->reply(t->rctx, s, n);
}

static char *put_num(char *p, int v) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

/* ---- modes / state ---- */

static void save_cursor(struct term *t, struct saved *s) {
    s->cx = t->cx; s->cy = t->cy; s->wrap = t->wrap; s->g0gfx = t->g0gfx;
    s->fg = t->pfg; s->bg = t->pbg; s->attr = t->pattr;
}

static void restore_cursor(struct term *t, const struct saved *s) {
    t->cx = imin(s->cx, t->cols - 1); t->cy = imin(s->cy, t->rows - 1); t->wrap = s->wrap; t->g0gfx = s->g0gfx;
    t->pfg = s->fg; t->pbg = s->bg; t->pattr = s->attr;
}

static void enter_alt(struct term *t, int save) {
    if (t->alt_on) return;
    if (save) save_cursor(t, &t->sv49);
    t->alt_on = 1;
    t->top = 0; t->bot = t->rows - 1;
    uint32_t bg = t->pbg; t->pbg = 0;
    for (int r = 0; r < t->rows; r++) blank_row(t, r);
    t->pbg = bg;
    mark_all(t);
}

static void leave_alt(struct term *t, int restore) {
    if (!t->alt_on) return;
    t->alt_on = 0;
    t->top = 0; t->bot = t->rows - 1;
    if (restore) restore_cursor(t, &t->sv49);
    mark_all(t);
}

static void reset(struct term *t) {
    leave_alt(t, 0);
    t->hist = 0; t->head = 0; t->view = 0;
    t->pfg = t->pbg = 0; t->pattr = 0;
    t->top = 0; t->bot = t->rows - 1;
    t->autowrap = 1; t->curvis = 1; t->ins = 0; t->g0gfx = 0;
    for (int r = 0; r < t->rows; r++) blank_row(t, r);
    t->cx = t->cy = t->wrap = 0;
    memset(&t->sv, 0, sizeof t->sv);
}

static void set_mode(struct term *t, int priv, int p, int on) {
    if (!priv) {
        if (p == 4) t->ins = on;
        return;
    }
    switch (p) {
    case 7: t->autowrap = on; break;
    case 25: t->curvis = on; break;
    case 47: case 1047: if (on) enter_alt(t, 0); else leave_alt(t, 0); break;
    case 1049: if (on) enter_alt(t, 1); else leave_alt(t, 1); break;
    default: break;   /* 2004 bracketed paste, mouse, app cursor keys: accepted, ignored */
    }
}

static void sgr(struct term *t) {
    int n = t->pany ? t->pi + 1 : 1;
    for (int i = 0; i < n; i++) {
        int p = t->par[i];
        if (p == 0) { t->pfg = t->pbg = 0; t->pattr = 0; }
        else if (p == 1) t->pattr |= TERM_A_BOLD;
        else if (p == 2) t->pattr |= TERM_A_DIM;
        else if (p == 3) t->pattr |= TERM_A_ITALIC;
        else if (p == 4) t->pattr |= TERM_A_UNDER;
        else if (p == 7) t->pattr |= TERM_A_INVERSE;
        else if (p == 9) t->pattr |= TERM_A_STRIKE;
        else if (p == 22) t->pattr &= ~(TERM_A_BOLD | TERM_A_DIM);
        else if (p == 23) t->pattr &= ~TERM_A_ITALIC;
        else if (p == 24) t->pattr &= ~TERM_A_UNDER;
        else if (p == 27) t->pattr &= ~TERM_A_INVERSE;
        else if (p == 29) t->pattr &= ~TERM_A_STRIKE;
        else if (p >= 30 && p <= 37) t->pfg = COL_PAL(p - 30);
        else if (p >= 40 && p <= 47) t->pbg = COL_PAL(p - 40);
        else if (p >= 90 && p <= 97) t->pfg = COL_PAL(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->pbg = COL_PAL(p - 100 + 8);
        else if (p == 39) t->pfg = 0;
        else if (p == 49) t->pbg = 0;
        else if (p == 38 || p == 48) {
            uint32_t col = 0; int ok = 0;
            if (i + 2 < n && t->par[i + 1] == 5) { col = COL_PAL(t->par[i + 2] & 0xFF); ok = 1; i += 2; }
            else if (i + 4 < n && t->par[i + 1] == 2) {
                col = COL_RGB(t->par[i + 2] & 0xFF, t->par[i + 3] & 0xFF, t->par[i + 4] & 0xFF); ok = 1; i += 4;
            } else i = n;   /* malformed: swallow the rest */
            if (ok) { if (p == 38) t->pfg = col; else t->pbg = col; }
        }
    }
}

/* ---- CSI ---- */

static int arg(struct term *t, int i, int def) {
    int n = t->pany ? t->pi + 1 : 0;
    return i < n && t->par[i] > 0 ? t->par[i] : def;
}

static void csi(struct term *t, int f) {
    int n = arg(t, 0, 1), np = t->pany ? t->pi + 1 : 0;
    for (const char *m = "ABCDEFG`dHfLMr"; *m; m++) if (*m == f && !t->priv) t->wrap = 0;   /* cursor moves cancel pending wrap */
    if (t->priv == '?') {
        if (f == 'h' || f == 'l') for (int i = 0; i < np; i++) set_mode(t, 1, t->par[i], f == 'h');
        return;
    }
    if (t->priv == '>') {
        if (f == 'c') reply_str(t, "\x1b[>0;0;0c");
        return;
    }
    if (t->priv) return;
    switch (f) {
    case 'A': t->cy = imax(t->cy - n, t->cy >= t->top ? t->top : 0); break;
    case 'B': t->cy = imin(t->cy + n, t->cy <= t->bot ? t->bot : t->rows - 1); break;
    case 'C': t->cx = imin(t->cx + n, t->cols - 1); break;
    case 'D': t->cx = imax(t->cx - n, 0); break;
    case 'E': t->cy = imin(t->cy + n, t->cy <= t->bot ? t->bot : t->rows - 1); t->cx = 0; break;
    case 'F': t->cy = imax(t->cy - n, t->cy >= t->top ? t->top : 0); t->cx = 0; break;
    case 'G': case '`': t->cx = imin(n - 1, t->cols - 1); break;
    case 'd': t->cy = imin(n - 1, t->rows - 1); break;
    case 'H': case 'f':
        t->cy = imin(arg(t, 0, 1) - 1, t->rows - 1);
        t->cx = imin(arg(t, 1, 1) - 1, t->cols - 1);
        break;
    case 'J': {
        int m = np ? t->par[0] : 0;
        if (m == 0) { erase_cells(t, t->cy, t->cx, t->cols - 1); for (int r = t->cy + 1; r < t->rows; r++) erase_cells(t, r, 0, t->cols - 1); }
        else if (m == 1) { for (int r = 0; r < t->cy; r++) erase_cells(t, r, 0, t->cols - 1); erase_cells(t, t->cy, 0, t->cx); }
        else if (m == 2) { for (int r = 0; r < t->rows; r++) erase_cells(t, r, 0, t->cols - 1); }
        else if (m == 3 && !t->alt_on) { t->head = (t->head + t->hist) % t->cap; t->hist = 0; t->view = 0; mark_all(t); }
        break;
    }
    case 'K': {
        int m = np ? t->par[0] : 0;
        if (m == 0) erase_cells(t, t->cy, t->cx, t->cols - 1);
        else if (m == 1) erase_cells(t, t->cy, 0, t->cx);
        else if (m == 2) erase_cells(t, t->cy, 0, t->cols - 1);
        break;
    }
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) { scroll_down(t, t->cy, t->bot, n); t->cx = 0; } break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) { scroll_up(t, t->cy, t->bot, n); t->cx = 0; } break;
    case 'P': {
        struct cell *row = rowp(t, t->cy);
        n = imin(n, t->cols - t->cx);
        memmove(&row[t->cx], &row[t->cx + n], (size_t)(t->cols - t->cx - n) * sizeof *row);
        blank(t, &row[t->cols - n], n);
        mark(t, t->cy, t->cx, t->cols - 1);
        break;
    }
    case '@': {
        struct cell *row = rowp(t, t->cy);
        n = imin(n, t->cols - t->cx);
        memmove(&row[t->cx + n], &row[t->cx], (size_t)(t->cols - t->cx - n) * sizeof *row);
        blank(t, &row[t->cx], n);
        mark(t, t->cy, t->cx, t->cols - 1);
        break;
    }
    case 'X': erase_cells(t, t->cy, t->cx, t->cx + n - 1); break;
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'r': {
        int tp = arg(t, 0, 1) - 1, bt = arg(t, 1, t->rows) - 1;
        if (bt >= t->rows) bt = t->rows - 1;
        if (tp < bt) { t->top = tp; t->bot = bt; } else { t->top = 0; t->bot = t->rows - 1; }
        t->cx = t->cy = 0;
        break;
    }
    case 's': save_cursor(t, &t->sv); break;
    case 'u': restore_cursor(t, &t->sv); break;
    case 'm': sgr(t); break;
    case 'h': case 'l': for (int i = 0; i < np; i++) set_mode(t, 0, t->par[i], f == 'h'); break;
    case 'n': {
        int m = np ? t->par[0] : 0;
        if (m == 5) reply_str(t, "\x1b[0n");
        else if (m == 6) {
            char b[32], *p = b;
            *p++ = 27; *p++ = '[';
            p = put_num(p, t->cy + 1); *p++ = ';'; p = put_num(p, t->cx + 1); *p++ = 'R'; *p = 0;
            reply_str(t, b);
        }
        break;
    }
    case 'c': if ((np ? t->par[0] : 0) == 0) reply_str(t, "\x1b[?1;2c"); break;
    default: break;
    }
}

static void osc_done(struct term *t) {
    t->osc[t->olen] = 0;
    int num = 0, i = 0;
    while (t->osc[i] >= '0' && t->osc[i] <= '9') num = num * 10 + t->osc[i++] - '0';
    if (t->osc[i] != ';' || (num != 0 && num != 2)) return;
    i++;
    int j = 0;
    while (t->osc[i] && j < (int)sizeof t->title - 1) t->title[j++] = t->osc[i++];
    t->title[j] = 0;
}

/* ---- parser ---- */

static void ctrl(struct term *t, uint8_t b) {
    switch (b) {
    case 7: t->bell = 1; break;
    case 8: t->wrap = 0; if (t->cx > 0) t->cx--; break;
    case 9: t->wrap = 0; t->cx = imin((t->cx / 8 + 1) * 8, t->cols - 1); break;
    case 10: case 11: case 12: index_down(t); break;
    case 13: t->wrap = 0; t->cx = 0; break;
    case 27: t->st = ST_ESC; break;
    default: break;
    }
}

static void feed(struct term *t, uint8_t b);

static void feed_ground(struct term *t, uint8_t b) {
    if (t->unr) {
        if ((b & 0xC0) == 0x80) {
            t->ucp = (t->ucp << 6) | (b & 0x3F);
            if (--t->unr == 0) put_char(t, t->ucp > 0x10FFFF ? 0xFFFD : t->ucp);
            return;
        }
        t->unr = 0;
        put_char(t, 0xFFFD);        /* truncated sequence: reprocess this byte */
    }
    if (b < 0x20) ctrl(t, b);
    else if (b == 0x7F) return;
    else if (b < 0x80) put_char(t, t->g0gfx ? dec_gfx(b) : b);
    else if (b >= 0xC2 && b <= 0xDF) { t->unr = 1; t->ucp = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) { t->unr = 2; t->ucp = b & 0x0F; }
    else if (b >= 0xF0 && b <= 0xF4) { t->unr = 3; t->ucp = b & 0x07; }
    else put_char(t, 0xFFFD);
}

static void feed(struct term *t, uint8_t b) {
    if (b == 0x18 || b == 0x1A) { t->st = ST_GROUND; t->unr = 0; return; }   /* CAN/SUB abort */
    switch (t->st) {
    case ST_GROUND:
        feed_ground(t, b);
        break;
    case ST_ESC:
        if (b < 0x20) { ctrl(t, b); break; }
        t->st = ST_GROUND;
        switch (b) {
        case '[':
            t->st = ST_CSI; t->pi = 0; t->pany = 0; t->priv = 0;
            memset(t->par, 0, sizeof t->par);
            break;
        case ']': t->st = ST_OSC; t->olen = 0; break;
        case '(': case ')': case '*': case '+': t->st = ST_CHARSET; t->csel = b; break;
        case '#': case '%': case ' ': t->st = ST_SKIP1; break;
        case '7': save_cursor(t, &t->sv); break;
        case '8': restore_cursor(t, &t->sv); break;
        case 'M': index_up(t); break;
        case 'D': index_down(t); break;
        case 'E': t->cx = 0; index_down(t); break;
        case 'c': reset(t); break;
        default: break;
        }
        break;
    case ST_CHARSET:
        if (t->csel == '(') t->g0gfx = b == '0';
        t->st = ST_GROUND;
        break;
    case ST_SKIP1:
        t->st = ST_GROUND;
        break;
    case ST_CSI:
        if (b < 0x20) ctrl(t, b);
        else if (b >= '0' && b <= '9') {
            t->pany = 1;
            if (t->par[t->pi] < 65535) t->par[t->pi] = t->par[t->pi] * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            t->pany = 1;
            if (t->pi < MAXPAR - 1) t->pi++;
        } else if (b >= 0x3C && b <= 0x3F) t->priv = b;
        else if (b <= 0x2F) { /* intermediate bytes (e.g. ' q'): ignored */ }
        else if (b >= 0x40 && b <= 0x7E) { t->st = ST_GROUND; csi(t, b); }
        else t->st = ST_GROUND;
        break;
    case ST_OSC:
        if (b == 7) { osc_done(t); t->st = ST_GROUND; }
        else if (b == 27) t->st = ST_OSC_ESC;
        else if (t->olen < OSCMAX - 1) t->osc[t->olen++] = b;
        break;
    case ST_OSC_ESC:
        osc_done(t);
        t->st = ST_GROUND;
        if (b != '\\') { t->st = ST_ESC; feed(t, b); }
        break;
    }
}

void term_write(struct term *t, const char *s, int n) {
    if (n > 0 && t->view) { t->view = 0; mark_all(t); }
    for (int i = 0; i < n; i++) feed(t, (uint8_t)s[i]);
}

/* ---- lifetime ---- */

static int alloc_dirty(struct term *t) {
    xfree(t->dmin); xfree(t->dmax);
    t->dmin = kmalloc(sizeof(int16_t) * t->rows);
    t->dmax = kmalloc(sizeof(int16_t) * t->rows);
    if (!t->dmin || !t->dmax) return 0;
    for (int r = 0; r < t->rows; r++) { t->dmin[r] = 0; t->dmax[r] = t->cols - 1; }
    return 1;
}

struct term *term_create(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    struct term *t = kmalloc(sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    t->cols = cols; t->rows = rows;
    t->cap = SCROLLBACK + rows;
    t->pri = kmalloc((size_t)t->cap * cols * sizeof(struct cell));
    t->alt = kmalloc((size_t)rows * cols * sizeof(struct cell));
    if (!t->pri || !t->alt || !alloc_dirty(t)) { term_destroy(t); return NULL; }
    memset(t->pri, 0, (size_t)t->cap * cols * sizeof(struct cell));
    memset(t->alt, 0, (size_t)rows * cols * sizeof(struct cell));
    t->bot = rows - 1;
    t->autowrap = 1; t->curvis = 1; t->focused = 1; t->phase = 1;
    return t;
}

void term_destroy(struct term *t) {
    if (!t) return;
    xfree(t->pri); xfree(t->alt); xfree(t->dmin); xfree(t->dmax);
    kfree(t);
}

void term_resize(struct term *t, int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;
    int oc = t->cols, orows = t->rows, cc = imin(cols, oc);
    int cap = SCROLLBACK + rows;
    struct cell *np = kmalloc((size_t)cap * cols * sizeof *np);
    struct cell *na = kmalloc((size_t)rows * cols * sizeof *na);
    int16_t *dn = kmalloc(sizeof(int16_t) * rows), *dx = kmalloc(sizeof(int16_t) * rows);
    if (!np || !na || !dn || !dx) { xfree(np); xfree(na); xfree(dn); xfree(dx); return; }
    memset(np, 0, (size_t)cap * cols * sizeof *np);
    memset(na, 0, (size_t)rows * cols * sizeof *na);

    /* primary: keep the cursor line visible, pushing lines into history if the screen shrank */
    int *pcy = t->alt_on ? &t->sv49.cy : &t->cy;
    int nh = t->hist, ny = *pcy;
    if (ny >= rows) { nh += ny - rows + 1; ny = rows - 1; }
    int drop = nh > SCROLLBACK ? nh - SCROLLBACK : 0;
    nh -= drop;
    for (int i = drop; i < t->hist + orows && i - drop < nh + rows; i++)
        memcpy(np + (size_t)(i - drop) * cols, lnp(t, i), (size_t)cc * sizeof *np);
    for (int r = 0; r < imin(rows, orows); r++)
        memcpy(na + (size_t)r * cols, t->alt + (size_t)r * oc, (size_t)cc * sizeof *na);

    kfree(t->pri); kfree(t->alt); kfree(t->dmin); kfree(t->dmax);
    t->pri = np; t->alt = na; t->dmin = dn; t->dmax = dx;
    t->cols = cols; t->rows = rows; t->cap = cap;
    t->head = 0; t->hist = nh; t->view = 0;
    if (t->alt_on) { t->sv49.cy = ny; t->cy = imin(t->cy, rows - 1); } else t->cy = ny;
    t->cx = imin(t->cx, cols - 1);
    t->sv.cx = imin(t->sv.cx, cols - 1); t->sv.cy = imin(t->sv.cy, rows - 1);
    t->sv49.cx = imin(t->sv49.cx, cols - 1);
    t->wrap = 0; t->top = 0; t->bot = rows - 1;
    t->lstyle = 0;
    for (int r = 0; r < rows; r++) { t->dmin[r] = cols; t->dmax[r] = -1; }
    mark_all(t);
}

/* ---- accessors ---- */

void term_set_reply(struct term *t, void (*fn)(void *, const char *, int), void *ctx) { t->reply = fn; t->rctx = ctx; }
int term_cols(struct term *t) { return t->cols; }
int term_rows(struct term *t) { return t->rows; }
const char *term_title(struct term *t) { return t->title; }
int term_take_bell(struct term *t) { int b = t->bell; t->bell = 0; return b; }
void term_set_cursor_blink_phase(struct term *t, int on) { t->phase = on; }
void term_set_focused(struct term *t, int f) { t->focused = f; }
void term_cursor(struct term *t, int *col, int *row) { *col = t->cx; *row = t->cy; }
int term_scrollback_lines(struct term *t) { return t->hist; }
int term_view_offset(struct term *t) { return t->view; }
int term_on_alt_screen(struct term *t) { return t->alt_on; }

void term_scroll_view(struct term *t, int lines) {
    int v = lines == 0 || t->alt_on ? 0 : t->view + lines;
    v = imax(0, imin(v, t->hist));
    if (v != t->view) { t->view = v; mark_all(t); }
}

static const struct cell *cell_at(struct term *t, int col, int row) {
    static const struct cell empty;
    if (col < 0 || col >= t->cols || row >= t->rows) return &empty;
    if (row < 0) {
        if (t->alt_on || t->hist + row < 0) return &empty;
        return lnp(t, t->hist + row) + col;
    }
    return rowp(t, row) + col;
}

uint32_t term_cell_char(struct term *t, int col, int row) { uint32_t c = cell_at(t, col, row)->ch; return c ? c : ' '; }
uint32_t term_cell_fg(struct term *t, int col, int row) { return resolve(cell_at(t, col, row)->fg, 1, 0); }
uint32_t term_cell_bg(struct term *t, int col, int row) { return resolve(cell_at(t, col, row)->bg, 0, 0); }
int term_cell_attr(struct term *t, int col, int row) { return cell_at(t, col, row)->attr; }

/* ---- rendering ---- */

static const uint16_t box_tab[128] = {   /* L | R<<4 | U<<8 | D<<12; 1 light 2 heavy 3 double */
    0x0011, 0x0022, 0x1100, 0x2200, 0x0011, 0x0022, 0x1100, 0x2200,
    0x0011, 0x0022, 0x1100, 0x2200, 0x1010, 0x1020, 0x2010, 0x2020,
    0x1001, 0x1002, 0x2001, 0x2002, 0x0110, 0x0120, 0x0210, 0x0220,
    0x0101, 0x0102, 0x0201, 0x0202, 0x1110, 0x1120, 0x1210, 0x2110,
    0x2210, 0x1220, 0x2120, 0x2220, 0x1101, 0x1102, 0x1201, 0x2101,
    0x2201, 0x1202, 0x2102, 0x2202, 0x1011, 0x1012, 0x1021, 0x1022,
    0x2011, 0x2012, 0x2021, 0x2022, 0x0111, 0x0112, 0x0121, 0x0122,
    0x0211, 0x0212, 0x0221, 0x0222, 0x1111, 0x1112, 0x1121, 0x1122,
    0x1211, 0x2111, 0x2211, 0x1212, 0x1221, 0x2112, 0x2121, 0x1222,
    0x2122, 0x2212, 0x2221, 0x2222, 0x0011, 0x0022, 0x1100, 0x2200,
    0x0033, 0x3300, 0x1030, 0x3010, 0x3030, 0x1003, 0x3001, 0x3003,
    0x0130, 0x0310, 0x0330, 0x0103, 0x0301, 0x0303, 0x1130, 0x3310,
    0x3330, 0x1103, 0x3301, 0x3303, 0x1033, 0x3011, 0x3033, 0x0133,
    0x0311, 0x0333, 0x1133, 0x3311, 0x3333, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0100, 0x0010, 0x1000,
    0x0002, 0x0200, 0x0020, 0x2000, 0x0021, 0x2100, 0x0012, 0x1200,
};

static const uint8_t arc_tab[5][5] = {   /* r=4 quarter arc coverage, corner pixel (cx,cy) at [0][0] */
    {0, 31, 255, 255, 255},
    {31, 255, 255, 95, 0},
    {255, 255, 0, 0, 0},
    {255, 95, 0, 0, 0},
    {255, 0, 0, 0, 0},
};

static void box_axis(struct canvas *c, int x, int y, int vert, int len, int ac, int pc,
                     const int *a, const int *p, color_t col) {
    int pm = imax(p[0], p[1]);
    int elo = ac - (pm == 3 ? 2 : pm == 2), ehi = ac + (pm == 3 ? 2 : pm == 2);
    for (int k = 0; k < 2; k++) {
        int w = a[k];
        if (!w) continue;
        int ns = w == 3 ? 2 : 1;
        for (int s = 0; s < ns; s++) {
            int u0, u1;                                    /* across-axis extent of this stroke */
            if (w == 3) u0 = u1 = pc + (s ? 2 : -2);
            else { u0 = pc - (w == 2); u1 = pc + (w == 2); }
            int e;
            if (pm == 3) {
                int near = k ? ac + 2 : ac - 2, far = k ? ac - 2 : ac + 2;
                if (w == 3) e = p[s] == 3 ? near : far;
                else if (a[1 - k]) e = far;                /* straight through a double crossing */
                else e = p[0] && p[1] ? near : far;
            } else e = k ? elo : ehi;
            int a0 = k ? e : 0, a1 = k ? len - 1 : e;
            if (vert) gfx_fill(c, x + u0, y + a0, u1 - u0 + 1, a1 - a0 + 1, col);
            else gfx_fill(c, x + a0, y + u0, a1 - a0 + 1, u1 - u0 + 1, col);
        }
    }
}

/* box drawing U+2500..257F and blocks U+2580..259F, drawn to fit a cw x ch cell */
static void draw_box(struct canvas *c, int x, int y, int cw, int ch, uint32_t cp, color_t fg, color_t bg) {
    int cx = cw / 2, cy = ch / 2;
    if (cp >= 0x2580) {
        int xm = (cw + 1) / 2, ym = ch / 2;
        if (cp == 0x2580) gfx_fill(c, x, y, cw, ym, fg);
        else if (cp <= 0x2588) { int h = (ch * (cp - 0x2580) + 4) / 8; gfx_fill(c, x, y + ch - h, cw, h, fg); }
        else if (cp <= 0x258F) gfx_fill(c, x, y, (cw * (0x2590 - cp) + 4) / 8, ch, fg);
        else if (cp == 0x2590) gfx_fill(c, x + xm, y, cw - xm, ch, fg);
        else if (cp <= 0x2593) gfx_fill(c, x, y, cw, ch, mix(bg, fg, (cp - 0x2590) * 64));
        else if (cp == 0x2594) gfx_fill(c, x, y, cw, (ch + 4) / 8, fg);
        else if (cp == 0x2595) gfx_fill(c, x + cw - (cw + 4) / 8, y, (cw + 4) / 8, ch, fg);
        else if (cp <= 0x259F) {
            static const uint8_t q[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };   /* UL=1 UR=2 LL=4 LR=8 */
            int m = q[cp - 0x2596];
            if (m & 1) gfx_fill(c, x, y, xm, ym, fg);
            if (m & 2) gfx_fill(c, x + xm, y, cw - xm, ym, fg);
            if (m & 4) gfx_fill(c, x, y + ym, xm, ch - ym, fg);
            if (m & 8) gfx_fill(c, x + xm, y + ym, cw - xm, ch - ym, fg);
        }
        return;
    }
    if (cp >= 0x256D && cp <= 0x2570) {                    /* rounded corners; arc_tab assumes cw>=9, ch>=20 */
        int fx = cp == 0x256E || cp == 0x256F, fy = cp == 0x256F || cp == 0x2570;
        for (int j = 0; j < 5; j++)
            for (int i = 0; i < 5; i++) {
                int a = arc_tab[j][i];
                if (a) gfx_pixel(c, x + (fx ? cx - i : cx + i), y + (fy ? cy - j : cy + j), (fg & 0xFFFFFF) | (uint32_t)a << 24);
            }
        if (fy) gfx_fill(c, x + cx, y, 1, cy - 4, fg); else gfx_fill(c, x + cx, y + cy + 5, 1, ch - cy - 5, fg);
        return;
    }
    if (cp >= 0x2571 && cp <= 0x2573) {
        if (cp != 0x2572) gfx_line(c, x, y + ch - 1, x + cw - 1, y, fg);
        if (cp != 0x2571) gfx_line(c, x, y, x + cw - 1, y + ch - 1, fg);
        return;
    }
    unsigned e = box_tab[cp - 0x2500];
    int h[2] = { e & 15, (e >> 4) & 15 }, v[2] = { (e >> 8) & 15, (e >> 12) & 15 };
    box_axis(c, x, y, 0, cw, cx, cy, h, v, fg);
    box_axis(c, x, y, 1, ch, cy, cx, v, h, fg);
}

/* cur: 0 none, 1 block, 2 hollow */
static void draw_cell(struct canvas *c, int x, int y, int cw, int ch, const struct cell *e, int cur) {
    int bold = e->attr & TERM_A_BOLD;
    color_t fg = resolve(e->fg, 1, bold), bg = resolve(e->bg, 0, 0);
    if (e->attr & TERM_A_INVERSE) { color_t tmp = fg; fg = bg; bg = tmp; }
    if (e->attr & TERM_A_DIM) fg = mix(fg, bg, 90);
    color_t curcol = TERM_DEFAULT_FG;
    if (cur == 1) { color_t tmp = bg; bg = curcol; fg = tmp; }
    gfx_fill(c, x, y, cw, ch, bg);
    uint32_t cp = e->ch;
    if (cp >= 0x2500 && cp <= 0x259F) draw_box(c, x, y, cw, ch, cp, fg, bg);
    else if (cp > ' ') {
        /* ponytail: italic is not synthesised; everything outside ASCII becomes '?' */
        gfx_char(c, bold ? &font_mono_bold : &font_mono, x, y, cp < 127 ? (char)cp : '?', fg);
    }
    if (e->attr & TERM_A_UNDER) gfx_fill(c, x, y + ch - 3, cw, 1, fg);
    if (e->attr & TERM_A_STRIKE) gfx_fill(c, x, y + ch / 2, cw, 1, fg);
    if (cur == 2) gfx_rect(c, x, y, cw, ch, curcol);
}

void term_render(struct term *t, struct canvas *c, int x, int y, int full, struct rect *damage) {
    int cw = term_cell_w(), ch = term_cell_h();
    int style = t->view == 0 && t->curvis ? (t->focused ? (t->phase ? 1 : 0) : 2) : 0;
    if (style != t->lstyle || t->cx != t->lcx || t->cy != t->lcy) {
        if (t->lstyle) mark(t, t->lcy, t->lcx, t->lcx);
        if (style) mark(t, t->cy, t->cx, t->cx);
        t->lstyle = style; t->lcx = t->cx; t->lcy = t->cy;
    }
    int minc = t->cols, maxc = -1, minr = t->rows, maxr = -1;
    for (int r = 0; r < t->rows; r++) {
        int a = full ? 0 : t->dmin[r], b = full ? t->cols - 1 : t->dmax[r];
        t->dmin[r] = t->cols; t->dmax[r] = -1;
        if (a > b) continue;
        const struct cell *row = t->alt_on ? rowp(t, r) : lnp(t, t->hist - t->view + r);
        for (int col = a; col <= b; col++)
            draw_cell(c, x + col * cw, y + r * ch, cw, ch, &row[col], style && col == t->cx && r == t->cy ? style : 0);
        minc = imin(minc, a); maxc = imax(maxc, b); minr = imin(minr, r); maxr = imax(maxr, r);
    }
    if (maxc < 0) { damage->x = x; damage->y = y; damage->w = damage->h = 0; return; }
    damage->x = x + minc * cw; damage->y = y + minr * ch;
    damage->w = (maxc - minc + 1) * cw; damage->h = (maxr - minr + 1) * ch;
}
