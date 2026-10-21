/* snake - classic snake in the VibeOS style */
#include <ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define GW 22
#define GH 18
#define CELL 22
#define PAD 16
#define HEAD 64
#define WIN_W (PAD * 2 + GW * CELL)
#define WIN_H (HEAD + GH * CELL + PAD)
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

static void draw(struct ui *ui, uint32_t now) {
    struct canvas *c = ui->c;
    ui_clear(ui);
    gfx_vgradient(c, 0, 0, WIN_W, HEAD, RGB(0x2C, 0x1B, 0x52), UI_BG);
    char buf[48];
    gfx_text(c, &font_title, PAD, 14, "Snake", UI_TEXT);
    snprintf(buf, sizeof buf, "%d", score);
    gfx_text(c, &font_ui, WIN_W - PAD - 150, 10, "SCORE", UI_TEXT_DIM);
    gfx_text(c, &font_ui, WIN_W - PAD - 70, 10, "BEST", UI_TEXT_DIM);
    gfx_text(c, &font_ui_bold, WIN_W - PAD - 150, 30, buf, UI_ACCENT);
    snprintf(buf, sizeof buf, "%d", best);
    gfx_text(c, &font_ui_bold, WIN_W - PAD - 70, 30, buf, UI_ACCENT2);

    int bx = PAD, by = HEAD, bw = GW * CELL, bh = GH * CELL;
    gfx_shadow(c, bx, by, bw, bh, 10, 10, 120);
    gfx_round_fill(c, bx, by, bw, bh, 10, RGB(0x10, 0x0F, 0x1A));
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if ((x + y) & 1) gfx_fill(c, bx + x * CELL + 1, by + y * CELL + 1, CELL - 2, CELL - 2, RGBA(255, 255, 255, 6));
    gfx_round_rect(c, bx, by, bw, bh, 10, UI_BORDER);

    /* food: pulsing glow */
    int ph = (now / 8) % 200;
    int pulse = ph < 100 ? ph : 200 - ph;                    /* 0..100 */
    int fx = bx + food_x * CELL, fy = by + food_y * CELL;
    gfx_round_fill(c, fx - 4 - pulse / 25, fy - 4 - pulse / 25, CELL + 8 + pulse / 12, CELL + 8 + pulse / 12, 12, RGBA(0xFF, 0x7A, 0xC6, 28 + pulse / 4));
    gfx_round_fill(c, fx - 1, fy - 1, CELL + 2, CELL + 2, 9, RGBA(0xFF, 0x7A, 0xC6, 50));
    int fr = 4 + pulse / 50;
    gfx_circle_fill(c, fx + CELL / 2, fy + CELL / 2, CELL / 2 - fr / 2, UI_ACCENT2);
    gfx_circle_fill(c, fx + CELL / 2 - 2, fy + CELL / 2 - 3, 2, RGBA(255, 255, 255, 170));

    /* snake: tail darker, head bright; fade out when dead */
    for (int i = len - 1; i >= 0; i--) {
        color_t col = mix(UI_ACCENT, RGB(0x4A, 0x2E, 0x8C), i * 256 / (len > 1 ? len : 1) * 3 / 4);
        if (state == DEAD) col = mix(col, UI_BAD, 120);
        int x = bx + body[i].x * CELL, y = by + body[i].y * CELL;
        if (i == 0) gfx_round_fill(c, x - 3, y - 3, CELL + 6, CELL + 6, 10, RGBA(0xB3, 0x88, 0xFF, 36));
        gfx_round_fill(c, x + 1, y + 1, CELL - 2, CELL - 2, i == 0 ? 8 : 6, col);
        if (i) {                                              /* bridge to the previous segment */
            int dx = body[i - 1].x - body[i].x, dy = body[i - 1].y - body[i].y;
            if (dx == 1) gfx_fill(c, x + CELL - 3, y + 3, 6, CELL - 6, col);
            if (dx == -1) gfx_fill(c, x - 3, y + 3, 6, CELL - 6, col);
            if (dy == 1) gfx_fill(c, x + 3, y + CELL - 3, CELL - 6, 6, col);
            if (dy == -1) gfx_fill(c, x + 3, y - 3, CELL - 6, 6, col);
        }
    }
    int hx = bx + body[0].x * CELL + CELL / 2, hy = by + body[0].y * CELL + CELL / 2;
    int ex = DX[dir], ey = DY[dir];
    for (int s = -1; s <= 1; s += 2) {                        /* eyes */
        int px = hx + ex * 4 - ey * 5 * s, py = hy + ey * 4 + ex * 5 * s;
        gfx_circle_fill(c, px, py, 3, RGB(255, 255, 255));
        gfx_circle_fill(c, px + ex, py + ey, 1, RGB(0x1A, 0x10, 0x2E));
    }

    if (state == PAUSED || state == DEAD) {
        gfx_round_fill(c, bx, by, bw, bh, 10, RGBA(0x0C, 0x0A, 0x16, 190));
        int cx = bx + bw / 2, cy = by + bh / 2;
        if (state == PAUSED) {
            gfx_text_center(c, &font_title, bx, cy - 30, bw, "Paused", UI_TEXT);
            gfx_text_center(c, &font_ui, bx, cy + 6, bw, "P or Space to resume", UI_TEXT_DIM);
        } else {
            gfx_text_center(c, &font_title, bx, cy - 74, bw, "Game over", UI_BAD);
            snprintf(buf, sizeof buf, "Score %d", score);
            gfx_text_center(c, &font_big, bx, cy - 40, bw, buf, UI_TEXT);
            if (score && score >= best) gfx_text_center(c, &font_ui_bold, bx, cy - 4, bw, "New best!", UI_WARN);
            if (ui_button(ui, cx - 70, cy + 24, 140, 38, "Play again", UI_ACCENT_BTN)) reset();
        }
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
        int wait = state == PLAY ? (int)(last_step + interval() - now) : 33;
        if (wait > 33) wait = 33;
        if (wait < 0) wait = 0;
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
        draw(&ui, now);
        ui_end(&ui);
    }
    gui_window_destroy(w);
    return 0;
}
