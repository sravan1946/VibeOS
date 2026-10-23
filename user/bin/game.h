/* game.h - the frame the games share (docs/DESIGN.md §9): a 40px header (stats left, actions right), the board
 * well, the end-of-game / pause card over the board, and the muted content palette. Static helpers, header only. */
#pragma once
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>

#define HEAD H_TOOLBAR                      /* header row height */
#define CTL_Y ((H_TOOLBAR - H_CTL) / 2)     /* controls in the header */

/* content hues (games only): the theme's calm ANSI palette, 1 red 2 green 3 yellow 4 blue 5 magenta 6 cyan.
 * As text they keep >= 4.5:1 on window/base; game_fill softens them for large fills on light themes. */
static inline color_t game_hue(int i) { return ui_th.term.pal[i]; }
static inline color_t game_fill(int i) { return ui_th.dark ? ui_th.term.pal[i] : mix(ui_th.term.pal[i], ui_th.window, 72); }

static inline void game_head(struct ui *ui, int w) { ui_toolbar(ui, 0, 0, w); }

/* "Score 120" at x in the header; returns the x where the next stat goes */
static inline int game_stat(struct ui *ui, int x, const char *label, const char *val) {
    int y = (HEAD - T_BODY->height) / 2;
    gfx_text(ui->c, T_BODY, x, y, label, UI_TEXT2);
    x += gfx_text_width(T_BODY, label) + 6;
    gfx_text(ui->c, T_BODY_B, x, y, val, UI_TEXT);
    return x + gfx_text_width(T_BODY_B, val) + SP_6;
}
static inline int game_stati(struct ui *ui, int x, const char *label, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    return game_stat(ui, x, label, b);
}

/* a header button laid out right to left: *rx is the free right edge, moved left past the button */
static inline bool game_btn(struct ui *ui, int *rx, int icon, const char *label, int style) {
    int w = label ? ui_button_w(icon, label) : H_CTL;
    *rx -= w;
    bool r = ui_button_ex(ui, *rx, CTL_Y, w, H_CTL, icon, label, style);
    *rx -= SP_3;
    return r;
}

static inline void game_well(struct ui *ui, int x, int y, int w, int h) { gfx_round_fill(ui->c, x, y, w, h, R_3, ui_th.base); }

/* a dialog-style card centred over the board (x, y, w, h) behind a scrim. body / sec may be NULL, pri too (no buttons).
 * Returns 1 when pri was clicked, 2 for sec, else 0. The game keeps handling its own keys. */
static inline int game_card(struct ui *ui, int x, int y, int w, int h, const char *title, const char *body, const char *sec, const char *pri) {
    struct canvas *c = ui->c;
    gfx_round_fill(c, x, y, w, h, R_3, ui_th.scrim);
    int pw = w - 2 * SP_5 < 320 ? w - 2 * SP_5 : 320, tw = pw - 2 * SP_6;
    int bh = body ? ui_text_wrap(ui, T_BODY, 0, 0, tw, body, 0) : 0;
    int ph = SP_6 + T_HEADING->height + (body ? SP_3 + bh : 0) + (pri ? SP_6 + H_CTL : 0) + SP_6;
    int px = x + (w - pw) / 2, py = y + (h - ph) / 2, r = 0;
    gfx_elev(c, px, py, pw, ph, R_3, E2, ui_th.elev[E2]);
    gfx_round_fill(c, px, py, pw, ph, R_3, ui_th.overlay);
    gfx_round_rect(c, px, py, pw, ph, R_3, ui_th.line);
    gfx_text(c, T_HEADING, px + SP_6, py + SP_6, title, UI_TEXT);
    if (body) ui_text_wrap(ui, T_BODY, px + SP_6, py + SP_6 + T_HEADING->height + SP_3, tw, body, UI_TEXT2);
    if (!pri) return 0;
    int by = py + ph - SP_6 - H_CTL, ow = ui_button_w(UI_NOICON, pri), bx = px + pw - SP_6;
    if (ow < W_CTL_MIN) ow = W_CTL_MIN;
    bx -= ow;
    if (ui_button(ui, bx, by, ow, H_CTL, pri, UI_PRIMARY)) r = 1;
    if (sec) {
        int sw = ui_button_w(UI_NOICON, sec);
        if (sw < W_CTL_MIN) sw = W_CTL_MIN;
        if (ui_button(ui, bx - SP_3 - sw, by, sw, H_CTL, sec, UI_SECONDARY)) r = 2;
    }
    return r;
}
