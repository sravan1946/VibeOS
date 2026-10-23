/* snake - classic snake */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define GW 22
#define GH 18
#define CELL 22
#define PAD SP_5
#define WIN_W (PAD * 2 + GW * CELL)
#define WIN_H (HEAD + PAD + GH * CELL + PAD)
#define BEST_PATH "/home/.snake_best"

enum { PLAY, PAUSED, DEAD };
static const int DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };   /* up right down left */

static struct { int x, y; } body[GW * GH];     /* body[0] = head */
static int len, dir, queue[3], nq, food_x, food_y, score, best, state;
static uint32_t last_step, death_ms;

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

static bool on_snake(int x, int y, int skip_tail) {
    for (int i = 0; i < len - skip_tail; i++) if (body[i].x == x && body[i].y == y) return true;
    return false;
}
static void place_food(void) {
    if (len >= GW * GH) return;
    do { food_x = rand() % GW; food_y = rand() % GH; } while (on_snake(food_x, food_y, 0));
}
static void reset(void) {
    len = 4; dir = 1; nq = 0; score = 0; state = PLAY;
    for (int i = 0; i < len; i++) { body[i].x = GW / 2 - i; body[i].y = GH / 2; }
    place_food();
    last_step = (uint32_t)uptime_ms();
}
static int interval(void) { int v = 140 - (score / 3) * 8; return v < 60 ? 60 : v; }

static void step(void) {
    if (nq) { dir = queue[0]; queue[0] = queue[1]; queue[1] = queue[2]; nq--; }
    int nx = body[0].x + DX[dir], ny = body[0].y + DY[dir];
    bool eat = nx == food_x && ny == food_y;
    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH || on_snake(nx, ny, eat ? 0 : 1)) {
        state = DEAD;
        death_ms = (uint32_t)uptime_ms();
        if (score > best) { best = score; save_best(); }
        beep(150, 120);
        return;
    }
    if (eat) len++;
    memmove(body + 1, body, (len - 1) * sizeof body[0]);
    body[0].x = nx; body[0].y = ny;
    if (eat) { score++; place_food(); beep(880, 25); }
}

static void turn(int d) {
    int last = nq ? queue[nq - 1] : dir;
    if (d == last || d == (last + 2) % 4 || nq >= 3) return;
    queue[nq++] = d;
}

static void draw(struct ui *ui) {
    struct canvas *c = ui->c;
    ui_clear(ui);
    game_head(ui, WIN_W);
    int x = game_stati(ui, PAD, "Score", score);
    game_stati(ui, x, "Best", best);
    int rx = WIN_W - PAD;
    if (game_btn(ui, &rx, UI_NOICON, "New game", UI_SECONDARY)) reset();

    int bx = PAD, by = HEAD + PAD, bw = GW * CELL, bh = GH * CELL;
    game_well(ui, bx, by, bw, bh);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if ((x + y) & 1) gfx_fill(c, bx + x * CELL, by + y * CELL, CELL, CELL, TH_A(UI_TEXT, 5));

    gfx_circle_fill(c, bx + food_x * CELL + CELL / 2, by + food_y * CELL + CELL / 2, CELL / 2 - 4, game_fill(1));

    /* snake: one muted green, the tail fading toward the well; grey once dead */
    color_t head = state == DEAD ? UI_TEXT3 : game_fill(2);
    for (int i = len - 1; i >= 0; i--) {
        color_t col = mix(head, ui_th.base, i * 110 / (len > 1 ? len : 1));
        int x = bx + body[i].x * CELL, y = by + body[i].y * CELL;
        gfx_round_fill(c, x + 2, y + 2, CELL - 4, CELL - 4, i ? R_2 : R_3, col);
        if (i) {                                              /* bridge to the previous segment */
            int dx = body[i - 1].x - body[i].x, dy = body[i - 1].y - body[i].y;
            if (dx == 1) gfx_fill(c, x + CELL - 4, y + 4, 8, CELL - 8, col);
            if (dx == -1) gfx_fill(c, x - 4, y + 4, 8, CELL - 8, col);
            if (dy == 1) gfx_fill(c, x + 4, y + CELL - 4, CELL - 8, 8, col);
            if (dy == -1) gfx_fill(c, x + 4, y - 4, CELL - 8, 8, col);
        }
    }
    int hx = bx + body[0].x * CELL + CELL / 2, hy = by + body[0].y * CELL + CELL / 2, ex = DX[dir], ey = DY[dir];
    for (int s = -1; s <= 1; s += 2) gfx_circle_fill(c, hx + ex * 4 - ey * 4 * s, hy + ey * 4 + ex * 4 * s, 2, ui_th.base);

    if (state == PAUSED) game_card(ui, bx, by, bw, bh, "Paused", "Press an arrow key or Space to play. P pauses.", NULL, NULL);
    else if (state == DEAD) {
        char b[48];
        snprintf(b, sizeof b, score && score >= best ? "Score %d, a new best." : "Score %d. Best %d.", score, best);
        if (game_card(ui, bx, by, bw, bh, "Game over", b, NULL, "Play again")) reset();
    }
}

int main(void) {
    struct gui_window *w = gui_window_create("Snake", WIN_W, WIN_H, WIN_FLAG_NORESIZE);
    if (!w) { perror("snake"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    srand((unsigned)uptime_ms());
    best = load_best();
    reset();
    state = PAUSED;                      /* start paused so the first key press begins */
    for (;;) {
        uint32_t now = (uint32_t)uptime_ms();
        int wait = state == PLAY ? (int)(last_step + interval() - now) : -1;
        if (wait > 33) wait = 33;
        if (wait < 0 && state == PLAY) wait = 0;
        if (!ui_poll(&ui, wait)) break;
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            int d = -1;
            switch (k.key) {
            case KEY_UP: case 'w': d = 0; break;
            case KEY_RIGHT: case 'd': d = 1; break;
            case KEY_DOWN: case 's': d = 2; break;
            case KEY_LEFT: case 'a': d = 3; break;
            case 'p': case ' ':
                if (state == PLAY) state = PAUSED;
                else if (state == PAUSED) { state = PLAY; last_step = (uint32_t)uptime_ms(); }
                else reset();
                break;
            case KEY_ENTER: if (state == DEAD) reset(); break;
            }
            if (d >= 0) {
                if (state == PAUSED) { state = PLAY; last_step = (uint32_t)uptime_ms(); }
                if (state == PLAY) turn(d);
            }
        }
        if (!ui.focused && state == PLAY) state = PAUSED;
        now = (uint32_t)uptime_ms();
        while (state == PLAY && now - last_step >= (uint32_t)interval()) {
            last_step += interval();
            step();
            if (now - last_step > 500) last_step = now;      /* don't spiral after a stall */
        }
        draw(&ui);
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
