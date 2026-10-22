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

/* colours are the desktop theme's (ui_th, kept current by libgui); a change arrives as EV_THEME and ui_poll returns,
 * so an app that redraws every frame follows it with no extra code. UI_OV / UI_ACC tint with an alpha. */
#define UI_BG        (ui_th.bg)
#define UI_PANEL     (ui_th.panel)
#define UI_SURFACE   (ui_th.surface)
#define UI_HOVER     (ui_th.hover)
#define UI_PRESSED   (ui_th.pressed)
#define UI_BORDER    (ui_th.border)
#define UI_TEXT      (ui_th.text)
#define UI_TEXT_DIM  (ui_th.dim)
#define UI_FAINT     (ui_th.faint)         /* decorative: line numbers, close glyphs */
#define UI_ACCENT    (ui_th.accent)
#define UI_ACCENT2   (ui_th.accent2)
#define UI_ON_ACCENT (ui_th.on_accent)     /* text on UI_ACCENT */
#define UI_INPUT     (ui_th.input)         /* text boxes, editors */
#define UI_GOOD      (ui_th.good)
#define UI_WARN      (ui_th.warn)
#define UI_BAD       (ui_th.bad)
#define UI_OV(a)     TH_A(ui_th.ov, a)     /* white on dark themes, black on light ones, at alpha a */
#define UI_ACC(a)    TH_A(ui_th.accent, a)
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
    bool dropped;                   /* EV_DROP this frame: a drag ended over this window at drop_x, drop_y */
    int drop_x, drop_y;
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
