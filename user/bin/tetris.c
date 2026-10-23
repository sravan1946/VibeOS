/* tetris - SRS-ish falling blocks */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define BW 10
#define BH 20
#define CELL 26
#define PAD SP_5
#define TOP (HEAD + PAD)
#define SIDE_X (PAD + BW * CELL + PAD)
#define SIDE_W 132
#define WIN_W (SIDE_X + SIDE_W + PAD)
#define WIN_H (TOP + BH * CELL + PAD)
#define BEST_PATH "/home/.tetris_best"

#define DAS_DELAY 160
#define DAS_RATE 40
#define SOFT_RATE 35
#define LOCK_DELAY 500
#define CLEAR_MS 380

enum { I_, J_, L_, O_, S_, T_, Z_ };
enum { PLAY, PAUSED, CLEARING, DEAD };

/* I J L O S T Z in the muted content hues */
static color_t piece_col(int t) {
    static const signed char H[7] = { 6, 4, -1, 3, 2, 5, 1 };
    return H[t] < 0 ? mix(game_fill(1), game_fill(3), 128) : game_fill(H[t]);
}
/* rotation-0 cells in the SRS bounding box; box size n (O never rotates) */
static const signed char SHAPE[7][4][2] = {
    { {0,1},{1,1},{2,1},{3,1} }, { {0,0},{0,1},{1,1},{2,1} }, { {2,0},{0,1},{1,1},{2,1} },
    { {0,0},{1,0},{0,1},{1,1} }, { {1,0},{2,0},{0,1},{1,1} }, { {1,0},{0,1},{1,1},{2,1} },
    { {0,0},{1,0},{1,1},{2,1} },
};
static const int BOXN[7] = { 4, 3, 3, 2, 3, 3, 3 };
/* SRS kick tables, y-up as published; [from state] for clockwise / counter-clockwise */
static const signed char KJ_CW[4][5][2] = {
    { {0,0},{-1,0},{-1,1},{0,-2},{-1,-2} }, { {0,0},{1,0},{1,-1},{0,2},{1,2} },
    { {0,0},{1,0},{1,1},{0,-2},{1,-2} },    { {0,0},{-1,0},{-1,-1},{0,2},{-1,2} } };
static const signed char KJ_CCW[4][5][2] = {
    { {0,0},{1,0},{1,1},{0,-2},{1,-2} },    { {0,0},{1,0},{1,-1},{0,2},{1,2} },
    { {0,0},{-1,0},{-1,1},{0,-2},{-1,-2} }, { {0,0},{-1,0},{-1,-1},{0,2},{-1,2} } };
static const signed char KI_CW[4][5][2] = {
    { {0,0},{-2,0},{1,0},{-2,-1},{1,2} },  { {0,0},{-1,0},{2,0},{-1,2},{2,-1} },
    { {0,0},{2,0},{-1,0},{2,1},{-1,-2} },  { {0,0},{1,0},{-2,0},{1,-2},{-2,1} } };
static const signed char KI_CCW[4][5][2] = {
    { {0,0},{-1,0},{2,0},{-1,2},{2,-1} },  { {0,0},{2,0},{-1,0},{2,1},{-1,-2} },
    { {0,0},{1,0},{-2,0},{1,-2},{-2,1} },  { {0,0},{-2,0},{1,0},{-2,-1},{1,2} } };
static const int GRAV[] = { 800, 717, 633, 550, 467, 383, 300, 217, 133, 100, 83, 67, 50 };

static signed char board[BH][BW];            /* -1 empty, else piece type */
static int cur, rot, px, py, hold, can_hold, nextq[3], bag[7], bag_i;
static int score, lines, level, best, state;
static int clear_rows[4], nclear;
static uint32_t last_drop, lock_t, clear_t, das_t, das_last, flash_t;
static int lowest, resets;
static bool on_ground, hold_left, hold_right, hold_soft, dirdown[512];
static int das_dir;
static char flash_msg[24];

static int load_best(void) {
    FILE *f = fopen(BEST_PATH, "r");
    int v = 0;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
    return v;
}
static void save_best(void) {
    FILE *f = fopen(BEST_PATH, "w");
    if (f) { fprintf(f, "%d\n", best); fclose(f); }
}

static int bag_next(void) {
    if (bag_i >= 7) {
        for (int i = 0; i < 7; i++) bag[i] = i;
        for (int i = 6; i > 0; i--) { int j = rand() % (i + 1), t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
        bag_i = 0;
    }
    return bag[bag_i++];
}

static void cells(int type, int r, int out[4][2]) {
    int n = BOXN[type];
    for (int i = 0; i < 4; i++) {
        int x = SHAPE[type][i][0], y = SHAPE[type][i][1];
        for (int k = 0; k < (type == O_ ? 0 : r); k++) { int t = x; x = n - 1 - y; y = t; }
        out[i][0] = x; out[i][1] = y;
    }
}
static bool fits(int type, int r, int x, int y) {
    int c[4][2];
    cells(type, r, c);
    for (int i = 0; i < 4; i++) {
        int bx = x + c[i][0], by = y + c[i][1];
        if (bx < 0 || bx >= BW || by >= BH) return false;
        if (by >= 0 && board[by][bx] >= 0) return false;
    }
    return true;
}

static void spawn(int type) {
    cur = type; rot = 0;
    px = type == O_ ? 4 : 3;
    py = type == I_ ? -1 : 0;
    lowest = py; resets = 0; on_ground = false;
    last_drop = (uint32_t)uptime_ms();
    if (!fits(cur, rot, px, py)) {
        state = DEAD;
        if (score > best) { best = score; save_best(); }
        beep(140, 150);
    }
}
static void spawn_next(void) {
    int t = nextq[0];
    nextq[0] = nextq[1]; nextq[1] = nextq[2]; nextq[2] = bag_next();
    spawn(t);
}

static void reset(void) {
    memset(board, -1, sizeof board);
    score = lines = level = 0; hold = -1; can_hold = 1; state = PLAY;
    bag_i = 7;
    for (int i = 0; i < 3; i++) nextq[i] = bag_next();
    hold_left = hold_right = hold_soft = false; das_dir = 0;
    flash_msg[0] = 0;
    spawn_next();
}

static int gravity(void) { int l = level < 13 ? level : 12; return GRAV[l]; }

static void touch_lock(uint32_t now) {      /* a successful shift/rotate while grounded extends the lock timer */
    if (!fits(cur, rot, px, py + 1)) {
        if (resets < 15) { lock_t = now; resets++; }
        on_ground = true;
    } else on_ground = false;
}
static bool shift(int dx, uint32_t now) {
    if (!fits(cur, rot, px + dx, py)) return false;
    px += dx; touch_lock(now);
    return true;
}
static void rotate(int dir, uint32_t now) {      /* dir +1 cw, -1 ccw */
    if (cur == O_) return;
    int to = (rot + dir + 4) & 3;
    const signed char (*k)[2] = cur == I_ ? (dir > 0 ? KI_CW[rot] : KI_CCW[rot]) : (dir > 0 ? KJ_CW[rot] : KJ_CCW[rot]);
    for (int i = 0; i < 5; i++) {
        int nx = px + k[i][0], ny = py - k[i][1];
        if (fits(cur, to, nx, ny)) { px = nx; py = ny; rot = to; touch_lock(now); return; }
    }
}
static int ghost_y(void) { int y = py; while (fits(cur, rot, px, y + 1)) y++; return y; }

static void lock_piece(uint32_t now) {
    int c[4][2];
    cells(cur, rot, c);
    for (int i = 0; i < 4; i++) {
        int bx = px + c[i][0], by = py + c[i][1];
        if (by >= 0) board[by][bx] = cur;
    }
    nclear = 0;
    for (int y = 0; y < BH; y++) {
        bool full = true;
        for (int x = 0; x < BW; x++) if (board[y][x] < 0) { full = false; break; }
        if (full && nclear < 4) clear_rows[nclear++] = y;
    }
    can_hold = 1;
    if (nclear) {
        state = CLEARING; clear_t = now;
        static const int pts[5] = { 0, 100, 300, 500, 800 };
        score += pts[nclear] * (level + 1);
        static const char *names[5] = { "", "Single", "Double", "Triple", "Tetris" };
        snprintf(flash_msg, sizeof flash_msg, "%s", names[nclear]);
        flash_t = now;
        beep(nclear == 4 ? 988 : 660, nclear == 4 ? 50 : 25);
    } else spawn_next();
}
static void finish_clear(void) {
    for (int i = 0; i < nclear; i++) {
        int y = clear_rows[i];
        memmove(board[1], board[0], y * BW);
        memset(board[0], -1, BW);
    }
    lines += nclear;
    level = lines / 10;
    nclear = 0;
    state = PLAY;
    spawn_next();
}

static void hard_drop(uint32_t now) {
    int y = ghost_y();
    score += 2 * (y - py);
    py = y;
    lock_piece(now);
}
static void do_hold(void) {
    if (!can_hold) return;
    int t = hold;
    hold = cur; can_hold = 0;
    if (t < 0) spawn_next(); else spawn(t);
}

/* ---- drawing ---- */
static void draw_cell(struct canvas *c, int x, int y, int s, color_t col) { gfx_round_fill(c, x + 1, y + 1, s - 2, s - 2, R_1, col); }
static void mini(struct canvas *c, int type, int cx, int cy, int s) {
    int cc[4][2], minx = 9, maxx = 0, miny = 9, maxy = 0;
    cells(type, 0, cc);
    for (int i = 0; i < 4; i++) {
        if (cc[i][0] < minx) minx = cc[i][0];
        if (cc[i][0] > maxx) maxx = cc[i][0];
        if (cc[i][1] < miny) miny = cc[i][1];
        if (cc[i][1] > maxy) maxy = cc[i][1];
    }
    int ox = cx - (maxx - minx + 1) * s / 2 - minx * s, oy = cy - (maxy - miny + 1) * s / 2 - miny * s;
    for (int i = 0; i < 4; i++) draw_cell(c, ox + cc[i][0] * s, oy + cc[i][1] * s, s, piece_col(type));
}
static void side_row(struct ui *ui, int y, const char *k, int v, bool first) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    ui_card_row(ui, SIDE_X, y, SIDE_W, H_ROW + SP_2, k, NULL, first);
    gfx_text(ui->c, T_BODY_B, SIDE_X + SIDE_W - SP_4 - gfx_text_width(T_BODY_B, b), y + (H_ROW + SP_2 - T_BODY->height) / 2, b, UI_TEXT);
}

static void draw(struct ui *ui, uint32_t now) {
    struct canvas *c = ui->c;
    ui_clear(ui);
    game_head(ui, WIN_W);
    game_stati(ui, game_stati(ui, PAD, "Score", score), "Best", best);
    int rx = WIN_W - PAD;
    if (game_btn(ui, &rx, UI_NOICON, "New game", UI_SECONDARY)) reset();

    int bx = PAD, by = TOP, bw = BW * CELL, bh = BH * CELL;
    game_well(ui, bx, by, bw, bh);
    for (int x = 1; x < BW; x++) gfx_fill(c, bx + x * CELL, by, 1, bh, TH_A(UI_TEXT, 8));
    for (int y = 1; y < BH; y++) gfx_fill(c, bx, by + y * CELL, bw, 1, TH_A(UI_TEXT, 8));

    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            if (board[y][x] >= 0) draw_cell(c, bx + x * CELL, by + y * CELL, CELL, piece_col(board[y][x]));

    if (state == CLEARING) {
        int t = (int)(now - clear_t) * 256 / CLEAR_MS;
        if (t > 256) t = 256;
        int a = (t < 90 ? t * 160 / 90 : 160 - (t - 90) * 160 / 166);
        for (int i = 0; i < nclear; i++) {
            int y = by + clear_rows[i] * CELL;
            gfx_fill(c, bx, y, bw, CELL, TH_A(UI_TEXT, a < 0 ? 0 : a));
            int shrink = t * bw / 512;
            gfx_fill(c, bx, y, shrink, CELL, ui_th.base);          /* wipe in from the sides */
            gfx_fill(c, bx + bw - shrink, y, shrink, CELL, ui_th.base);
        }
    } else {
        int cc[4][2];
        cells(cur, rot, cc);
        if (state == PLAY) {
            int gy = ghost_y();
            color_t pc = piece_col(cur);
            for (int i = 0; i < 4; i++) {
                int y = gy + cc[i][1];
                if (y < 0) continue;
                int x = bx + (px + cc[i][0]) * CELL, yy = by + y * CELL;
                gfx_round_fill(c, x + 1, yy + 1, CELL - 2, CELL - 2, R_1, TH_A(pc, 36));
                gfx_round_rect(c, x + 1, yy + 1, CELL - 2, CELL - 2, R_1, TH_A(pc, 140));
            }
            for (int i = 0; i < 4; i++) {
                int y = py + cc[i][1];
                if (y >= 0) draw_cell(c, bx + (px + cc[i][0]) * CELL, by + y * CELL, CELL, pc);
            }
        } else if (state == DEAD)
            for (int i = 0; i < 4; i++) {
                int y = py + cc[i][1];
                if (y >= 0) draw_cell(c, bx + (px + cc[i][0]) * CELL, by + y * CELL, CELL, UI_TEXT3);
            }
    }

    /* side column: hold, next, level and lines */
    int y = TOP;
    ui_card(ui, SIDE_X, y, SIDE_W, 92);
    gfx_text(c, T_CAPTION_B, SIDE_X + SP_4, y + SP_3, "Hold", UI_TEXT2);
    if (hold >= 0) mini(c, hold, SIDE_X + SIDE_W / 2, y + 54, 16);
    y += 92 + SP_4;
    ui_card(ui, SIDE_X, y, SIDE_W, 3 * 50 + 30);
    gfx_text(c, T_CAPTION_B, SIDE_X + SP_4, y + SP_3, "Next", UI_TEXT2);
    for (int i = 0; i < 3; i++) mini(c, nextq[i], SIDE_X + SIDE_W / 2, y + 54 + i * 50, i ? 14 : 16);
    y += 3 * 50 + 30 + SP_4;
    ui_card(ui, SIDE_X, y, SIDE_W, 2 * (H_ROW + SP_2));
    side_row(ui, y, "Level", level + 1, true);
    side_row(ui, y + H_ROW + SP_2, "Lines", lines, false);

    if (flash_msg[0] && now - flash_t < 900 && state != DEAD) {
        int a = 255 - (int)(now - flash_t) * 255 / 900;
        int dy = (int)(now - flash_t) / 40;
        gfx_text_center(c, T_HEADING, bx, by + bh / 2 - dy - 40, bw, flash_msg, TH_A(UI_TEXT, a));
    }

    if (state == PAUSED)
        game_card(ui, bx, by, bw, bh, "Paused", "Press any key to play. Left and Right move, Up or X rotates, Z rotates back, Down drops faster, Space drops, C holds, P pauses.", NULL, NULL);
    else if (state == DEAD) {
        char b[48];
        snprintf(b, sizeof b, score && score >= best ? "Score %d, a new best." : "Score %d. Best %d.", score, best);
        if (game_card(ui, bx, by, bw, bh, "Game over", b, NULL, "Play again")) reset();
    }
}

int main(void) {
    struct gui_window *w = gui_window_create("Tetris", WIN_W, WIN_H, WIN_FLAG_NORESIZE);
    if (!w) { perror("tetris"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    srand((unsigned)uptime_ms());
    best = load_best();
    reset();
    state = PAUSED;
    for (;;) {
        if (!ui_poll(&ui, state == PLAY || state == CLEARING ? 16 : 100)) break;
        uint32_t now = (uint32_t)uptime_ms();
        if (!ui.focused) {                         /* lost focus: drop held keys, pause */
            memset(dirdown, 0, sizeof dirdown);
            hold_left = hold_right = hold_soft = false; das_dir = 0;
            if (state == PLAY) state = PAUSED;
        }
        for (int i = 0; i < ui.nups; i++) {
            int k = ui.ups[i];
            if (k < 512) dirdown[k] = false;
            if (k == KEY_LEFT) { hold_left = false; if (das_dir < 0) das_dir = hold_right ? 1 : 0; if (das_dir) das_t = das_last = now; }
            if (k == KEY_RIGHT) { hold_right = false; if (das_dir > 0) das_dir = hold_left ? -1 : 0; if (das_dir) das_t = das_last = now; }
            if (k == KEY_DOWN) hold_soft = false;
        }
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            bool fresh = k.key < 512 && !dirdown[k.key];
            if (k.key < 512) dirdown[k.key] = true;
            if (k.key == 'p' || k.key == KEY_ESC) {
                if (!fresh) continue;
                if (state == PLAY) state = PAUSED;
                else if (state == PAUSED) { state = PLAY; last_drop = now; }
                continue;
            }
            if (state == PAUSED) { if (fresh) { state = PLAY; last_drop = now; } continue; }
            if (state == DEAD) { if (fresh && (k.key == KEY_ENTER || k.key == 'r')) reset(); continue; }
            if (state != PLAY || !fresh) continue;
            switch (k.key) {
            case KEY_LEFT:  hold_left = true; das_dir = -1; das_t = das_last = now; shift(-1, now); break;
            case KEY_RIGHT: hold_right = true; das_dir = 1; das_t = das_last = now; shift(1, now); break;
            case KEY_DOWN:  hold_soft = true; break;
            case KEY_UP: case 'x': rotate(1, now); break;
            case 'z': rotate(-1, now); break;
            case ' ': hard_drop(now); break;
            case 'c': do_hold(); break;
            }
        }
        if (state == CLEARING && now - clear_t >= CLEAR_MS) finish_clear();
        if (state == PLAY) {
            if (das_dir && now - das_t >= DAS_DELAY)
                while (now - das_last >= DAS_RATE) { das_last += DAS_RATE; shift(das_dir, now); }
            if (!das_dir) das_last = now;
            int iv = hold_soft && gravity() > SOFT_RATE ? SOFT_RATE : gravity();
            while (state == PLAY && now - last_drop >= (uint32_t)iv) {
                last_drop += iv;
                if (fits(cur, rot, px, py + 1)) {
                    py++;
                    if (hold_soft) score++;
                    if (py > lowest) { lowest = py; resets = 0; }
                    on_ground = !fits(cur, rot, px, py + 1);
                    if (on_ground) lock_t = now;
                }
                if (now - last_drop > 400) last_drop = now;
            }
            if (state == PLAY && !fits(cur, rot, px, py + 1)) {
                if (!on_ground) { on_ground = true; lock_t = now; }
                if (now - lock_t >= LOCK_DELAY) lock_piece(now);
            }
        }
        draw(&ui, now);
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
