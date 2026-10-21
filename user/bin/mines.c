/* mines - minesweeper in the VibeOS style */
#include <ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define CS 24
#define PAD 16
#define HEAD 104
#define MAXW 30
#define MAXH 16

enum { READY, PLAY, WON, LOST };
enum { HIDDEN, OPEN, FLAG };

static const struct { const char *name; int w, h, mines; } LEVELS[3] = {
    { "Beginner", 9, 9, 10 }, { "Intermediate", 16, 16, 40 }, { "Expert", 30, 16, 99 },
};
static const color_t NUMCOL[9] = {
    0, RGB(0x6C, 0x9C, 0xFF), RGB(0x4B, 0xE0, 0x8A), RGB(0xFF, 0x5F, 0x7A), RGB(0xB3, 0x88, 0xFF),
    RGB(0xFF, 0xA2, 0x4B), RGB(0x4B, 0xD8, 0xF5), RGB(0xFF, 0x7A, 0xC6), RGB(0x9A, 0x96, 0xB8),
};

static int lvl, cols, rows, nmines, state, flags, opened, boom_x, boom_y;
static unsigned char mine[MAXH][MAXW], nb[MAXH][MAXW], st[MAXH][MAXW];
static uint32_t t_start, t_final;

static int win_w(void) { int w = cols * CS + 2 * PAD; return w < 344 ? 344 : w; }
static int win_h(void) { return HEAD + rows * CS + PAD; }
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
static void draw_flag(struct canvas *c, int x, int y) {
    gfx_fill(c, x + 10, y + 5, 2, 14, UI_TEXT);
    for (int i = 0; i < 7; i++) gfx_fill(c, x + 12, y + 5 + i, 7 - i, 1, UI_ACCENT2);
    gfx_fill(c, x + 7, y + 18, 9, 2, UI_TEXT_DIM);
}
static void draw_mine(struct canvas *c, int x, int y) {
    static const int d[4][2] = { {1,0}, {0,1}, {1,1}, {1,-1} };
    int cx = x + CS / 2, cy = y + CS / 2;
    for (int i = 0; i < 4; i++)
        gfx_line_aa(c, cx - d[i][0] * 8, cy - d[i][1] * 8, cx + d[i][0] * 8, cy + d[i][1] * 8, 1.6f, RGB(0xE8, 0xDF, 0xFF));
    gfx_circle_fill(c, cx, cy, 6, RGB(0x2A, 0x20, 0x44));
    gfx_circle_fill(c, cx - 2, cy - 2, 2, RGB(255, 255, 255));
}

static void smiley(struct ui *ui, int x, int y, bool scared) {
    struct canvas *c = ui->c;
    bool hov = ui_hover(ui, x, y, 40, 40);
    gfx_round_fill(c, x, y, 40, 40, 10, hov ? UI_HOVER : UI_SURFACE);
    gfx_round_rect(c, x, y, 40, 40, 10, UI_BORDER);
    int cx = x + 20, cy = y + 20;
    gfx_circle_fill(c, cx, cy, 13, state == LOST ? RGB(0xFF, 0x9A, 0x7A) : UI_WARN);
    color_t k = RGB(0x2A, 0x1A, 0x40);
    if (state == LOST) {
        for (int s = -1; s <= 1; s += 2) {
            gfx_line_aa(c, cx + s * 5 - 2, cy - 6, cx + s * 5 + 2, cy - 2, 1.6f, k);
            gfx_line_aa(c, cx + s * 5 + 2, cy - 6, cx + s * 5 - 2, cy - 2, 1.6f, k);
        }
        gfx_line_aa(c, cx - 5, cy + 7, cx, cy + 4, 1.6f, k);
        gfx_line_aa(c, cx, cy + 4, cx + 5, cy + 7, 1.6f, k);
    } else if (state == WON) {
        gfx_fill(c, cx - 9, cy - 6, 8, 5, k); gfx_fill(c, cx + 1, cy - 6, 8, 5, k);
        gfx_fill(c, cx - 1, cy - 6, 2, 2, k);
        gfx_line_aa(c, cx - 5, cy + 3, cx - 2, cy + 6, 1.6f, k);
        gfx_line_aa(c, cx - 2, cy + 6, cx + 2, cy + 6, 1.6f, k);
        gfx_line_aa(c, cx + 2, cy + 6, cx + 5, cy + 3, 1.6f, k);
    } else {
        gfx_circle_fill(c, cx - 5, cy - 3, 2, k); gfx_circle_fill(c, cx + 5, cy - 3, 2, k);
        if (scared) gfx_circle_fill(c, cx, cy + 6, 3, k);
        else {
            gfx_line_aa(c, cx - 6, cy + 3, cx - 3, cy + 6, 1.6f, k);
            gfx_line_aa(c, cx - 3, cy + 6, cx + 3, cy + 6, 1.6f, k);
            gfx_line_aa(c, cx + 3, cy + 6, cx + 6, cy + 3, 1.6f, k);
        }
    }
}

static void counter(struct canvas *c, int x, int y, int v, const char *label) {
    char b[16];
    snprintf(b, sizeof b, v < 0 ? "-%02d" : "%03d", v < 0 ? -v : v);
    gfx_round_fill(c, x, y, 84, 40, 8, RGB(0x10, 0x0F, 0x1A));
    gfx_round_rect(c, x, y, 84, 40, 8, UI_BORDER);
    gfx_text(c, &font_ui, x + 4, y - 16, label, UI_TEXT_DIM);
    gfx_text_center(c, &font_title, x, y + 8, 84, b, state == LOST ? UI_BAD : UI_ACCENT);
}

static void draw_board(struct ui *ui, bool *scared) {
    struct canvas *c = ui->c;
    int bx = bx0(), by = HEAD;
    gfx_shadow(c, bx - 4, by - 4, cols * CS + 8, rows * CS + 8, 8, 8, 110);
    gfx_round_fill(c, bx - 4, by - 4, cols * CS + 8, rows * CS + 8, 8, UI_PANEL);
    int hx = ui->mx >= bx ? (ui->mx - bx) / CS : -1, hy = ui->my >= by ? (ui->my - by) / CS : -1;
    if (hx >= cols || hy >= rows) hx = hy = -1;
    *scared = hx >= 0 && hy >= 0 && (ui->buttons & 1) && st[hy][hx] == HIDDEN && (state == READY || state == PLAY);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            int px = bx + x * CS, py = by + y * CS;
            bool hot = x == hx && y == hy && (state == READY || state == PLAY);
            char b[2] = { '0' + nb[y][x], 0 };
            bool showmine = state == LOST && mine[y][x] && st[y][x] != FLAG;
            if (st[y][x] == OPEN && !showmine) {
                gfx_round_fill(c, px + 1, py + 1, CS - 2, CS - 2, 4, RGB(0x14, 0x13, 0x20));
                if (nb[y][x]) gfx_text_center(c, &font_ui_bold, px, py + (CS - font_ui_bold.height) / 2, CS, b, NUMCOL[nb[y][x]]);
            } else if (showmine) {
                gfx_round_fill(c, px + 1, py + 1, CS - 2, CS - 2, 4, x == boom_x && y == boom_y ? RGB(0xB0, 0x30, 0x4A) : RGB(0x14, 0x13, 0x20));
                draw_mine(c, px, py);
            } else {
                color_t top = hot ? RGB(0x4A, 0x45, 0x6C) : RGB(0x3A, 0x36, 0x56), bot = hot ? RGB(0x38, 0x34, 0x54) : RGB(0x2A, 0x27, 0x3D);
                gfx_round_fill(c, px + 1, py + 1, CS - 2, CS - 2, 5, mix(top, RGB(255, 255, 255), 24));
                gfx_vgradient(c, px + 2, py + 2, CS - 4, CS - 4, top, bot);
                if (st[y][x] == FLAG) {
                    draw_flag(c, px, py);
                    if (state == LOST && !mine[y][x]) {
                        gfx_line_aa(c, px + 5, py + 5, px + CS - 5, py + CS - 5, 2.0f, UI_BAD);
                        gfx_line_aa(c, px + CS - 5, py + 5, px + 5, py + CS - 5, 2.0f, UI_BAD);
                    }
                }
            }
        }
}

int main(void) {
    struct gui_window *w = gui_window_create("Minesweeper", 344, win_h(), WIN_FLAG_NORESIZE);
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
        int bx = bx0(), by = HEAD;
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
        struct canvas *c = ui.c;
        gfx_vgradient(c, 0, 0, w->w, HEAD - 8, RGB(0x2C, 0x1B, 0x52), UI_BG);
        int bw3 = 104, gap = 6, x0 = (w->w - (3 * bw3 + 2 * gap)) / 2, newlvl = -1;
        for (int i = 0; i < 3; i++)
            if (ui_button(&ui, x0 + i * (bw3 + gap), 10, bw3, 28, LEVELS[i].name, i == lvl ? UI_ACCENT_BTN : UI_NORMAL_BTN)) newlvl = i;
        bool scared;
        draw_board(&ui, &scared);
        counter(c, 12, 62, nmines - flags, "MINES");
        uint32_t secs = state == READY ? 0 : ((state == PLAY ? now : t_final) - t_start) / 1000;
        counter(c, w->w - 12 - 84, 62, secs > 999 ? 999 : (int)secs, "TIME");
        smiley(&ui, w->w / 2 - 20, 62, scared);
        if (ui_clicked(&ui, w->w / 2 - 20, 62, 40, 40)) newlvl = lvl;

        if (over) {
            int bw = cols * CS, bh = rows * CS;
            gfx_round_fill(c, bx, by, bw, bh, 6, RGBA(0x0C, 0x0A, 0x16, 120));
            int cw = 200, ch = 112, cxx = bx + (bw - cw) / 2, cyy = by + (bh - ch) / 2;
            gfx_shadow(c, cxx, cyy, cw, ch, 12, 12, 160);
            ui_panel(&ui, cxx, cyy, cw, ch);
            char b[32];
            gfx_text_center(c, &font_title, cxx, cyy + 8, cw, state == WON ? "Cleared!" : "Boom!", state == WON ? UI_GOOD : UI_BAD);
            snprintf(b, sizeof b, "%us", (unsigned)((t_final - t_start) / 1000));
            gfx_text_center(c, &font_ui, cxx, cyy + 42, cw, b, UI_TEXT_DIM);
            if (ui_button(&ui, cxx + cw / 2 - 55, cyy + 66, 110, 34, "Play again", UI_ACCENT_BTN)) newlvl = lvl;
        }
        ui_end(&ui);
        if (newlvl >= 0) { new_game(newlvl); gui_resize(w, win_w(), win_h()); }
    }
    gui_window_destroy(w);
    return 0;
}
