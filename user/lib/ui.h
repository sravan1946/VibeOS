/* ui.h - tiny immediate-mode widget toolkit with the VibeOS look.
 *
 *   struct ui ui; ui_init(&ui, gui_window_create("App", 400, 300, 0));
 *   while (ui_poll(&ui, -1)) {          // waits for input (timeout_ms for animation)
 *       ui_clear(&ui);
 *       if (ui_button(&ui, 10, 10, 100, 32, "Hello", UI_ACCENT_BTN)) ...;
 *       ui_end(&ui);                    // flips the window
 *   }
 * Widgets are identified by their rectangle, so keep them at stable positions. */
#pragma once
#include "gui.h"
#include <stdbool.h>

#define UI_BG        RGB(0x16, 0x15, 0x22)
#define UI_PANEL     RGB(0x1E, 0x1C, 0x2D)
#define UI_SURFACE   RGB(0x2A, 0x27, 0x3D)
#define UI_HOVER     RGB(0x35, 0x31, 0x4C)
#define UI_PRESSED   RGB(0x22, 0x20, 0x33)
#define UI_BORDER    RGBA(255, 255, 255, 26)
#define UI_TEXT      RGB(0xED, 0xEB, 0xFF)
#define UI_TEXT_DIM  RGB(0x9A, 0x96, 0xB8)
#define UI_ACCENT    RGB(0xB3, 0x88, 0xFF)
#define UI_ACCENT2   RGB(0xFF, 0x7A, 0xC6)
#define UI_GOOD      RGB(0x4B, 0xE0, 0x8A)
#define UI_WARN      RGB(0xFF, 0xC2, 0x4B)
#define UI_BAD       RGB(0xFF, 0x5F, 0x7A)
#define UI_RADIUS    8

enum { UI_NORMAL_BTN = 0, UI_ACCENT_BTN, UI_GHOST_BTN };

struct ui {
    struct gui_window *win;
    struct canvas *c;
    int mx, my, buttons;            /* pointer in client coords */
    bool pressed, released, rpressed; /* left down / up, right down this frame */
    bool dbl;                       /* this press is a double click */
    int scroll;                     /* wheel this frame (+ = down) */
    struct gui_event keys[32];      /* key presses this frame, consumed by widgets */
    int nkeys;
    int ups[16], nups;              /* key codes released this frame (for games / DAS) */
    uint32_t hot, active, focus;    /* widget ids */
    bool closed, resized, focused, timeout;
    uint32_t last_click_ms;
    int last_click_x, last_click_y;
    uint32_t *back;                 /* private frame; ui_end copies + flips only what changed */
    int back_w, back_h;
    struct canvas bcv;
};

void ui_init(struct ui *ui, struct gui_window *win);
bool ui_poll(struct ui *ui, int timeout_ms);   /* false once the window was closed */
void ui_end(struct ui *ui);                    /* flip the region that changed since last frame */
void ui_clear(struct ui *ui);                  /* fill with UI_BG */

bool ui_hover(struct ui *ui, int x, int y, int w, int h);
bool ui_clicked(struct ui *ui, int x, int y, int w, int h);   /* press inside this frame */
bool ui_button(struct ui *ui, int x, int y, int w, int h, const char *label, int style);
bool ui_checkbox(struct ui *ui, int x, int y, const char *label, bool *v);
bool ui_slider(struct ui *ui, int x, int y, int w, int *v, int min, int max);
/* single-line editor; returns true when Enter is pressed. Click to focus. */
bool ui_textbox(struct ui *ui, int x, int y, int w, int h, char *buf, int cap, const char *placeholder);
/* scrolling list; *sel / *scroll are owned by the caller. Returns the index activated
 * (double click or Enter) or -1. */
int  ui_list(struct ui *ui, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll);
void ui_panel(struct ui *ui, int x, int y, int w, int h);
void ui_label(struct ui *ui, int x, int y, const char *s, color_t col);
/* pop the next unconsumed key press; false if none */
bool ui_key(struct ui *ui, struct gui_event *out);
/* text clipped to maxw with a trailing "..." */
void ui_text_fit(struct ui *ui, const struct font *f, int x, int y, int maxw, const char *s, color_t col);
/* segmented tab strip; returns true when *sel changed */
bool ui_tabs(struct ui *ui, int x, int y, int w, int h, const char *const *names, int n, int *sel);
/* flat toolbar button; `on` draws it latched. Returns clicked. */
bool ui_tool(struct ui *ui, int x, int y, int w, int h, const char *label, bool on);
/* vertical scrollbar for content of `total` rows with `visible` shown; *first is dragged. */
void ui_vscroll(struct ui *ui, int x, int y, int h, int total, int visible, int *first);
/* the id a widget at this rectangle gets, e.g. to set ui->focus on a textbox programmatically */
uint32_t ui_id(int x, int y, int w, int h);
/* modal dialogs: draw them last, every frame, while open. Return 1 = confirmed, -1 = cancelled, 0 = still open.
 * ui_prompt edits buf (focus is taken automatically); ui_confirm just asks. */
int ui_prompt(struct ui *ui, const char *title, const char *okl, char *buf, int cap);
int ui_confirm(struct ui *ui, const char *title, const char *text, const char *okl);
