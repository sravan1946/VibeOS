/* calc - calculator with expression history */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

#define EXPR_MAX 96

static char expr[EXPR_MAX];        /* what the user typed, using * and / */
static char hist[EXPR_MAX + 8];    /* line above the display */
static char result[40];            /* shown after '=' */
static bool done, err, dirty;

/* ---- evaluation: + - * / with precedence, unary minus, postfix % ---- */
static const char *ep;
static bool eerr;
static double parse_sum(void);

static double parse_unary(void) {
    if (*ep == '-') { ep++; return -parse_unary(); }
    if (*ep == '+') { ep++; return parse_unary(); }
    char *end;
    double v = strtod(ep, &end);
    if (end == ep) { eerr = true; return 0; }
    ep = end;
    while (*ep == '%') { v /= 100; ep++; }
    return v;
}

static double parse_term(void) {
    double v = parse_unary();
    while (*ep == '*' || *ep == '/') {
        char op = *ep++;
        double r = parse_unary();
        if (op == '*') v *= r;
        else if (r == 0) eerr = true;
        else v /= r;
    }
    return v;
}

static double parse_sum(void) {
    double v = parse_term();
    while (*ep == '+' || *ep == '-') {
        char op = *ep++;
        double r = parse_term();
        v = op == '+' ? v + r : v - r;
    }
    return v;
}

static void fmt_num(char *out, int n, double v) {
    if (isnan(v) || isinf(v)) { snprintf(out, n, "Error"); return; }
    if (fabs(v) < 1e-12) v = 0;
    snprintf(out, n, "%.10g", v);
}

/* drop trailing operators so "5+" evaluates as 5 */
static void evaluate(void) {
    if (!expr[0]) return;
    char tmp[EXPR_MAX];
    strcpy(tmp, expr);
    int n = strlen(tmp);
    while (n && strchr("+-*/.", tmp[n - 1])) tmp[--n] = 0;
    if (!n) return;
    ep = tmp;
    eerr = false;
    double v = parse_sum();
    if (*ep) eerr = true;
    snprintf(hist, sizeof hist, "%s =", tmp);
    err = eerr || isnan(v) || isinf(v);
    if (err) snprintf(result, sizeof result, "Error");
    else fmt_num(result, sizeof result, v);
    done = true;
}

static void clear_all(void) { expr[0] = hist[0] = result[0] = 0; done = err = false; }

/* start of the number currently being typed */
static int last_num(void) {
    int i = strlen(expr);
    while (i > 0 && (isdigit((unsigned char)expr[i - 1]) || expr[i - 1] == '.')) i--;
    return i;
}

static void put(char ch) {
    if (done) {                       /* continue from the result, or start fresh on a digit */
        bool op = strchr("+-*/%", ch) != 0;
        if (op && !err) snprintf(expr, sizeof expr, "%s", result);
        else expr[0] = 0;
        done = err = false;
        hist[0] = 0;
    }
    int n = strlen(expr);
    if (n >= EXPR_MAX - 2) return;
    if (strchr("+-*/", ch)) {
        if (!n) { if (ch != '-') return; }
        else if (strchr("+-*/", expr[n - 1])) {            /* replace the previous operator */
            if (n == 1) { if (ch != '-') return; }
            expr[n - 1] = ch;
            return;
        }
    } else if (ch == '%') {
        if (!n || !(isdigit((unsigned char)expr[n - 1]) || expr[n - 1] == '%')) return;
    } else if (ch == '.') {
        if (strchr(expr + last_num(), '.')) return;
        if (last_num() == n) { expr[n++] = '0'; }
    } else if (n && expr[n - 1] == '%') {
        return;                                            /* digit right after % makes no sense */
    }
    expr[n] = ch;
    expr[n + 1] = 0;
}

static void backspace(void) {
    if (done) { clear_all(); return; }
    int n = strlen(expr);
    if (n) expr[n - 1] = 0;
}

static void negate(void) {
    if (done && !err) { snprintf(expr, sizeof expr, "%s", result); done = false; hist[0] = 0; }
    if (done) return;
    int i = last_num(), n = strlen(expr);
    if (i == n) return;
    if (i > 0 && expr[i - 1] == '-' && (i == 1 || strchr("+-*/", expr[i - 2]))) {   /* remove unary minus */
        memmove(expr + i - 1, expr + i, n - i + 1);
    } else if (n < EXPR_MAX - 2) {
        memmove(expr + i + 1, expr + i, n - i + 1);
        expr[i] = '-';
    }
}

/* operator glyphs drawn from primitives (the baked fonts are ASCII only) */
static void icon(struct canvas *c, int kind, int cx, int cy, color_t col) {
    switch (kind) {
    case 0:  /* plus */
        gfx_line_aa(c, cx - 8, cy, cx + 8, cy, 2.6f, col);
        gfx_line_aa(c, cx, cy - 8, cx, cy + 8, 2.6f, col);
        break;
    case 1:  /* minus */
        gfx_line_aa(c, cx - 8, cy, cx + 8, cy, 2.6f, col);
        break;
    case 2:  /* times */
        gfx_line_aa(c, cx - 6, cy - 6, cx + 6, cy + 6, 2.6f, col);
        gfx_line_aa(c, cx - 6, cy + 6, cx + 6, cy - 6, 2.6f, col);
        break;
    case 3:  /* divide */
        gfx_line_aa(c, cx - 8, cy, cx + 8, cy, 2.6f, col);
        gfx_circle_fill(c, cx, cy - 7, 2, col);
        gfx_circle_fill(c, cx, cy + 7, 2, col);
        break;
    case 4:  /* backspace arrow */
        gfx_line_aa(c, cx - 9, cy, cx + 9, cy, 2.4f, col);
        gfx_line_aa(c, cx - 9, cy, cx - 2, cy - 7, 2.4f, col);
        gfx_line_aa(c, cx - 9, cy, cx - 2, cy + 7, 2.4f, col);
        break;
    case 5:  /* plus/minus */
        gfx_line_aa(c, cx - 8, cy - 4, cx, cy - 4, 2.2f, col);
        gfx_line_aa(c, cx - 4, cy - 8, cx - 4, cy, 2.2f, col);
        gfx_line_aa(c, cx - 8, cy + 7, cx + 8, cy + 7, 2.2f, col);
        break;
    }
}

struct key { const char *label; int ico; char ch; int style; };
static const struct key keys[5][4] = {
    { { "C", -1, 'C', 0 },  { "",  4, 'B', 0 }, { "%", -1, '%', 0 },  { "", 3, '/', 1 } },
    { { "7", -1, '7', 2 },  { "8", -1, '8', 2 }, { "9", -1, '9', 2 }, { "", 2, '*', 1 } },
    { { "4", -1, '4', 2 },  { "5", -1, '5', 2 }, { "6", -1, '6', 2 }, { "", 1, '-', 1 } },
    { { "1", -1, '1', 2 },  { "2", -1, '2', 2 }, { "3", -1, '3', 2 }, { "", 0, '+', 1 } },
    { { "",  5, 'N', 0 },   { "0", -1, '0', 2 }, { ".", -1, '.', 2 }, { "=", -1, '=', 3 } },
};

static void do_key(char ch) {
    dirty = true;
    switch (ch) {
    case 'C': clear_all(); break;
    case 'B': backspace(); break;
    case 'N': negate(); break;
    case '=': evaluate(); break;
    default: put(ch);
    }
}

int main(void) {
    struct gui_window *w = gui_window_create("Calculator", 340, 520, 0);
    if (!w) { perror("calc"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    do {
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            if (k.mods & MOD_CTRL) continue;
            int ch = k.ch;
            if (k.key == KEY_ENTER || ch == '=') do_key('=');
            else if (k.key == KEY_BACKSPACE) do_key('B');
            else if (k.key == KEY_ESC || k.key == KEY_DELETE || ch == 'c' || ch == 'C') do_key('C');
            else if (ch == 'x' || ch == 'X') do_key('*');
            else if (ch && strchr("0123456789.+-*/%", ch)) do_key(ch);
        }
again:
        dirty = false;
        ui_clear(&ui);
        struct canvas *c = ui.c;
        int m = 14, gap = 10, W = w->w, H = w->h;
        int dh = 128;
        /* display */
        ui_panel(&ui, m, m, W - 2 * m, dh);
        gfx_vgradient(c, m + 6, m + 2, W - 2 * m - 12, 2, RGBA(0xB3, 0x88, 0xFF, 80), RGBA(0xB3, 0x88, 0xFF, 0));
        char disp[EXPR_MAX + 8], line[EXPR_MAX + 8];
        const char *src = done ? result : expr[0] ? expr : "0";
        int j = 0;
        for (const char *p = src; *p && j < (int)sizeof disp - 1; p++) disp[j++] = *p == '*' ? 'x' : *p;
        disp[j] = 0;
        snprintf(line, sizeof line, "%s", hist);
        for (char *p = line; *p; p++) if (*p == '*') *p = 'x';
        int maxw = W - 2 * m - 28;
        /* history line, right aligned and clipped on the left */
        if (line[0]) {
            int lw = gfx_text_width(&font_ui, line);
            struct canvas cc = *c;
            canvas_clip(&cc, m + 14, m + 8, maxw, 24);
            gfx_text(&cc, &font_ui, m + 14 + maxw - lw, m + 14, line, UI_TEXT_DIM);
        }
        const struct font *f = &font_big;
        if (gfx_text_width(f, disp) > maxw) f = &font_title;
        if (gfx_text_width(f, disp) > maxw) f = &font_ui_bold;
        int tw = gfx_text_width(f, disp);
        struct canvas cc = *c;
        canvas_clip(&cc, m + 14, m + 36, maxw, dh - 40);
        int tx = m + 14 + maxw - tw;
        if (tx < m + 14) tx = m + 14 + maxw - tw;              /* overflow: show the tail */
        gfx_text(&cc, f, tx, m + 40 + (dh - 48 - f->height) / 2, disp, err ? UI_BAD : UI_TEXT);

        /* buttons */
        int top = m + dh + 14;
        int bw = (W - 2 * m - 3 * gap) / 4, bh = (H - top - m - 4 * gap) / 5;
        for (int r = 0; r < 5; r++)
            for (int cI = 0; cI < 4; cI++) {
                const struct key *kk = &keys[r][cI];
                int x = m + cI * (bw + gap), y = top + r * (bh + gap);
                uint32_t id = (uint32_t)(x * 31 + y * 17 + 1);
                bool hov = ui_hover(&ui, x, y, bw, bh);
                if (hov && ui.pressed) ui.active = id;
                bool hit = ui.released && ui.active == id && hov;
                bool down = ui.active == id && (ui.buttons & 1) && hov;
                color_t bg, fg = UI_TEXT;
                if (kk->style == 3) bg = down ? RGB(0x95, 0x6C, 0xE0) : hov ? RGB(0xC4, 0xA0, 0xFF) : UI_ACCENT;
                else if (kk->style == 1) bg = down ? RGB(0x3A, 0x2E, 0x5C) : hov ? RGB(0x4A, 0x3B, 0x78) : RGB(0x3B, 0x30, 0x62);
                else if (kk->style == 2) bg = down ? UI_PRESSED : hov ? UI_HOVER : UI_SURFACE;
                else bg = down ? UI_PRESSED : hov ? UI_SURFACE : UI_PANEL;
                if (kk->style == 3) fg = RGB(0x1A, 0x10, 0x2E);
                if (kk->style == 1) fg = RGB(0xD9, 0xC4, 0xFF);
                gfx_round_fill(c, x, y + down, bw, bh, UI_RADIUS + 2, bg);
                if (kk->style != 3) gfx_round_rect(c, x, y + down, bw, bh, UI_RADIUS + 2, UI_BORDER);
                if (kk->ico >= 0) icon(c, kk->ico, x + bw / 2, y + bh / 2 + down, kk->ico == 4 && !expr[0] && !done ? UI_TEXT_DIM : fg);
                else {
                    const struct font *f2 = &font_title;
                    gfx_text_center(c, f2, x, y + (bh - f2->height) / 2 + down, bw, kk->label, kk->label[0] == 'C' ? UI_ACCENT2 : fg);
                }
                if (hit) do_key(kk->ch);
            }
        if (dirty) { ui.pressed = ui.released = false; goto again; }   /* buttons changed what the display shows */
        ui_end(&ui);
    } while (ui_poll(&ui, -1));
    gui_window_destroy(w);
    return 0;
}
