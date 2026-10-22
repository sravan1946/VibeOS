#pragma once
void mouse_init(int screen_w, int screen_h);
void mouse_pos(int *x, int *y);
void mouse_set_screen(int screen_w, int screen_h);   /* display mode change */
extern void (*mouse_sink)(int x, int y, int buttons, int dz);   /* called from IRQ context */
