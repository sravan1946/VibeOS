/* bounce.c - a GUI program built inside VibeOS:  tcc bounce.c -lgui -o bounce && ./bounce
 * Click to drop a new ball, drag the slider to change the speed, close the window to quit. */
#include <ui.h>
#include <stdio.h>
#include <stdlib.h>

#define W 480
#define H 320
#define MAXB 24

struct ball { float x, y, dx, dy; int r; color_t c; };
static struct ball b[MAXB];
static int nb;

static void add(int x, int y) {
    if (nb == MAXB) return;
    b[nb].x = x; b[nb].y = y;
    b[nb].dx = (rand() % 5 - 2) | 1; b[nb].dy = (rand() % 5 - 2) | 1;
    b[nb].r = 8 + rand() % 12;
    b[nb].c = RGB(100 + rand() % 156, 100 + rand() % 156, 100 + rand() % 156);
    nb++;
}

int main(void) {
    struct gui_window *w = gui_window_create("Bounce (built with tcc)", W, H, WIN_FLAG_NORESIZE);
    if (!w) { perror("bounce"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    int speed = 10;
    char txt[64];
    for (int i = 0; i < 5; i++) add(60 + rand() % (W - 120), 80 + rand() % (H - 120));
    while (ui_poll(&ui, 16)) {                       /* 16 ms timeout = ~60 fps animation */
        if (ui.pressed && ui.my > 40) add(ui.mx, ui.my);
        ui_clear(&ui);
        for (int i = 0; i < nb; i++) {
            struct ball *p = &b[i];
            p->x += p->dx * speed / 10.0f;
            p->y += p->dy * speed / 10.0f;
            if (p->x < p->r || p->x > W - p->r) p->dx = -p->dx;
            if (p->y < 40 + p->r || p->y > H - p->r) p->dy = -p->dy;
            gfx_circle_fill(ui.c, (int)p->x, (int)p->y, p->r, p->c);
        }
        ui_panel(&ui, 0, 0, W, 36);
        snprintf(txt, sizeof txt, "%d balls", nb);
        ui_label(&ui, 12, 10, txt, UI_TEXT);
        ui_slider(&ui, 120, 10, 200, &speed, 1, 40);
        if (ui_button(&ui, 350, 4, 110, 28, "Clear", UI_NORMAL_BTN)) nb = 0;
        ui_end(&ui);
    }
    return 0;
}
