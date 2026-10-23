/* chess - play the computer: click or drag pieces, legal-move hints, castling, en passant, promotion,
 * check / mate / stalemate / draw detection, three difficulty levels. Rules and search live in engine.c. */
#include "../game.h"
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

static color_t SQ_LIGHT, SQ_DARK;      /* two quiet warm neutrals, a step brighter on light themes */
static void board_colours(void) {
    SQ_LIGHT = ui_th.dark ? RGB(0xC9, 0xC5, 0xBC) : RGB(0xE8, 0xE5, 0xDE);
    SQ_DARK = ui_th.dark ? RGB(0x8E, 0x89, 0x7F) : RGB(0xB3, 0xAE, 0xA3);
}

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
    if (over == ST_MATE) return cur_side() == human ? "Checkmate, the computer wins" : "Checkmate, you win";
    if (over == ST_STALEMATE) return "Stalemate, a draw";
    if (over == ST_INSUFFICIENT) return "Draw by insufficient material";
    if (over == ST_FIFTY) return "Draw by the fifty-move rule";
    if (over == 10) return "Draw by threefold repetition";
    if (cur_side() != human) return "Thinking…";
    snprintf(b, sizeof b, "%s to move%s", cur_side() == WHITE ? "White" : "Black", cp_in_check(&hist[nply], cur_side()) ? ", check" : "");
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
    if (lead > 0) { char s[8]; snprintf(s, sizeof s, "+%d", lead); gfx_text(ui->c, T_BODY_B, ox + SP_2, y + (24 - T_BODY_B->height) / 2, s, UI_TEXT2); }
}

int main(int argc, char **argv) {
    struct gui_window *w = gui_window_create("Chess", 840, 600, 0);
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
        cell = (W - PANEL_W - 3 * SP_5) / 8;
        if ((H - HEAD - 2 * SP_5) / 8 < cell) cell = (H - HEAD - 2 * SP_5) / 8;
        if (cell < 40) cell = 40;
        bx = SP_5; by = HEAD + SP_5;
        board_colours();
        pieces_build(0, cell * 88 / 100);
        pieces_build(1, 24);
        int ps = cell * 88 / 100, off = (cell - ps) / 2;
        int px0 = bx + 8 * cell + SP_5, pw = W - px0 - SP_5;

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
        game_head(&ui, W);
        int rx = W - SP_5;
        if (game_btn(&ui, &rx, UI_NOICON, "New game", UI_SECONDARY)) new_game();
        if (game_btn(&ui, &rx, ICON_SORT, NULL, UI_GHOST)) flipped = !flipped;
        ui_tooltip(&ui, rx + SP_3, CTL_Y, H_CTL, H_CTL, "Flip board · F");
        if (game_btn(&ui, &rx, ICON_UNDO, NULL, UI_GHOST | (nply ? 0 : UI_DISABLED))) undo();
        ui_tooltip(&ui, rx + SP_3, CTL_Y, H_CTL, H_CTL, "Undo · U");
        {
            const char *names[2] = { "White", "Black" };
            int sw = 3 * SP_1;
            for (int i = 0; i < 2; i++) sw += gfx_text_width(T_BODY, names[i]) + 2 * SP_4;
            rx -= sw;
            side_tab = human;
            if (ui_segmented(&ui, rx, CTL_Y, NULL, names, 2, &side_tab) && side_tab != human) { human = side_tab; new_game(); }
            ui_tooltip(&ui, rx, CTL_Y, sw, H_CTL, "Play as");
            rx -= SP_3 + 112;
            const char *lv[3] = { LEVEL[0].name, LEVEL[1].name, LEVEL[2].name };
            if (ui_dropdown(&ui, rx, CTL_Y, 112, lv, 3, &lv_tab, 0)) level = lv_tab;
            ui_tooltip(&ui, rx, CTL_Y, 112, H_CTL, "Difficulty");
        }
        {
            color_t sc = over ? (over == ST_MATE && cur_side() != human ? UI_GOOD : over == ST_MATE ? UI_BAD : UI_TEXT2) : UI_TEXT;
            int sx = SP_5;
            if (!over && cur_side() != human) { ui_spinner(&ui, sx, (HEAD - ICON_SM) / 2); sx += ICON_SM + SP_3; }
            ui_text_fit(&ui, T_BODY_B, sx, (HEAD - T_BODY_B->height) / 2, rx - SP_5 - sx, status_text(), sc);
        }

        for (int sq = 0; sq < 64; sq++) {
            int x = sq_x(sq), y = sq_y(sq), f = sq & 7, r = sq >> 3;
            gfx_fill(c, x, y, cell, cell, ((f + r) & 1) ? SQ_LIGHT : SQ_DARK);
        }
        /* last move and selection in amber, check in the danger colour */
        if (nply > 0) {
            gfx_fill(c, sq_x(mv[nply - 1].from), sq_y(mv[nply - 1].from), cell, cell, TH_A(UI_ACCENT, 70));
            gfx_fill(c, sq_x(mv[nply - 1].to), sq_y(mv[nply - 1].to), cell, cell, TH_A(UI_ACCENT, 100));
        }
        if (sel >= 0) gfx_fill(c, sq_x(sel), sq_y(sel), cell, cell, TH_A(UI_ACCENT, 150));
        if (cp_in_check(&hist[nply], cur_side()))
            for (int i = 0; i < 64; i++) if (hist[nply].b[i] == (cur_side() == WHITE ? K_ : -K_))
                gfx_fill(c, sq_x(i), sq_y(i), cell, cell, TH_A(ui_th.danger, 120));
        for (int sq = 0; sq < 64; sq++) {                 /* file / rank labels */
            int x = sq_x(sq), y = sq_y(sq), f = sq & 7, r = sq >> 3;
            color_t lc = ((f + r) & 1) ? SQ_DARK : SQ_LIGHT;
            if (r == (flipped ? 7 : 0)) { char s[2] = { 'a' + f, 0 }; gfx_text(c, T_CAPTION_B, x + cell - SP_2 - gfx_text_width(T_CAPTION_B, s), y + cell - SP_1 - T_CAPTION_B->height, s, lc); }
            if (f == (flipped ? 7 : 0)) { char s[2] = { '1' + r, 0 }; gfx_text(c, T_CAPTION_B, x + SP_2, y + SP_1, s, lc); }
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
        color_t hint = RGBA(0x1B, 0x1C, 0x1F, 64);
        if (sel >= 0 && human_turn) {
            for (int i = 0; i < nlegal; i++) {
                if (legal[i].from != sel) continue;
                int to = legal[i].to, x = sq_x(to), y = sq_y(to);
                bool cap = hist[nply].b[to] != 0 || (hist[nply].b[sel] * hist[nply].b[sel] == 1 && to == hist[nply].ep);
                if (cap) {
                    struct canvas cc = *c;
                    canvas_clip(&cc, x, y, cell, cell);
                    gfx_circle_fill(&cc, x + cell / 2, y + cell / 2, cell * 3 / 5, hint);
                    gfx_circle_fill(&cc, x + cell / 2, y + cell / 2, cell * 3 / 5 - 5, ((to & 7) + (to >> 3)) & 1 ? SQ_LIGHT : SQ_DARK);
                    draw_piece(&ui, 0, hist[nply].b[to] ? hist[nply].b[to] : -hist[nply].b[sel], x + off, y + off, ps);
                } else gfx_circle_fill(c, x + cell / 2, y + cell / 2, cell / 8, hint);
            }
        }
        if (dragging && drag_from >= 0) {
            int hs = sq_at(ui.mx, ui.my);
            if (hs >= 0) gfx_rrect_ring(c, sq_x(hs), sq_y(hs), cell, cell, 0, 2, ui_th.accent_line);
            draw_piece(&ui, 0, hist[nply].b[drag_from], drag_x - ps / 2, drag_y - ps / 2, ps);
        }
        if (promo_from >= 0) {                                /* promotion chooser: a strip of pieces from the target square inward */
            gfx_fill(c, bx, by, 8 * cell, 8 * cell, ui_th.scrim);
            int tx = sq_x(promo_to), ty0 = sq_y(promo_to), down = ty0 < by + 4 * cell;
            static const int order[4] = { Q_, R_, B_, N_ };
            for (int i = 0; i < 4; i++) {
                int yy = down ? ty0 + i * cell : ty0 - i * cell;
                bool hov = ui_hover(&ui, tx, yy, cell, cell);
                gfx_round_fill(c, tx + SP_1, yy + SP_1, cell - 2 * SP_1, cell - 2 * SP_1, R_2, hov ? mix(SQ_LIGHT, UI_ACCENT, 90) : SQ_LIGHT);
                draw_piece(&ui, 0, human == WHITE ? order[i] : -order[i], tx + off, yy + off, ps);
            }
        }
        if (over) {
            const char *t = over == ST_MATE ? "Checkmate" : over == ST_STALEMATE ? "Stalemate" : "Draw";
            const char *body = over == ST_MATE ? (cur_side() == human ? "The computer wins this one." : "You win.") : status_text();
            int r = game_card(&ui, bx, by, 8 * cell, 8 * cell, t, body, nply ? "Undo" : NULL, "New game");
            if (r == 1) new_game();
            else if (r == 2) undo();
        }

        /* ---- side panel: captures and the move list ---- */
        ui_card(&ui, px0, by, pw, 8 * cell);
        int y = by + SP_4, ix = px0 + SP_4, iw = pw - 2 * SP_4;
        int on_board = 0;
        for (int i = 0; i < 64; i++) on_board += hist[nply].b[i] != 0;
        if (on_board == 32) gfx_text(c, T_BODY, ix, y + 28 - T_BODY->height / 2, "No captures yet", UI_TEXT3);
        captured_row(&ui, ix, y, human == WHITE ? BLACK : WHITE);   /* what the AI has taken from you */
        y += 28;
        captured_row(&ui, ix, y, human);
        y += 24 + SP_4;
        gfx_hline(c, px0, y, pw, UI_LINE);
        y += SP_4;
        gfx_text(c, T_CAPTION_B, ix, y, "Moves", UI_TEXT2);
        y += T_CAPTION_B->height + SP_2;
        int ly = y, lh = by + 8 * cell - SP_3 - ly, rows = (nply + 1) / 2, rh = H_ROW, vis = lh / rh;
        if (rows > vis && list_stick) list_first = rows - vis;
        if (ui_hover(&ui, ix, ly, iw, lh) && ui.scroll) { list_first += ui.scroll; list_stick = false; }
        if (list_first > rows - vis) { list_first = rows - vis; }
        if (list_first < 0) list_first = 0;
        struct canvas lc = *c;
        canvas_clip(&lc, px0, ly, pw, lh);
        int mw = (iw - 40) / 2;
        for (int r = list_first; r < rows && r < list_first + vis + 1; r++) {
            int yy = ly + (r - list_first) * rh, ty = yy + (rh - T_BODY->height) / 2;
            char num[8];
            snprintf(num, sizeof num, "%d.", r + 1);
            gfx_text(&lc, T_BODY, ix + 28 - gfx_text_width(T_BODY, num), ty, num, UI_TEXT3);
            for (int s = 0; s < 2 && r * 2 + s < nply; s++) {
                int idx = r * 2 + s, xx = ix + 40 + s * mw;
                bool cur = idx == nply - 1;
                if (cur) gfx_round_fill(&lc, xx - SP_3, yy + SP_1, mw - SP_2, rh - 2 * SP_1, R_2, UI_SELECTION);
                gfx_text(&lc, cur ? T_BODY_B : T_BODY, xx, ty, san[idx], UI_TEXT);
            }
        }
        if (!nply) gfx_text_center(c, T_BODY, ix, ly + lh / 2 - T_BODY->height, iw, "No moves yet", UI_TEXT2);
        {
            int first = list_first;
            ui_vscroll(&ui, px0 + pw - 12, ly, lh, rows, vis, &first);
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
