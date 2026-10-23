/* mines - minesweeper */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define CS 24
#define PAD (SP_5 + SP_2)                     /* window edge to the cells: 16 margin + the well's 4px rim */
#define TOPY (HEAD + PAD)
#define MINW 400
#define MAXW 30
#define MAXH 16

enum { READY, PLAY, WON, LOST };
enum { HIDDEN, OPEN, FLAG };

static const struct { const char *name; int w, h, mines; } LEVELS[3] = {
    { "Beginner", 9, 9, 10 }, { "Intermediate", 16, 16, 40 }, { "Expert", 30, 16, 99 },
};
static color_t numcol(int n) {                    /* 1 blue 2 green 3 red 4 magenta 5 yellow 6 cyan 7 8 grey */
    static const signed char H[9] = { 0, 4, 2, 1, 5, 3, 6, 0, 0 };
    return n == 7 ? UI_TEXT : n == 8 ? UI_TEXT2 : game_hue(H[n]);
}

static int lvl, cols, rows, nmines, state, flags, opened, boom_x, boom_y;
static unsigned char mine[MAXH][MAXW], nb[MAXH][MAXW], st[MAXH][MAXW];
static uint32_t t_start, t_final;

static int win_w(void) { int w = cols * CS + 2 * PAD; return w < MINW ? MINW : w; }
static int win_h(void) { return TOPY + rows * CS + PAD; }
static int bx0(void) { return (win_w() - cols * CS) / 2; }

static void new_game(int level) {
    lvl = level; cols = LEVELS[level].w; rows = LEVELS[level].h; nmines = LEVELS[level].mines;
    memset(mine, 0, sizeof mine); memset(nb, 0, sizeof nb); memset(st, 0, sizeof st);
    state = READY; flags = opened = 0; boom_x = boom_y = -1; t_start = t_final = 0;
}

static void place_mines(int sx, int sy) {            /* first click and its neighbours are always clear */
    for (int n = 0; n < nmines; ) {
        int x = rand() % cols, y = rand() % rows;
        if (mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;
        mine[y][x] = 1; n++;
    }
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if ((dx || dy) && xx >= 0 && yy >= 0 && xx < cols && yy < rows && mine[yy][xx]) nb[y][x]++;
                }
}

static void lose(int x, int y) {
    state = LOST; boom_x = x; boom_y = y;
    t_final = (uint32_t)uptime_ms();
    beep(120, 140);
}
static void check_win(void) {
    if (opened != cols * rows - nmines) return;
    state = WON; t_final = (uint32_t)uptime_ms();
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) if (mine[y][x]) st[y][x] = FLAG;
    flags = nmines;
    beep(1046, 60);
}

static void reveal(int x, int y) {                   /* flood fill from zero cells */
    static short stack[MAXW * MAXH * 8][2];
    int sp = 0;
    if (st[y][x] != HIDDEN) return;
    stack[sp][0] = x; stack[sp++][1] = y;
    while (sp) {
        sp--;
        int cx = stack[sp][0], cy = stack[sp][1];
        if (st[cy][cx] != HIDDEN) continue;
        st[cy][cx] = OPEN; opened++;
        if (mine[cy][cx]) { lose(cx, cy); return; }
        if (nb[cy][cx]) continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int xx = cx + dx, yy = cy + dy;
                if (xx >= 0 && yy >= 0 && xx < cols && yy < rows && st[yy][xx] == HIDDEN && sp < MAXW * MAXH * 8)
                    { stack[sp][0] = xx; stack[sp++][1] = yy; }
            }
    }
}

static void click(int x, int y) {
    if (state == READY) { place_mines(x, y); state = PLAY; t_start = (uint32_t)uptime_ms(); }
    if (state != PLAY) return;
    if (st[y][x] == OPEN && nb[y][x]) {              /* chord */
        int f = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < cols && yy < rows && st[yy][xx] == FLAG) f++;
            }
        if (f != nb[y][x]) return;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < cols && yy < rows && state == PLAY) reveal(xx, yy);
            }
    } else reveal(x, y);
    if (state == PLAY) check_win();
}

static void flag(int x, int y) {
    if (state != PLAY && state != READY) return;
    if (st[y][x] == HIDDEN) { st[y][x] = FLAG; flags++; beep(500, 8); }
    else if (st[y][x] == FLAG) { st[y][x] = HIDDEN; flags--; }
}

/* ---- drawing ---- */
static void draw_board(struct ui *ui) {
    struct canvas *c = ui->c;
    int bx = bx0(), by = TOPY;
    game_well(ui, bx - SP_2, by - SP_2, cols * CS + 2 * SP_2, rows * CS + 2 * SP_2);
    int hx = ui->mx >= bx ? (ui->mx - bx) / CS : -1, hy = ui->my >= by ? (ui->my - by) / CS : -1;
    if (hx >= cols || hy >= rows) hx = hy = -1;
    color_t tile = mix(ui_th.base, ui_th.control, ui_th.dark ? 100 : 80);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            int px = bx + x * CS, py = by + y * CS, ix = px + (CS - ICON_SM) / 2, iy = py + (CS - ICON_SM) / 2;
            bool hot = x == hx && y == hy && (state == READY || state == PLAY);
            char b[2] = { '0' + nb[y][x], 0 };
            bool showmine = state == LOST && mine[y][x] && st[y][x] != FLAG;
            if (st[y][x] == OPEN && !showmine) {
                if (nb[y][x]) gfx_text_center(c, T_BODY_B, px, py + (CS - T_BODY_B->height) / 2, CS, b, numcol(nb[y][x]));
            } else if (showmine) {
                bool boom = x == boom_x && y == boom_y;
                if (boom) gfx_round_fill(c, px + 1, py + 1, CS - 2, CS - 2, R_2, ui_th.danger);
                ui_icon(ui, ix, iy, ICON_BOMB, ICON_SM, boom ? ui_th.on_danger : UI_TEXT);
            } else {
                gfx_round_fill(c, px + 1, py + 1, CS - 2, CS - 2, R_2, tile);
                if (hot) gfx_round_fill(c, px + 1, py + 1, CS - 2, CS - 2, R_2, (ui->buttons & 1) ? ui_th.press_ov : ui_th.hover_ov);
                if (st[y][x] == FLAG) {
                    if (state == LOST && !mine[y][x]) ui_icon(ui, ix, iy, ICON_CLOSE, ICON_SM, ui_th.danger);
                    else ui_icon(ui, ix, iy, ICON_FLAG, ICON_SM, game_hue(1));
                }
            }
        }
}

/* icon + value in the header */
static int stat_icon(struct ui *ui, int x, int icon, const char *val, const char *tip) {
    ui_icon(ui, x, (HEAD - ICON_SM) / 2, icon, ICON_SM, UI_TEXT2);
    gfx_text(ui->c, T_BODY_B, x + ICON_SM + 6, (HEAD - T_BODY_B->height) / 2, val, UI_TEXT);
    int w = ICON_SM + 6 + gfx_text_width(T_BODY_B, val);
    ui_tooltip(ui, x, CTL_Y, w, H_CTL, tip);
    return x + w + SP_6;
}

int main(void) {
    struct gui_window *w = gui_window_create("Minesweeper", MINW, win_h(), WIN_FLAG_NORESIZE);
    if (!w) { perror("mines"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    srand((unsigned)uptime_ms());
    new_game(0);
    gui_resize(w, win_w(), win_h());
    for (;;) {
        if (!ui_poll(&ui, 200)) break;
        if (w->w != win_w() || w->h != win_h()) continue;     /* wait for EV_RESIZE after a level switch */
        uint32_t now = (uint32_t)uptime_ms();
        int bx = bx0(), by = TOPY;
        int cx = ui.mx >= bx ? (ui.mx - bx) / CS : -1, cy = ui.my >= by ? (ui.my - by) / CS : -1;
        bool inb = cx >= 0 && cy >= 0 && cx < cols && cy < rows;
        bool over = state == WON || state == LOST;
        if (inb && !over) {
            if (ui.pressed) click(cx, cy);
            if (ui.rpressed) flag(cx, cy);
        }
        struct gui_event k;
        while (ui_key(&ui, &k)) if (k.key == 'n' || k.key == KEY_F2) new_game(lvl);

        ui_clear(&ui);
        game_head(&ui, w->w);
        char b[48];
        snprintf(b, sizeof b, "%d", nmines - flags);
        int x = stat_icon(&ui, SP_5, ICON_FLAG, b, "Mines left");
        uint32_t secs = state == READY ? 0 : ((state == PLAY ? now : t_final) - t_start) / 1000;
        snprintf(b, sizeof b, "%u", secs > 999 ? 999 : (unsigned)secs);
        stat_icon(&ui, x, ICON_CLOCK, b, "Time");
        int rx = w->w - SP_5, newlvl = -1;
        if (game_btn(&ui, &rx, UI_NOICON, "New game", UI_SECONDARY)) newlvl = lvl;
        const char *names[3] = { LEVELS[0].name, LEVELS[1].name, LEVELS[2].name };
        int sel = lvl;
        rx -= 132;
        if (ui_dropdown(&ui, rx, CTL_Y, 132, names, 3, &sel, 0)) newlvl = sel;
        draw_board(&ui);
        if (over) {
            unsigned t = (t_final - t_start) / 1000;
            if (state == WON) snprintf(b, sizeof b, "Cleared in %u second%s.", t, t == 1 ? "" : "s");
            else snprintf(b, sizeof b, "You hit a mine after %u second%s.", t, t == 1 ? "" : "s");
            if (game_card(&ui, 0, HEAD, w->w, w->h - HEAD, state == WON ? "Field cleared" : "Game over", b, NULL, "Play again")) newlvl = lvl;
        }
        ui_end(&ui);
        if (newlvl >= 0) { new_game(newlvl); gui_resize(w, win_w(), win_h()); }
    }
    gui_window_destroy(w);
    return 0;
}
