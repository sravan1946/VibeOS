/* doomgeneric_vibeos.c - doomgeneric platform layer: a libgui window, key events, uptime clock.
 * Doom renders 640x400 (its 320x200 at 2x); the frame is copied into the window, centred if the
 * window was resized. Sound is i_vibesound.c, which is fed from here (DG_SleepMs/DG_DrawFrame). */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

/* our KEY_* are enum constants; doomkeys.h #defines the same names, so take ours first */
enum { A_ESC = KEY_ESC, A_ENTER = KEY_ENTER, A_TAB = KEY_TAB, A_BS = KEY_BACKSPACE,
       A_UP = KEY_UP, A_DOWN = KEY_DOWN, A_LEFT = KEY_LEFT, A_RIGHT = KEY_RIGHT,
       A_HOME = KEY_HOME, A_END = KEY_END, A_PGUP = KEY_PGUP, A_PGDN = KEY_PGDN, A_INS = KEY_INSERT, A_DEL = KEY_DELETE,
       A_F1 = KEY_F1, A_LSHIFT = KEY_LSHIFT, A_RSHIFT = KEY_RSHIFT, A_LCTRL = KEY_LCTRL, A_RCTRL = KEY_RCTRL,
       A_LALT = KEY_LALT, A_RALT = KEY_RALT };
#include "doomkeys.h"
#include "doomgeneric.h"
#include "i_system.h"

void vibe_sound_pump(void);
void vibe_sound_close(void);

static struct gui_window *win;
static int redraw_all;
static uint16_t kq[128];                    /* pressed << 8 | doom key */
static unsigned kq_r, kq_w;
static uint8_t down[256];                   /* doom keys currently held (released again if focus is lost) */

static int doom_key(int k) {
    if (k >= 'a' && k <= 'z') return k;
    if (k >= ' ' && k < 127) switch (k) {
        case ' ': return KEY_USE;
        case ',': return KEY_STRAFE_L;
        case '.': return KEY_STRAFE_R;
        default: return k;                  /* digits (weapons), y/n, - = and the rest are ASCII in Doom too */
    }
    switch (k) {
    case A_ESC: return KEY_ESCAPE;
    case A_ENTER: return KEY_ENTER;
    case A_TAB: return KEY_TAB;
    case A_BS: return KEY_BACKSPACE;
    case A_UP: return KEY_UPARROW;
    case A_DOWN: return KEY_DOWNARROW;
    case A_LEFT: return KEY_LEFTARROW;
    case A_RIGHT: return KEY_RIGHTARROW;
    case A_LCTRL: case A_RCTRL: return KEY_FIRE;
    case A_LSHIFT: case A_RSHIFT: return KEY_RSHIFT;
    case A_LALT: case A_RALT: return KEY_RALT;
    case A_HOME: return KEY_HOME;
    case A_END: return KEY_END;
    case A_PGUP: return KEY_PGUP;
    case A_PGDN: return KEY_PGDN;
    case A_INS: return KEY_INS;
    case A_DEL: return KEY_DEL;
    }
    if (k >= A_F1 && k < A_F1 + 10) return KEY_F1 + (k - A_F1);
    if (k == A_F1 + 10) return KEY_F11;
    if (k == A_F1 + 11) return KEY_F12;
    return 0;
}

static void push(int pressed, int key) {
    if (kq_w - kq_r >= sizeof kq / sizeof *kq) return;
    if (pressed) down[key] = 1; else down[key] = 0;
    kq[kq_w++ % (sizeof kq / sizeof *kq)] = pressed << 8 | key;
}

static void handle(const struct gui_event *ev) {
    switch (ev->type) {
    case EV_KEY: { int k = doom_key(ev->key); if (k) push(ev->pressed, k); break; }
    case EV_FOCUS:
        if (!ev->pressed) for (int k = 1; k < 256; k++) if (down[k]) push(0, k);   /* no stuck keys */
        break;
    case EV_RESIZE: redraw_all = 1; break;
    case EV_CLOSE: I_Quit(); exit(0);   /* (quit_hook already left) */
    }
}

/* wait up to ms for window events (0 = just drain what is there) */
static void pump(int ms) {
    struct gui_event ev;
    while (gui_next_event(win, &ev, ms) == 1) { handle(&ev); ms = 0; }
}

static void quit_hook(void) { vibe_sound_close(); exit(0); }   /* I_Quit returns in doomgeneric: leave here */

void DG_Init(void) {
    win = gui_window_create("Doom", DOOMGENERIC_RESX, DOOMGENERIC_RESY, 0);
    if (!win) { fprintf(stderr, "doom: cannot open a window\n"); exit(1); }
    redraw_all = 1;
    I_AtExit(quit_hook, false);
}

void DG_DrawFrame(void) {
    pump(0);
    vibe_sound_pump();
    int cw = win->w < DOOMGENERIC_RESX ? win->w : DOOMGENERIC_RESX, ch = win->h < DOOMGENERIC_RESY ? win->h : DOOMGENERIC_RESY;
    int ox = (win->w - cw) / 2, oy = (win->h - ch) / 2;
    if (redraw_all) {
        for (int y = 0; y < win->h; y++) for (int x = 0; x < win->w; x++) win->pixels[y * win->stride + x] = 0xFF0E0F11;   /* letterbox: near-black, the frame is the game */
    }
    for (int y = 0; y < ch; y++) {
        const uint32_t *s = DG_ScreenBuffer + y * DOOMGENERIC_RESX;
        uint32_t *d = win->pixels + (oy + y) * win->stride + ox;
        for (int x = 0; x < cw; x++) d[x] = s[x] | 0xFF000000;
    }
    if (redraw_all) { redraw_all = 0; gui_flip(win); }
    else gui_flip_rect(win, ox, oy, cw, ch);
}

void DG_SleepMs(uint32_t ms) { vibe_sound_pump(); pump(ms); }
uint32_t DG_GetTicksMs(void) { return uptime_ms(); }

int DG_GetKey(int *pressed, unsigned char *key) {
    pump(0);
    if (kq_r == kq_w) return 0;
    uint16_t e = kq[kq_r++ % (sizeof kq / sizeof *kq)];
    *pressed = e >> 8;
    *key = e & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title) { gui_set_title(win, title); }
