/* solitaire - Klondike with drag and drop. Double-click sends a card to a foundation, cards fly to where they land,
 * draw 1 or 3, undo, auto-complete, score / moves / time. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

enum { STOCK, WASTE, FOUND0, TAB0 = 6, NPILES = 13 };      /* FOUND0..FOUND0+3, TAB0..TAB0+6 */
#define FLY_MS 170
#define UNDO_MAX 200

struct card { unsigned char rank, suit, up; };              /* rank 1..13; suit 0 spades 1 hearts 2 diamonds 3 clubs */
struct pile { struct card c[52]; int n; };
struct state { struct pile p[NPILES]; int score, moves; };

static struct state g;
static struct state *undo_stack;
static int nundo;
static bool draw3, win, autoplay, started;
static uint32_t t_start, t_end;

/* card content colours, the same in every theme: warm paper, muted red and graphite ink, a slate back */
static color_t RED = RGB(0xB3, 0x42, 0x3A), BLACK = RGB(0x26, 0x27, 0x2B), FACE = RGB(0xF7, 0xF5, 0xF0);
static color_t BACK = RGB(0x4A, 0x51, 0x5B), BACK_RIM = RGB(0x6A, 0x73, 0x7E);

/* ---- layout (recomputed every frame from the window size) ---- */
static int cw, ch, gap, topY, tabY, wfan, W_, H_;

static int col_x(int col) { return gap + col * (cw + gap); }

static int tab_step(int p, bool up) {                       /* vertical step under a card of column p, compressed to fit the window */
    struct pile *pl = &g.p[p];
    int du = ch / 4 + 2, dd = ch / 9, avail = H_ - tabY - 12 - ch, need = 0;
    for (int i = 0; i + 1 < pl->n; i++) need += pl->c[i].up ? du : dd;
    if (need > avail && need > 0) { du = du * avail / need; dd = dd * avail / need; }
    if (du < 8) du = 8;
    if (dd < 4) dd = 4;
    return up ? du : dd;
}

static void pile_xy(int p, int idx, int *x, int *y) {
    if (p == STOCK) { *x = col_x(0); *y = topY; }
    else if (p == WASTE) {
        int n = g.p[WASTE].n, d = n - 1 - idx;
        *x = col_x(1); *y = topY;
        if (draw3 && d < 3) { int shown = n < 3 ? n : 3; *x += (shown - 1 - d) * wfan; }
    } else if (p < TAB0) { *x = col_x(3 + p - FOUND0); *y = topY; }
    else {
        *x = col_x(p - TAB0); *y = tabY;
        for (int i = 0; i < idx; i++) *y += tab_step(p, g.p[p].c[i].up);
    }
}

/* ---- suit icons: polygons / circles in a 100 box, supersampled once per size ---- */
struct prim { char k; int n; float v[16]; };
static const struct prim HEART[]   = { { 'c', 3, { 29, 36, 22 } }, { 'c', 3, { 71, 36, 22 } }, { 'p', 3, { 9, 46, 91, 46, 50, 92 } }, { 'p', 0, { 0 } } };
static const struct prim DIAMOND[] = { { 'p', 4, { 50, 4, 88, 50, 50, 96, 12, 50 } }, { 'p', 0, { 0 } } };
static const struct prim SPADE[]   = { { 'c', 3, { 29, 58, 22 } }, { 'c', 3, { 71, 58, 22 } }, { 'p', 3, { 9, 52, 91, 52, 50, 4 } },
                                       { 'p', 4, { 43, 58, 57, 58, 66, 96, 34, 96 } }, { 'p', 0, { 0 } } };
static const struct prim CLUB[]    = { { 'c', 3, { 50, 28, 21 } }, { 'c', 3, { 27, 60, 21 } }, { 'c', 3, { 73, 60, 21 } },
                                       { 'p', 4, { 43, 55, 57, 55, 66, 96, 34, 96 } }, { 'p', 0, { 0 } } };
static const struct prim *SUIT_SHAPE[4] = { SPADE, HEART, DIAMOND, CLUB };
static uint32_t *suit_px[2][4];
static int suit_sz[2];

static int inside(const struct prim *p, float x, float y) {
    if (p->k == 'c') { float dx = x - p->v[0], dy = y - p->v[1]; return dx * dx + dy * dy <= p->v[2] * p->v[2]; }
    int in = 0;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        float xi = p->v[2 * i], yi = p->v[2 * i + 1], xj = p->v[2 * j], yj = p->v[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

static void suits_build(int set, int S) {
    if (suit_sz[set] == S) return;
    suit_sz[set] = S;
    for (int s = 0; s < 4; s++) {
        free(suit_px[set][s]);
        uint32_t *px = suit_px[set][s] = malloc(S * S * 4);
        color_t col = (s == 1 || s == 2) ? RED : BLACK;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int hit = 0;
                for (int sy = 0; sy < 3; sy++)
                    for (int sx = 0; sx < 3; sx++) {
                        float u = (x + (sx + 0.5f) / 3) * 100.0f / S, v = (y + (sy + 0.5f) / 3) * 100.0f / S;
                        for (const struct prim *p = SUIT_SHAPE[s]; p->n; p++) if (inside(p, u, v)) { hit++; break; }
                    }
                px[y * S + x] = (col & 0xFFFFFF) | ((uint32_t)(hit * 255 / 9) << 24);
            }
    }
}

static void suit_draw(struct canvas *c, int set, int suit, int x, int y) {
    int S = suit_sz[set];
    gfx_blit_alpha(c, x, y, suit_px[set][suit], S, S, S);
}

static const char *RANKS[14] = { "", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };

static void draw_card(struct canvas *c, int x, int y, struct card cd, int h_visible) {
    struct canvas cc = *c;
    if (h_visible < ch) canvas_clip(&cc, x, y, cw, h_visible);
    gfx_elev(&cc, x, y, cw, ch, R_3, E1, ui_th.elev[E1]);
    if (!cd.up) {
        gfx_round_fill(&cc, x, y, cw, ch, R_3, BACK);
        gfx_round_rect(&cc, x + SP_2, y + SP_2, cw - 2 * SP_2, ch - 2 * SP_2, R_2, BACK_RIM);
        return;
    }
    color_t col = (cd.suit == 1 || cd.suit == 2) ? RED : BLACK;
    gfx_round_fill(&cc, x, y, cw, ch, R_3, FACE);
    gfx_round_rect(&cc, x, y, cw, ch, R_3, RGBA(0, 0, 0, 46));
    const struct font *f = T_BODY_B;
    int tw = gfx_text_width(f, RANKS[cd.rank]);
    gfx_text(&cc, f, x + SP_3, y + SP_2, RANKS[cd.rank], col);
    suit_draw(&cc, 1, cd.suit, x + SP_3 + tw + SP_2, y + SP_2 + (f->height - suit_sz[1]) / 2);
    int big = suit_sz[0];
    if (cd.rank >= 11) {
        gfx_text_center(&cc, T_DISPLAY, x, y + ch / 2 - T_DISPLAY->height / 2 - 6, cw, RANKS[cd.rank], col);
        suit_draw(&cc, 1, cd.suit, x + cw / 2 - suit_sz[1] / 2, y + ch - suit_sz[1] - SP_4);
    } else {
        suit_draw(&cc, 0, cd.suit, x + (cw - big) / 2, y + ch / 2 - big / 2 + 8);
    }
}

static void draw_slot(struct ui *ui, int x, int y, const char *label, int icon) {
    struct canvas *c = ui->c;
    gfx_round_fill(c, x, y, cw, ch, R_3, TH_A(UI_TEXT, 6));
    gfx_round_rect(c, x, y, cw, ch, R_3, ui_th.line_strong);
    if (label) gfx_text_center(c, T_TITLE, x, y + (ch - T_TITLE->height) / 2, cw, label, UI_TEXT3);
    if (icon >= 0) ui_icon(ui, x + (cw - ICON_LG) / 2, y + (ch - ICON_LG) / 2, icon, ICON_LG, UI_TEXT3);
}

/* ---- game ---- */
static void push_undo(void) {
    if (!undo_stack) undo_stack = malloc(UNDO_MAX * sizeof *undo_stack);
    if (!undo_stack) return;
    if (nundo == UNDO_MAX) { memmove(undo_stack, undo_stack + 1, (UNDO_MAX - 1) * sizeof *undo_stack); nundo--; }
    undo_stack[nundo++] = g;
}

static void new_game(void) {
    memset(&g, 0, sizeof g);
    struct card deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (struct card){ i % 13 + 1, i / 13, 0 };
    for (int i = 51; i > 0; i--) { int j = rand() % (i + 1); struct card t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    int k = 0;
    for (int col = 0; col < 7; col++)
        for (int i = 0; i <= col; i++) { g.p[TAB0 + col].c[g.p[TAB0 + col].n++] = deck[k]; g.p[TAB0 + col].c[i].up = i == col; k++; }
    while (k < 52) g.p[STOCK].c[g.p[STOCK].n++] = deck[k++];
    nundo = 0; win = autoplay = started = false;
    t_start = t_end = 0;
}

static bool is_red(struct card c) { return c.suit == 1 || c.suit == 2; }

static bool can_drop(int p, struct card top_moved) {          /* may a stack starting with this card land on pile p */
    struct pile *pl = &g.p[p];
    if (p >= TAB0) {
        if (!pl->n) return top_moved.rank == 13;
        struct card t = pl->c[pl->n - 1];
        return t.up && is_red(t) != is_red(top_moved) && t.rank == top_moved.rank + 1;
    }
    return false;
}
static bool can_found(int p, struct card c) {
    struct pile *pl = &g.p[p];
    if (!pl->n) return c.rank == 1;
    return pl->c[pl->n - 1].suit == c.suit && pl->c[pl->n - 1].rank + 1 == c.rank;
}

/* ---- flying cards (also used for returns); the destination cards are hidden until they land ---- */
static struct { int n, hp, hidx; float sx[13], sy[13], ex[13], ey[13]; struct card c[13]; uint32_t t0; bool on; } fly;

static void fly_start(int dest, int idx, int n, const float *sx, const float *sy) {
    fly.on = true; fly.n = n; fly.hp = dest; fly.hidx = idx; fly.t0 = uptime_ms();
    for (int i = 0; i < n; i++) {
        int x, y;
        pile_xy(dest, idx + i, &x, &y);
        fly.sx[i] = sx[i]; fly.sy[i] = sy[i]; fly.ex[i] = x; fly.ey[i] = y;
        fly.c[i] = g.p[dest].c[idx + i];
    }
}

static void flip_top(int p) {
    struct pile *pl = &g.p[p];
    if (p >= TAB0 && pl->n && !pl->c[pl->n - 1].up) { pl->c[pl->n - 1].up = 1; g.score += 5; }
}

static void add_score(int d) { g.score += d; if (g.score < 0) g.score = 0; }

/* move n cards from pile `from` (starting at idx) to pile `to`; sx/sy = where they start on screen */
static void move_cards(int from, int idx, int to, const float *sx, const float *sy) {
    push_undo();
    int n = g.p[from].n - idx;
    for (int i = 0; i < n; i++) g.p[to].c[g.p[to].n + i] = g.p[from].c[idx + i];
    int at = g.p[to].n;
    g.p[to].n += n; g.p[from].n = idx;
    if (to >= FOUND0 && to < TAB0) add_score(10);
    else if (from == WASTE && to >= TAB0) add_score(5);
    else if (from >= FOUND0 && from < TAB0 && to >= TAB0) add_score(-15);
    flip_top(from);
    g.moves++;
    if (!started) { started = true; t_start = uptime_ms(); }
    fly_start(to, at, n, sx, sy);
    int full = 0;
    for (int f = 0; f < 4; f++) full += g.p[FOUND0 + f].n == 13;
    if (full == 4) { win = true; autoplay = false; t_end = uptime_ms(); }
}

static bool auto_to_found(int from) {                          /* send the top card of `from` to a foundation if one takes it */
    struct pile *pl = &g.p[from];
    if (!pl->n || !pl->c[pl->n - 1].up) return false;
    for (int f = FOUND0; f < TAB0; f++)
        if (can_found(f, pl->c[pl->n - 1])) {
            float sx, sy;
            int x, y;
            pile_xy(from, pl->n - 1, &x, &y);
            sx = x; sy = y;
            move_cards(from, pl->n - 1, f, &sx, &sy);
            return true;
        }
    return false;
}

static void draw_stock(void) {
    push_undo();
    struct pile *s = &g.p[STOCK], *w = &g.p[WASTE];
    if (s->n) {
        int k = draw3 ? 3 : 1;
        for (int i = 0; i < k && s->n; i++) { struct card c = s->c[--s->n]; c.up = 1; w->c[w->n++] = c; }
    } else if (w->n) {                                          /* recycle the waste */
        while (w->n) { struct card c = w->c[--w->n]; c.up = 0; s->c[s->n++] = c; }
        add_score(draw3 ? -20 : -100);
    } else { nundo--; return; }
    g.moves++;
    if (!started) { started = true; t_start = uptime_ms(); }
}

static void undo(void) {
    if (!nundo || fly.on) return;
    g = undo_stack[--nundo];
    win = autoplay = false;
}

static bool can_autocomplete(void) {
    if (win || g.p[STOCK].n || g.p[WASTE].n) return false;
    for (int p = TAB0; p < NPILES; p++) for (int i = 0; i < g.p[p].n; i++) if (!g.p[p].c[i].up) return false;
    return true;
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Solitaire", 720, 660, 0);
    if (!w) return 1;
    srand(argc > 1 ? (unsigned)atoi(argv[1]) : uptime_ms());      /* optional deal number */
    struct ui ui;
    ui_init(&ui, w);
    new_game();
    int drag_p = -1, drag_i = 0, drag_n = 0;
    float press_x = 0, press_y = 0, grab_dx = 0, grab_dy = 0;
    bool moved_drag = false;
    uint32_t last_auto = 0;
    int mode_tab = 0;
    for (;;) {
        bool busy = fly.on || autoplay || drag_p >= 0;
        if (!ui_poll(&ui, busy ? 16 : 500)) break;
        W_ = w->w; H_ = w->h;
        gap = W_ / 60 < 8 ? 8 : W_ / 60;
        cw = (W_ - 8 * gap) / 7;
        if (cw > 110) cw = 110;
        ch = cw * 7 / 5;
        gap = (W_ - 7 * cw) / 8;
        topY = HEAD + SP_5; tabY = topY + ch + SP_6;
        wfan = cw / 5;
        suits_build(0, cw * 5 / 12);
        suits_build(1, 14);
        uint32_t now = uptime_ms();
        if (fly.on && now - fly.t0 >= FLY_MS) fly.on = false;

        struct gui_event k;
        while (ui_key(&ui, &k)) {
            int ch2 = k.key < 128 ? (k.key | 0x20) : 0;
            if (ch2 == 'n') new_game();
            else if (ch2 == 'u' || ((k.mods & MOD_CTRL) && ch2 == 'z')) undo();
        }

        /* ---- mouse ---- */
        if (ui.pressed && !fly.on && !win && ui.my > HEAD) {
            int hp = -1, hi = -1;
            for (int p = NPILES - 1; p >= 0 && hp < 0; p--) {
                struct pile *pl = &g.p[p];
                if (p == STOCK || p < TAB0) {                              /* stock, waste, foundations: only the top card */
                    int x, y;
                    int idx = pl->n ? pl->n - 1 : 0;
                    pile_xy(p, idx, &x, &y);
                    if (ui_hover(&ui, x, y, cw, ch)) { hp = p; hi = pl->n ? pl->n - 1 : -1; }
                } else {
                    for (int i = pl->n - 1; i >= 0; i--) {
                        int x, y;
                        pile_xy(p, i, &x, &y);
                        int hgt = i == pl->n - 1 ? ch : tab_step(p, pl->c[i].up);
                        if (ui_hover(&ui, x, y, cw, hgt)) { hp = p; hi = i; break; }
                    }
                    if (hp < 0 && !pl->n && ui_hover(&ui, col_x(p - TAB0), tabY, cw, ch)) { hp = p; hi = -1; }
                }
            }
            if (hp == STOCK) draw_stock();
            else if (hp >= 0 && hi >= 0 && g.p[hp].c[hi].up) {
                if (ui.dbl && hi == g.p[hp].n - 1 && auto_to_found(hp)) { drag_p = -1; }
                else {
                    int x, y;
                    pile_xy(hp, hi, &x, &y);
                    drag_p = hp; drag_i = hi; drag_n = g.p[hp].n - hi; moved_drag = false;
                    grab_dx = ui.mx - x; grab_dy = ui.my - y; press_x = ui.mx; press_y = ui.my;
                }
            }
        }
        if (drag_p >= 0 && (ui.buttons & 1) && abs(ui.mx - press_x) + abs(ui.my - press_y) > 4) moved_drag = true;
        if (drag_p >= 0 && ui.released) {
            if (moved_drag) {
                float sx[13], sy[13];
                int step = drag_p >= TAB0 ? tab_step(drag_p, true) : 0;
                for (int i = 0; i < drag_n; i++) { sx[i] = ui.mx - grab_dx; sy[i] = ui.my - grab_dy + i * step; }
                /* best target by overlap of the dragged first card with each pile's landing rect */
                int best = -1, best_area = 0;
                struct card lead = g.p[drag_p].c[drag_i];
                for (int p = FOUND0; p < NPILES; p++) {
                    if (p == drag_p) continue;
                    bool ok = p < TAB0 ? (drag_n == 1 && can_found(p, lead)) : can_drop(p, lead);
                    if (!ok) continue;
                    int x, y;
                    pile_xy(p, g.p[p].n ? g.p[p].n - 1 : 0, &x, &y);
                    int ox = (int)sx[0] < x ? (int)sx[0] + cw - x : x + cw - (int)sx[0];
                    int oy = (int)sy[0] < y ? (int)sy[0] + ch - y : y + ch - (int)sy[0];
                    if (ox > cw) ox = cw;
                    if (oy > ch) oy = ch;
                    if (ox > 0 && oy > 0 && ox * oy > best_area) { best_area = ox * oy; best = p; }
                }
                if (best >= 0) move_cards(drag_p, drag_i, best, sx, sy);
                else fly_start(drag_p, drag_i, drag_n, sx, sy);             /* snap back */
            }
            drag_p = -1;
        }
        if (drag_p >= 0 && !(ui.buttons & 1)) drag_p = -1;

        /* auto-complete: one card per tick */
        if (autoplay && !fly.on && now - last_auto > 40) {
            bool did = false;
            for (int p = TAB0; p < NPILES && !did; p++) did = auto_to_found(p);
            if (!did) autoplay = false;
            last_auto = now;
        }

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;
        game_head(&ui, W_);
        {
            char t[16];
            uint32_t el = started ? ((win ? t_end : now) - t_start) / 1000 : 0;
            snprintf(t, sizeof t, "%d:%02d", (int)(el / 60), (int)(el % 60));
            game_stat(&ui, game_stati(&ui, game_stati(&ui, SP_5, "Score", g.score), "Moves", g.moves), "Time", t);
        }
        int rx = W_ - SP_5;
        if (game_btn(&ui, &rx, UI_NOICON, "New game", UI_SECONDARY)) new_game();
        {
            const char *names[2] = { "Draw 1", "Draw 3" };
            int sw = 3 * SP_1;
            for (int i = 0; i < 2; i++) sw += gfx_text_width(T_BODY, names[i]) + 2 * SP_4;
            rx -= sw;
            mode_tab = draw3;
            if (ui_segmented(&ui, rx, CTL_Y, NULL, names, 2, &mode_tab) && mode_tab != draw3) { draw3 = mode_tab; new_game(); }
            rx -= SP_3;
        }
        if (game_btn(&ui, &rx, ICON_UNDO, NULL, UI_GHOST | (nundo ? 0 : UI_DISABLED))) undo();
        ui_tooltip(&ui, rx + SP_3, CTL_Y, H_CTL, H_CTL, "Undo · U");
        if (can_autocomplete() && !autoplay && game_btn(&ui, &rx, UI_NOICON, "Finish", UI_SECONDARY)) autoplay = true;
        /* top row */
        if (g.p[STOCK].n) draw_card(c, col_x(0), topY, (struct card){ 0, 0, 0 }, ch);
        else {
            draw_slot(&ui, col_x(0), topY, NULL, g.p[WASTE].n ? ICON_REFRESH : -1);
        }
        draw_slot(&ui, col_x(1), topY, NULL, -1);
        for (int i = 0; i < g.p[WASTE].n; i++) {
            if (g.p[WASTE].n - 1 - i > 3 && draw3) continue;
            if (drag_p == WASTE && moved_drag && i == g.p[WASTE].n - 1) continue;
            if (fly.on && fly.hp == WASTE && i >= fly.hidx) continue;
            int x, y;
            pile_xy(WASTE, i, &x, &y);
            if (!draw3 && g.p[WASTE].n - 1 - i > 0) continue;
            draw_card(c, x, y, g.p[WASTE].c[i], ch);
        }
        for (int f = FOUND0; f < TAB0; f++) {
            int x, y;
            pile_xy(f, 0, &x, &y);
            draw_slot(&ui, x, y, "A", -1);
            int n = g.p[f].n;
            if (fly.on && fly.hp == f) n = fly.hidx;
            if (drag_p == f && moved_drag) n--;
            if (n > 0) draw_card(c, x, y, g.p[f].c[n - 1], ch);
        }
        for (int p = TAB0; p < NPILES; p++) {
            struct pile *pl = &g.p[p];
            int x0, y0;
            pile_xy(p, 0, &x0, &y0);
            draw_slot(&ui, x0, y0, pl->n ? NULL : "K", -1);
            for (int i = 0; i < pl->n; i++) {
                if (drag_p == p && moved_drag && i >= drag_i) break;
                if (fly.on && fly.hp == p && i >= fly.hidx) break;
                int x, y;
                pile_xy(p, i, &x, &y);
                draw_card(c, x, y, pl->c[i], i == pl->n - 1 ? ch : tab_step(p, pl->c[i].up) + 8);
            }
        }
        /* drop hints while dragging */
        if (drag_p >= 0 && moved_drag) {
            struct card lead = g.p[drag_p].c[drag_i];
            for (int p = FOUND0; p < NPILES; p++) {
                if (p == drag_p) continue;
                bool ok = p < TAB0 ? (drag_n == 1 && can_found(p, lead)) : can_drop(p, lead);
                if (!ok) continue;
                int x, y;
                pile_xy(p, g.p[p].n ? g.p[p].n - 1 : 0, &x, &y);
                gfx_rrect_ring(c, x - 3, y - 3, cw + 6, ch + 6, RADII(R_3 + 3, R_3 + 3, R_3 + 3, R_3 + 3), 2, ui_th.accent_line);
            }
        }
        if (fly.on) {
            float t = (float)(now - fly.t0) / FLY_MS;
            t = 1 - (1 - t) * (1 - t);
            for (int i = 0; i < fly.n; i++)
                draw_card(c, (int)(fly.sx[i] + (fly.ex[i] - fly.sx[i]) * t), (int)(fly.sy[i] + (fly.ey[i] - fly.sy[i]) * t), fly.c[i], ch);
        }
        if (drag_p >= 0 && moved_drag) {
            int step = drag_p >= TAB0 ? tab_step(drag_p, true) : 0;
            for (int i = 0; i < drag_n; i++) draw_card(c, ui.mx - (int)grab_dx, ui.my - (int)grab_dy + i * step, g.p[drag_p].c[drag_i + i], ch);
        }
        if (win) {
            char t[64];
            uint32_t el = (t_end - t_start) / 1000;
            snprintf(t, sizeof t, "Score %d in %d moves, %d:%02d.", g.score, g.moves, (int)(el / 60), (int)(el % 60));
            if (game_card(&ui, 0, HEAD, W_, H_ - HEAD, "You won", t, NULL, "Play again")) new_game();
        }
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
