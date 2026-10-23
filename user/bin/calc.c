/* calc - calculator with expression history */
#include <ui.h>
#include <design.h>
#include <icons.h>
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

/* kind: 0 function (ghost), 1 operator (secondary, icon), 2 digit (secondary), 3 equals (primary) */
struct key { const char *label; int ico; char ch; int kind; };
static const struct key keys[5][4] = {
    { { "C", -1, 'C', 0 },  { 0, ICON_BACKSPACE, 'B', 0 }, { "%", -1, '%', 0 }, { 0, ICON_DIVIDE, '/', 1 } },
    { { "7", -1, '7', 2 },  { "8", -1, '8', 2 }, { "9", -1, '9', 2 }, { 0, ICON_CLOSE, '*', 1 } },
    { { "4", -1, '4', 2 },  { "5", -1, '5', 2 }, { "6", -1, '6', 2 }, { 0, ICON_MINUS, '-', 1 } },
    { { "1", -1, '1', 2 },  { "2", -1, '2', 2 }, { "3", -1, '3', 2 }, { 0, ICON_PLUS, '+', 1 } },
    { { 0, ICON_PLUS_MINUS, 'N', 0 }, { "0", -1, '0', 2 }, { ".", -1, '.', 2 }, { "=", -1, '=', 3 } },
};
static const char *const key_tip[] = { ['C'] = "Clear \xc2\xb7 Esc", ['B'] = "Delete \xc2\xb7 Backspace", ['N'] = "Change sign",
                                       ['/'] = "Divide", ['*'] = "Multiply", ['-'] = "Subtract", ['+'] = "Add", ['='] = "Equals \xc2\xb7 Enter" };

/* the typed * / - shown as the real multiply, divide and minus signs */
static void pretty(char *out, int n, const char *s) {
    int j = 0;
    for (; *s && j < n - 4; s++) {
        const char *g = *s == '*' ? "\xc3\x97" : *s == '/' ? "\xc3\xb7" : *s == '-' ? "\xe2\x88\x92" : NULL;
        if (g) while (*g) out[j++] = *g++;
        else out[j++] = *s;
    }
    out[j] = 0;
}

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
    struct gui_window *w = gui_window_create("Calculator", 340, 472, 0);
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
        int gap = SP_3, W = w->w, H = w->h;
        int dh = SP_6 + T_BODY->height + SP_2 + T_DISPLAY->height + SP_5;   /* history line, result */
        /* keys stop growing at 96 x 64; a bigger (maximised) window centres the calculator */
        int bw = (W - 2 * SP_5 - 3 * gap) / 4, bh = (H - dh - 2 * SP_5 - 4 * gap) / 5;
        if (bw > 96) bw = 96;
        if (bh > 64) bh = 64;
        int CW = 4 * bw + 3 * gap, CH = dh + 5 * bh + 4 * gap, m = (W - CW) / 2, y0 = (H - CH) / 2, dy = y0 + SP_6;
        char disp[3 * EXPR_MAX + 8], line[3 * EXPR_MAX + 8];   /* signs are up to 3 bytes */
        const char *src = done ? result : expr[0] ? expr : "0";
        pretty(disp, sizeof disp, src);
        pretty(line, sizeof line, hist);
        int maxw = CW - 2 * SP_3, dx = m + SP_3;
        struct canvas cc = *c;
        canvas_clip(&cc, dx, dy, maxw, dh - SP_6);
        if (line[0])                                    /* history: right aligned, the head clipped off on overflow */
            gfx_text(&cc, T_BODY, dx + maxw - gfx_text_width(T_BODY, line), dy, line, UI_TEXT2);
        const struct font *f = T_DISPLAY;
        if (gfx_text_width(f, disp) > maxw) f = T_TITLE;
        if (gfx_text_width(f, disp) > maxw) f = T_HEADING;
        gfx_text(&cc, f, dx + maxw - gfx_text_width(f, disp), dy + T_BODY->height + SP_2 + T_DISPLAY->height - f->height, disp, err ? UI_BAD : UI_TEXT);

        /* keypad */
        int top = y0 + dh;
        for (int r = 0; r < 5; r++)
            for (int cI = 0; cI < 4; cI++) {
                const struct key *kk = &keys[r][cI];
                int x = m + cI * (bw + gap), y = top + r * (bh + gap);
                int st = kk->kind == 3 ? UI_PRIMARY : kk->kind == 0 ? UI_GHOST : UI_SECONDARY;
                if (kk->ch == 'B' && !expr[0] && !done) st |= UI_DISABLED;
                bool hit = ui_button_ex(&ui, x, y, bw, bh, UI_NOICON, NULL, st);
                bool hov = ui_hover(&ui, x, y, bw, bh) && !(st & UI_DISABLED);
                color_t fg = kk->kind == 3 ? UI_ON_ACCENT : st & UI_DISABLED ? ui_th.disabled : kk->kind == 2 || hov ? UI_TEXT : UI_TEXT2;
                if (kk->ico >= 0) ui_icon(&ui, x + (bw - ICON_MD) / 2, y + (bh - ICON_MD) / 2, kk->ico, ICON_MD, fg);
                else {
                    const struct font *f2 = kk->kind == 2 || kk->kind == 3 ? T_TITLE : T_HEADING;
                    gfx_text_center(c, f2, x, y + (bh - f2->height) / 2, bw, kk->label, fg);
                }
                if (key_tip[(int)kk->ch]) ui_tooltip(&ui, x, y, bw, bh, key_tip[(int)kk->ch]);
                if (hit) do_key(kk->ch);
            }
        if (dirty) { ui.pressed = ui.released = false; goto again; }   /* buttons changed what the display shows */
        ui_end(&ui);
    } while (ui_poll(&ui, -1));
    gui_window_destroy(w);
    return 0;
}
