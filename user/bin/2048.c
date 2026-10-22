/* 2048 - slide and merge tiles; animated slides, merge pops and spawns. Arrows / WASD, swipe with the mouse, U undoes. */
#include <ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define N 4
#define GAP 12
#define SLIDE_MS 110
#define POP_MS 150
#define BEST_PATH "/home/.2048_best"

static int grid[N][N];                    /* exponent: 0 empty, 1 = 2, 2 = 4 ... */
static bool merged[N][N];                 /* created by a merge in the last move */
static bool spawned[N][N];                /* tiles added by the last move or deal */
static int score, best, last_gain;
static int undo_grid[N][N], undo_score;
static bool can_undo, won, keep_going, over;

struct mover { int fr, fc, tr, tc, val; };
static struct mover movers[N * N];
static int nmovers;
static uint32_t anim_t0;                  /* start of the current move's animation, 0 = idle */
static int pending_dir = -1;              /* a key pressed while animating */

static const color_t TILE[12] = {
    RGB(0x24, 0x21, 0x36),
    RGB(0x3B, 0x35, 0x62), RGB(0x4E, 0x42, 0x88), RGB(0x7A, 0x55, 0xE0), RGB(0x9B, 0x6B, 0xFF), RGB(0xC2, 0x6B, 0xF5),
    RGB(0xFF, 0x6B, 0xC8), RGB(0xFF, 0x7A, 0x8E), RGB(0xFF, 0xA2, 0x4B), RGB(0xFF, 0xC2, 0x4B), RGB(0xB6, 0xE3, 0x5A),
    RGB(0x4B, 0xE0, 0x8A),
};

static void load_best(void) {
    FILE *f = fopen(BEST_PATH, "r");
    if (f) { if (fscanf(f, "%d", &best) != 1) best = 0; fclose(f); }
}
static void save_best(void) {
    FILE *f = fopen(BEST_PATH, "w");
    if (f) { fprintf(f, "%d\n", best); fclose(f); }
}

static bool add_tile(void) {
    int er[N * N], ec[N * N], n = 0;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (!grid[r][c]) { er[n] = r; ec[n] = c; n++; }
    if (!n) return false;
    int i = rand() % n;
    grid[er[i]][ec[i]] = rand() % 10 ? 1 : 2;
    spawned[er[i]][ec[i]] = true;
    return true;
}

static bool any_move(void) {
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        if (!grid[r][c]) return true;
        if (c + 1 < N && grid[r][c] == grid[r][c + 1]) return true;
        if (r + 1 < N && grid[r][c] == grid[r + 1][c]) return true;
    }
    return false;
}

static void new_game(void) {
    memset(grid, 0, sizeof grid); memset(merged, 0, sizeof merged);
    score = last_gain = 0; can_undo = won = keep_going = over = false;
    nmovers = 0; anim_t0 = 0; pending_dir = -1; memset(spawned, 0, sizeof spawned);
    add_tile(); add_tile();
    anim_t0 = uptime_ms();                /* the two starting tiles pop in */
}

/* dir: 0 left, 1 right, 2 up, 3 down. Returns false if nothing moved. */
static bool slide(int dir) {
    int old[N][N], ng[N][N] = { { 0 } };
    memcpy(old, grid, sizeof old);
    memset(merged, 0, sizeof merged);
    nmovers = 0;
    int gain = 0;
    for (int line = 0; line < N; line++) {
        int w = 0, prev_val = 0, prev_slot = -1;           /* write position, last tile placed, and whether it may still merge */
        for (int k = 0; k < N; k++) {
            int i = dir == 0 || dir == 2 ? k : N - 1 - k;   /* read from the side we slide towards */
            int fr = dir < 2 ? line : i, fc = dir < 2 ? i : line;
            int v = old[fr][fc];
            if (!v) continue;
            int tr, tc;
            if (prev_slot >= 0 && prev_val == v) {          /* merge into the previous tile */
                int j = prev_slot;
                tr = dir < 2 ? line : (dir == 2 ? j : N - 1 - j);
                tc = dir < 2 ? (dir == 0 ? j : N - 1 - j) : line;
                ng[tr][tc] = v + 1; merged[tr][tc] = true;
                gain += 1 << (v + 1);
                prev_slot = -1; prev_val = 0;
            } else {
                int j = w++;
                tr = dir < 2 ? line : (dir == 2 ? j : N - 1 - j);
                tc = dir < 2 ? (dir == 0 ? j : N - 1 - j) : line;
                ng[tr][tc] = v;
                prev_slot = j; prev_val = v;
            }
            movers[nmovers++] = (struct mover){ fr, fc, tr, tc, v };
        }
    }
    if (!memcmp(old, ng, sizeof old)) { nmovers = 0; memset(merged, 0, sizeof merged); return false; }
    memcpy(undo_grid, old, sizeof old); undo_score = score; can_undo = true;
    memcpy(grid, ng, sizeof grid);
    score += gain; last_gain = gain;
    if (score > best) { best = score; save_best(); }
    memset(spawned, 0, sizeof spawned);
    add_tile();
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (grid[r][c] >= 11 && merged[r][c] && !keep_going) won = true;
    if (!any_move()) over = true;
    anim_t0 = uptime_ms();
    return true;
}

static void undo(void) {
    if (!can_undo) return;
    memcpy(grid, undo_grid, sizeof grid);
    score = undo_score; can_undo = over = false; last_gain = 0;
    nmovers = 0; anim_t0 = 0; memset(spawned, 0, sizeof spawned); memset(merged, 0, sizeof merged);
}

static void draw_tile(struct ui *ui, int x, int y, int s, int val) {
    if (s < 4) return;
    color_t col = TILE[val < 11 ? val : 11];
    gfx_round_fill(ui->c, x, y, s, s, 10, col);
    if (val >= 2) gfx_vgradient(ui->c, x + 8, y + 2, s - 16, 2, RGBA(255, 255, 255, 70), RGBA(255, 255, 255, 0));
    char t[12];
    snprintf(t, sizeof t, "%d", 1 << val);
    const struct font *f = &font_big;
    if (gfx_text_width(f, t) > s - 14) f = &font_title;
    if (gfx_text_width(f, t) > s - 14) f = &font_ui_bold;
    color_t fc = val >= 8 ? RGB(0x1A, 0x10, 0x2E) : UI_TEXT;
    struct canvas cc = *ui->c;
    canvas_clip(&cc, x, y, s, s);
    gfx_text_center(&cc, f, x, y + (s - f->height) / 2, s, t, fc);
}

static float ease(float t) { return 1 - (1 - t) * (1 - t); }

int main(void) {
    struct gui_window *w = gui_window_create("2048", 460, 590, 0);
    if (!w) return 1;
    srand(uptime_ms());
    load_best();
    struct ui ui;
    ui_init(&ui, w);
    new_game();
    bool swiping = false;
    int sx = 0, sy = 0;
    for (;;) {
        uint32_t now = uptime_ms();
        bool anim = anim_t0 && now - anim_t0 < SLIDE_MS + POP_MS;
        if (!ui_poll(&ui, anim ? 16 : -1)) break;
        now = uptime_ms();
        anim = anim_t0 && now - anim_t0 < SLIDE_MS + POP_MS;
        if (!anim) { anim_t0 = 0; nmovers = 0; }
        bool modal = won || over;
        int dir = -1;
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            int ch = k.key < 128 ? (k.key | 0x20) : 0;
            if (k.key == KEY_LEFT || ch == 'a') dir = 0;
            else if (k.key == KEY_RIGHT || ch == 'd') dir = 1;
            else if (k.key == KEY_UP || ch == 'w') dir = 2;
            else if (k.key == KEY_DOWN || ch == 's') dir = 3;
            else if (ch == 'u' || ((k.mods & MOD_CTRL) && ch == 'z')) undo();
            else if (ch == 'n') new_game();
        }
        int W = w->w, H = w->h;
        int bs = W - 48;
        if (H - 176 - 24 < bs) bs = H - 176 - 24;
        int bx = (W - bs) / 2, by = 152;
        if (ui.pressed && !modal && ui_hover(&ui, bx, by, bs, bs)) { swiping = true; sx = ui.mx; sy = ui.my; }
        if (ui.released && swiping) {
            int dx = ui.mx - sx, dy = ui.my - sy;
            if (abs(dx) > 24 || abs(dy) > 24) dir = abs(dx) > abs(dy) ? (dx < 0 ? 0 : 1) : (dy < 0 ? 2 : 3);
            swiping = false;
        }
        if (modal) dir = -1;
        if (dir >= 0) {
            if (anim) pending_dir = dir;
            else slide(dir);
        } else if (!anim && pending_dir >= 0 && !modal) { slide(pending_dir); pending_dir = -1; }
        if (!anim && pending_dir >= 0 && dir < 0) pending_dir = -1;
        anim = anim_t0 && uptime_ms() - anim_t0 < SLIDE_MS + POP_MS;
        uint32_t el = anim ? uptime_ms() - anim_t0 : 1 << 30;

        /* ---- draw ---- */
        ui_clear(&ui);
        struct canvas *c = ui.c;
        gfx_text(c, &font_big, 24, 18, "2048", UI_ACCENT);
        gfx_text(c, &font_ui, 26, 74, "Join the tiles, reach 2048", UI_TEXT_DIM);
        int sw = 88;
        for (int i = 0; i < 2; i++) {
            int x = W - 24 - (2 - i) * sw - (1 - i) * 8 + (i ? 0 : 0);
            gfx_round_fill(c, x, 18, sw, 52, 10, UI_PANEL);
            gfx_round_rect(c, x, 18, sw, 52, 10, UI_BORDER);
            gfx_text_center(c, &font_ui, x, 24, sw, i ? "BEST" : "SCORE", UI_TEXT_DIM);
            char t[16];
            snprintf(t, sizeof t, "%d", i ? best : score);
            gfx_text_center(c, &font_ui_bold, x, 44, sw, t, UI_TEXT);
        }
        if (last_gain && anim && el < 600) {                    /* +N floating above the score */
            char t[16];
            snprintf(t, sizeof t, "+%d", last_gain);
            int a = 255 - (int)el * 255 / 600;
            gfx_text_center(c, &font_ui_bold, W - 24 - 2 * sw - 8, 4 - (int)el / 40, sw, t, RGBA(0x4B, 0xE0, 0x8A, a));
        }
        int bw = (bs - 12) / 2 < 120 ? (bs - 12) / 2 : 120;
        if (ui_button(&ui, 24, 100, bw, 36, "New Game", UI_ACCENT_BTN)) new_game();
        if (ui_button(&ui, 24 + bw + 10, 100, 90, 36, "Undo", can_undo ? UI_NORMAL_BTN : UI_GHOST_BTN)) undo();
        gfx_text(c, &font_ui, 24 + bw + 110, 108, "arrows / WASD / swipe", UI_TEXT_DIM);

        gfx_shadow(c, bx, by, bs, bs, 14, 14, 110);
        gfx_round_fill(c, bx, by, bs, bs, 14, UI_PANEL);
        gfx_round_rect(c, bx, by, bs, bs, 14, UI_BORDER);
        int ts = (bs - GAP * (N + 1)) / N;
        for (int r = 0; r < N; r++)
            for (int cc = 0; cc < N; cc++) gfx_round_fill(c, bx + GAP + cc * (ts + GAP), by + GAP + r * (ts + GAP), ts, ts, 10, TILE[0]);
        if (el < SLIDE_MS && nmovers) {                         /* phase 1: tiles slide */
            float t = ease((float)el / SLIDE_MS);
            for (int i = 0; i < nmovers; i++) {
                struct mover *m = &movers[i];
                int x0 = bx + GAP + m->fc * (ts + GAP), y0 = by + GAP + m->fr * (ts + GAP);
                int x1 = bx + GAP + m->tc * (ts + GAP), y1 = by + GAP + m->tr * (ts + GAP);
                draw_tile(&ui, x0 + (int)((x1 - x0) * t), y0 + (int)((y1 - y0) * t), ts, m->val);
            }
        } else {                                                // phase 2 / idle: settled grid with pops
            float pt = nmovers ? (float)((int)el - SLIDE_MS) / POP_MS : (float)el / POP_MS;
            if (pt > 1) pt = 1;
            for (int r = 0; r < N; r++)
                for (int cc = 0; cc < N; cc++) {
                    int v = grid[r][cc];
                    if (!v) continue;
                    int x = bx + GAP + cc * (ts + GAP), y = by + GAP + r * (ts + GAP);
                    float sc = 1.0f;
                    if (anim && spawned[r][cc]) sc = ease(pt);
                    else if (anim && merged[r][cc]) sc = 1.0f + 0.18f * (pt < 0.5f ? pt * 2 : (1 - pt) * 2);
                    int s = (int)(ts * sc);
                    draw_tile(&ui, x + (ts - s) / 2, y + (ts - s) / 2, s, v);
                }
        }
        if (modal) {
            gfx_round_fill(c, bx, by, bs, bs, 14, RGBA(0x16, 0x15, 0x22, 215));
            const char *t = won ? "You made 2048!" : "No moves left";
            gfx_text_center(c, &font_title, bx, by + bs / 2 - 70, bs, t, won ? UI_GOOD : UI_BAD);
            char s[32];
            snprintf(s, sizeof s, "Score %d", score);
            gfx_text_center(c, &font_ui, bx, by + bs / 2 - 30, bs, s, UI_TEXT_DIM);
            int bw2 = 130;
            if (won) {
                if (ui_button(&ui, bx + bs / 2 - bw2 - 6, by + bs / 2 + 4, bw2, 38, "Keep going", UI_ACCENT_BTN)) { won = false; keep_going = true; }
                if (ui_button(&ui, bx + bs / 2 + 6, by + bs / 2 + 4, bw2, 38, "New Game", UI_NORMAL_BTN)) new_game();
            } else {
                if (ui_button(&ui, bx + bs / 2 - bw2 - 6, by + bs / 2 + 4, bw2, 38, "Try again", UI_ACCENT_BTN)) new_game();
                if (ui_button(&ui, bx + bs / 2 + 6, by + bs / 2 + 4, bw2, 38, "Undo", can_undo ? UI_NORMAL_BTN : UI_GHOST_BTN)) undo();
            }
        }
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
