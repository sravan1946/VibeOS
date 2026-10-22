/* chess - play the computer: click or drag pieces, legal-move hints, castling, en passant, promotion,
 * check / mate / stalemate / draw detection, three difficulty levels. Rules and search live in engine.c. */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include "engine.h"
#include "pieces.h"

#define MAXPLY 600
#define PANEL_W 262
#define ANIM_MS 170

static const struct { const char *name; int depth, ms, noise; } LEVEL[3] = {
    { "Easy", 2, 400, 90 }, { "Medium", 3, 1800, 10 }, { "Hard", 6, 2500, 0 },
};

static cpos hist[MAXPLY + 1];
static cmove mv[MAXPLY];
static char san[MAXPLY][12];
static int nply;
static int human = WHITE, level = 1, flipped;
static int over;                          /* 0 playing, else ST_* or 10 for repetition */
static int sel = -1;                      /* selected square */
static int drag_from = -1, drag_x, drag_y;
static bool dragging;
static int press_x, press_y;
static int promo_from = -1, promo_to = -1;
static struct { int from, to, piece; uint32_t t0; } anim = { -1, -1, 0, 0 };
static int think;                         /* frames the AI has been due, so "Thinking" is drawn before it blocks */
static int list_first;
static bool list_stick = true;

static uint32_t clock_ms(void) { return uptime_ms(); }

static color_t SQ_LIGHT = RGB(0xE6, 0xDD, 0xF6), SQ_DARK = RGB(0x8E, 0x74, 0xC4);

static int cur_side(void) { return hist[nply].side; }

static void check_over(void) {
    over = cp_status(&hist[nply]);
    int same = 0;
    for (int i = 0; i <= nply; i++) if (cp_same(&hist[i], &hist[nply])) same++;
    if (!over && same >= 3) over = 10;
}

static void play(cmove m) {
    if (nply >= MAXPLY) return;
    cp_san(&hist[nply], m, san[nply]);
    anim.from = m.from; anim.to = m.to; anim.piece = hist[nply].b[m.from]; anim.t0 = uptime_ms();
    mv[nply] = m;
    cp_make(&hist[nply], m, &hist[nply + 1]);
    nply++;
    sel = -1; promo_from = -1;
    check_over();
    list_stick = true;
    think = 0;
}

static void new_game(void) {
    cp_start(&hist[0]);
    nply = 0; over = 0; sel = -1; promo_from = -1; dragging = false; drag_from = -1;
    anim.from = -1; think = 0; list_first = 0; list_stick = true;
    flipped = human == BLACK;
}

static void undo(void) {
    if (!nply) return;
    do nply--; while (nply > 0 && cur_side() != human);
    sel = -1; promo_from = -1; anim.from = -1; think = 0;
    check_over();
}

static int cell, bx, by;

static int sq_x(int sq) { int f = sq & 7; return bx + (flipped ? 7 - f : f) * cell; }
static int sq_y(int sq) { int r = sq >> 3; return by + (flipped ? r : 7 - r) * cell; }
static int sq_at(int x, int y) {
    int c = (x - bx) / cell, r = (y - by) / cell;
    if (x < bx || y < by || c > 7 || r > 7) return -1;
    return (flipped ? r : 7 - r) * 8 + (flipped ? 7 - c : c);
}

static void draw_piece(struct ui *ui, int set, int piece, int x, int y, int size) {
    gfx_blit_alpha(ui->c, x, y, piece_sprite(set, piece), size, size, size);
}

static const char *status_text(void) {
    static char b[48];
    if (over == ST_MATE) return cur_side() == human ? "Checkmate - AI wins" : "Checkmate - you win!";
    if (over == ST_STALEMATE) return "Stalemate - draw";
    if (over == ST_INSUFFICIENT) return "Draw - insufficient material";
    if (over == ST_FIFTY) return "Draw - fifty-move rule";
    if (over == 10) return "Draw - threefold repetition";
    if (cur_side() != human) return "Thinking...";
    snprintf(b, sizeof b, "%s to move%s", cur_side() == WHITE ? "White" : "Black", cp_in_check(&hist[nply], cur_side()) ? " - check!" : "");
    return b;
}

/* pieces captured so far by `by` (their colour's opponents), as sprites in a row; returns material lead */
static void captured_row(struct ui *ui, int x, int y, int by_side) {
    static const int init[7] = { 0, 8, 2, 2, 2, 1, 0 }, val[7] = { 0, 1, 3, 3, 5, 9, 0 };
    int have[7] = { 0 }, gone[7], lead = 0, mine = 0, theirs = 0;
    for (int i = 0; i < 64; i++) {
        int p = hist[nply].b[i];
        if (p && (p > 0) != (by_side == WHITE)) have[p < 0 ? -p : p]++;       /* pieces of the side being captured */
        if (p && (p > 0) == (by_side == WHITE)) mine += val[p < 0 ? -p : p];
        if (p && (p > 0) != (by_side == WHITE)) theirs += val[p < 0 ? -p : p];
    }
    lead = mine - theirs;
    int ox = x;
    for (int t = 5; t >= 1; t--) {
        gone[t] = init[t] - have[t];
        for (int k = 0; k < gone[t]; k++) {
            draw_piece(ui, 1, by_side == WHITE ? -t : t, ox, y, 24);
            ox += gone[t] > 3 ? 10 : 16;
        }
        if (gone[t] > 0) ox += 6;
    }
    if (lead > 0) { char s[8]; snprintf(s, sizeof s, "+%d", lead); gfx_text(ui->c, &font_ui_bold, ox + 4, y + 4, s, UI_ACCENT); }
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Chess", 840, 604, 0);
    if (!w) return 1;
    engine_clock = clock_ms;
    struct ui ui;
    ui_init(&ui, w);
    new_game();
    int lv_tab = level, side_tab = 0;
    for (;;) {
        bool ai_turn = !over && cur_side() != human && nply < MAXPLY;
        bool animating = anim.from >= 0 && uptime_ms() - anim.t0 < ANIM_MS;
        if (!ui_poll(&ui, animating ? 16 : ai_turn ? 0 : 500)) break;
        int W = w->w, H = w->h;
        cell = (W - PANEL_W - 48) / 8;
        if ((H - 40) / 8 < cell) cell = (H - 40) / 8;
        if (cell < 40) cell = 40;
        bx = 24; by = (H - 8 * cell) / 2;
        pieces_build(0, cell * 88 / 100);
        pieces_build(1, 24);
        int ps = cell * 88 / 100, off = (cell - ps) / 2;
        int px0 = bx + 8 * cell + 24, pw = W - px0 - 16;

        bool human_turn = !over && cur_side() == human && promo_from < 0;

        /* ---- keys ---- */
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            int ch = k.key < 128 ? (k.key | 0x20) : 0;
            if (k.key == KEY_ESC) { sel = -1; promo_from = -1; dragging = false; }
            else if (ch == 'n' || ((k.mods & MOD_CTRL) && ch == 'n')) new_game();
            else if (ch == 'u' || ((k.mods & MOD_CTRL) && ch == 'z')) undo();
            else if (ch == 'f') flipped = !flipped;
        }

        /* ---- board input ---- */
        cmove legal[256];
        int nlegal = cp_moves(&hist[nply], legal);
        if (promo_from >= 0) {
            if (ui.pressed) {
                int tx = sq_x(promo_to), ty0 = sq_y(promo_to), down = ty0 < by + 4 * cell;
                static const int order[4] = { Q_, R_, B_, N_ };
                int chosen = 0;
                for (int i = 0; i < 4; i++)
                    if (ui_hover(&ui, tx, down ? ty0 + i * cell : ty0 - i * cell, cell, cell)) chosen = order[i];
                if (chosen) {
                    for (int i = 0; i < nlegal; i++)
                        if (legal[i].from == promo_from && legal[i].to == promo_to && legal[i].promo == chosen) { play(legal[i]); break; }
                }
                promo_from = -1; sel = -1;
                ui.pressed = false;
            }
        } else if (human_turn) {
            int hs = sq_at(ui.mx, ui.my);
            if (ui.pressed && hs >= 0) {
                int pc = hist[nply].b[hs];
                bool mine = pc && (pc > 0) == (human == WHITE);
                int target = -1;
                if (sel >= 0 && hs != sel) for (int i = 0; i < nlegal; i++) if (legal[i].from == sel && legal[i].to == hs) target = hs;
                if (target >= 0) {
                    int cnt = 0;
                    for (int i = 0; i < nlegal; i++) if (legal[i].from == sel && legal[i].to == hs) cnt++;
                    if (cnt > 1) { promo_from = sel; promo_to = hs; }
                    else for (int i = 0; i < nlegal; i++) if (legal[i].from == sel && legal[i].to == hs) { play(legal[i]); break; }
                } else if (mine) {
                    sel = hs; drag_from = hs; dragging = false; press_x = ui.mx; press_y = ui.my;
                } else sel = -1;
            } else if (drag_from >= 0 && (ui.buttons & 1)) {
                if (abs(ui.mx - press_x) + abs(ui.my - press_y) > 6) dragging = true;
                drag_x = ui.mx; drag_y = ui.my;
            }
            if (ui.released && drag_from >= 0) {
                if (dragging && hs >= 0 && hs != drag_from) {
                    int cnt = 0, idx = -1;
                    for (int i = 0; i < nlegal; i++) if (legal[i].from == drag_from && legal[i].to == hs) { cnt++; idx = i; }
                    if (cnt > 1) { promo_from = drag_from; promo_to = hs; }
                    else if (cnt == 1) play(legal[idx]);
                    else if (hist[nply].b[hs] && ((hist[nply].b[hs] > 0) == (human == WHITE))) sel = hs;
                    else sel = drag_from;
                }
                dragging = false; drag_from = -1;
            }
        } else if (ui.released) { dragging = false; drag_from = -1; }

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;
        gfx_shadow(c, bx, by, 8 * cell, 8 * cell, 10, 14, 110);
        for (int sq = 0; sq < 64; sq++) {
            int x = sq_x(sq), y = sq_y(sq), f = sq & 7, r = sq >> 3;
            gfx_fill(c, x, y, cell, cell, ((f + r) & 1) ? SQ_LIGHT : SQ_DARK);
        }
        /* last move */
        if (nply > 0) {
            gfx_fill(c, sq_x(mv[nply - 1].from), sq_y(mv[nply - 1].from), cell, cell, RGBA(0xFF, 0xD8, 0x4B, 90));
            gfx_fill(c, sq_x(mv[nply - 1].to), sq_y(mv[nply - 1].to), cell, cell, RGBA(0xFF, 0xD8, 0x4B, 110));
        }
        if (sel >= 0) gfx_fill(c, sq_x(sel), sq_y(sel), cell, cell, RGBA(0x7C, 0x4D, 0xFF, 130));
        if (cp_in_check(&hist[nply], cur_side())) {
            for (int i = 0; i < 64; i++) if (hist[nply].b[i] == (cur_side() == WHITE ? K_ : -K_)) {
                int cx = sq_x(i) + cell / 2, cy = sq_y(i) + cell / 2;
                struct canvas cc = *c;
                canvas_clip(&cc, sq_x(i), sq_y(i), cell, cell);
                for (int rr = cell * 3 / 4; rr > 4; rr -= cell / 14) gfx_circle_fill(&cc, cx, cy, rr, RGBA(0xFF, 0x30, 0x50, 26));
            }
        }
        for (int sq = 0; sq < 64; sq++) {                 /* file / rank labels */
            int x = sq_x(sq), y = sq_y(sq), f = sq & 7, r = sq >> 3;
            color_t lc = ((f + r) & 1) ? SQ_DARK : SQ_LIGHT;
            if (r == (flipped ? 7 : 0)) { char s[2] = { 'a' + f, 0 }; gfx_text(c, &font_ui, x + cell - 11, y + cell - 18, s, lc); }
            if (f == (flipped ? 7 : 0)) { char s[2] = { '1' + r, 0 }; gfx_text(c, &font_ui, x + 4, y + 2, s, lc); }
        }
        /* pieces */
        int slide = animating ? (int)(ANIM_MS - (uptime_ms() - anim.t0)) : 0;
        for (int sq = 0; sq < 64; sq++) {
            int pc = hist[nply].b[sq];
            if (!pc) continue;
            if (dragging && sq == drag_from) continue;
            int x = sq_x(sq) + off, y = sq_y(sq) + off;
            if (animating && sq == anim.to) {                 /* ease-out slide from the origin square */
                float t = 1.0f - (float)slide / ANIM_MS;
                t = 1 - (1 - t) * (1 - t);
                x = sq_x(anim.from) + off + (int)((sq_x(anim.to) - sq_x(anim.from)) * t);
                y = sq_y(anim.from) + off + (int)((sq_y(anim.to) - sq_y(anim.from)) * t);
            }
            draw_piece(&ui, 0, pc, x, y, ps);
        }
        /* move hints */
        if (sel >= 0 && human_turn) {
            for (int i = 0; i < nlegal; i++) {
                if (legal[i].from != sel) continue;
                int to = legal[i].to, x = sq_x(to), y = sq_y(to);
                bool cap = hist[nply].b[to] != 0 || (hist[nply].b[sel] * hist[nply].b[sel] == 1 && to == hist[nply].ep);
                if (cap) {
                    struct canvas cc = *c;
                    canvas_clip(&cc, x, y, cell, cell);
                    gfx_circle_fill(&cc, x + cell / 2, y + cell / 2, cell * 3 / 5, RGBA(0x30, 0x20, 0x60, 90));
                    gfx_circle_fill(&cc, x + cell / 2, y + cell / 2, cell * 3 / 5 - 5, ((to & 7) + (to >> 3)) & 1 ? SQ_LIGHT : SQ_DARK);
                    draw_piece(&ui, 0, hist[nply].b[to] ? hist[nply].b[to] : -hist[nply].b[sel], x + off, y + off, ps);
                } else gfx_circle_fill(c, x + cell / 2, y + cell / 2, cell / 8, RGBA(0x30, 0x20, 0x60, 110));
            }
        }
        if (dragging && drag_from >= 0) {
            int hs = sq_at(ui.mx, ui.my);
            if (hs >= 0) gfx_rect(c, sq_x(hs), sq_y(hs), cell, cell, RGBA(255, 255, 255, 200));
            draw_piece(&ui, 0, hist[nply].b[drag_from], drag_x - ps / 2, drag_y - ps / 2, ps);
        }
        if (promo_from >= 0) {                                /* promotion chooser: a strip of pieces from the target square inward */
            gfx_fill(c, bx, by, 8 * cell, 8 * cell, RGBA(0, 0, 0, 120));
            int tx = sq_x(promo_to), ty0 = sq_y(promo_to), down = ty0 < by + 4 * cell;
            static const int order[4] = { Q_, R_, B_, N_ };
            for (int i = 0; i < 4; i++) {
                int yy = down ? ty0 + i * cell : ty0 - i * cell;
                bool hov = ui_hover(&ui, tx, yy, cell, cell);
                gfx_round_fill(c, tx + 2, yy + 2, cell - 4, cell - 4, 10, hov ? RGB(0xF4, 0xE8, 0xFF) : RGB(0xD2, 0xC4, 0xEE));
                draw_piece(&ui, 0, human == WHITE ? order[i] : -order[i], tx + off, yy + off, ps);
            }
        }
        if (over) {
            const char *t = status_text();
            int tw = gfx_text_width(&font_title, t) + 48, th = 64;
            int x = bx + (8 * cell - tw) / 2, y = by + (8 * cell - th) / 2;
            gfx_shadow(c, x, y, tw, th, 14, 14, 150);
            gfx_round_fill(c, x, y, tw, th, 14, RGBA(0x1E, 0x1C, 0x2D, 235));
            gfx_round_rect(c, x, y, tw, th, 14, RGBA(0xB3, 0x88, 0xFF, 160));
            gfx_text_center(c, &font_title, x, y + (th - font_title.height) / 2, tw, t, UI_TEXT);
        }

        /* ---- side panel ---- */
        ui_panel(&ui, px0, by, pw, 8 * cell);
        int y = by + 14, ix = px0 + 14, iw = pw - 28;
        gfx_text(c, &font_title, ix, y, "Chess", UI_TEXT);
        y += font_title.height + 6;
        color_t sc = over ? (over == ST_MATE && cur_side() != human ? UI_GOOD : over == ST_MATE ? UI_BAD : UI_WARN)
                          : cur_side() != human ? UI_ACCENT : UI_TEXT_DIM;
        gfx_circle_fill(c, ix + 5, y + 9, 4, sc);
        ui_text_fit(&ui, &font_ui, ix + 18, y, iw - 18, status_text(), sc);
        y += 30;
        captured_row(&ui, ix, y, human == WHITE ? BLACK : WHITE);   /* what the AI has taken from you */
        y += 28;
        captured_row(&ui, ix, y, human);
        y += 34;
        gfx_text(c, &font_ui, ix, y, "Difficulty", UI_TEXT_DIM);
        y += 22;
        {
            const char *names[3] = { LEVEL[0].name, LEVEL[1].name, LEVEL[2].name };
            if (ui_tabs(&ui, ix, y, iw, 32, names, 3, &lv_tab)) level = lv_tab;
        }
        y += 44;
        gfx_text(c, &font_ui, ix, y, "Play as", UI_TEXT_DIM);
        y += 22;
        {
            const char *names[2] = { "White", "Black" };
            side_tab = human;
            if (ui_tabs(&ui, ix, y, iw, 32, names, 2, &side_tab) && side_tab != human) { human = side_tab; new_game(); }
        }
        y += 46;
        int bw = (iw - 16) / 3;
        if (ui_button(&ui, ix, y, bw, 34, "New", UI_ACCENT_BTN)) new_game();
        if (ui_button(&ui, ix + bw + 8, y, bw, 34, "Undo", UI_NORMAL_BTN)) undo();
        if (ui_button(&ui, ix + 2 * (bw + 8), y, bw, 34, "Flip", UI_NORMAL_BTN)) flipped = !flipped;
        y += 46;
        /* move list */
        int ly = y, lh = by + 8 * cell - 12 - ly, rows = (nply + 1) / 2, rh = 22, vis = lh / rh;
        gfx_round_fill(c, ix, ly, iw, lh, 8, RGB(0x16, 0x15, 0x22));
        if (rows > vis && list_stick) list_first = rows - vis;
        if (ui_hover(&ui, ix, ly, iw, lh) && ui.scroll) { list_first += ui.scroll; list_stick = false; }
        if (list_first > rows - vis) { list_first = rows - vis; }
        if (list_first < 0) list_first = 0;
        struct canvas lc = *c;
        canvas_clip(&lc, ix, ly + 2, iw, lh - 4);
        for (int r = list_first; r < rows && r < list_first + vis + 1; r++) {
            int yy = ly + 6 + (r - list_first) * rh;
            char num[8];
            snprintf(num, sizeof num, "%d.", r + 1);
            gfx_text(&lc, &font_mono, ix + 8, yy, num, UI_TEXT_DIM);
            for (int s = 0; s < 2 && r * 2 + s < nply; s++) {
                int idx = r * 2 + s, xx = ix + 44 + s * (iw - 52) / 2;
                if (idx == nply - 1) gfx_round_fill(&lc, xx - 4, yy - 2, (iw - 52) / 2 - 4, rh - 2, 5, RGBA(0xB3, 0x88, 0xFF, 80));
                gfx_text(&lc, &font_mono, xx, yy, san[idx], UI_TEXT);
            }
        }
        if (!nply) gfx_text_center(c, &font_ui, ix, ly + lh / 2 - 8, iw, "No moves yet", UI_TEXT_DIM);
        {
            int first = list_first;
            ui_vscroll(&ui, ix + iw - 10, ly + 4, lh - 8, rows, vis, &first);
            if (first != list_first) { list_first = first; list_stick = false; }
        }
        ui_end(&ui);

        /* the AI moves after a couple of frames so "Thinking..." is on screen while it works */
        if (ai_turn && !animating && promo_from < 0) {
            if (++think >= 3) {
                int sc2;
                cmove m = cp_search(&hist[nply], LEVEL[level].depth, LEVEL[level].ms, LEVEL[level].noise, &sc2);
                if (m.from != m.to) play(m);
                think = 0;
            }
        }
    }
    gui_window_destroy(w);
    return 0;
}
