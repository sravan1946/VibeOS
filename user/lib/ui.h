/* ui.h - immediate-mode widget toolkit, Graphite & Signal (docs/DESIGN.md §9).
 *
 *   struct ui ui; ui_init(&ui, gui_window_create("App", 400, 300, 0));
 *   while (ui_poll(&ui, -1)) {          // waits for input (timeout_ms for animation)
 *       ui_clear(&ui);
 *       if (ui_button(&ui, 16, 16, 96, H_CTL, "Save", UI_PRIMARY)) ...;
 *       ui_end(&ui);                    // overlays (menus, tooltips, toasts), then flips what changed
 *   }
 * Widgets are identified by their rectangle, so keep them at stable positions. Every widget reads the theme each frame,
 * so EV_THEME (ui->themed) repaints correctly with no extra code. Sizes, radii and type roles come from design.h (T_*,
 * H_CTL...; include it, and icons.h for ICON_*: ui.h doesn't, so older apps' own names don't clash).
 * Keyboard: Tab / Shift+Tab move focus between widgets drawn this frame; Enter/Space activate, arrows adjust. The 2px
 * focus ring shows only after keyboard use (ui->kbd) and clears on a pointer press. */
#pragma once
#include "gui.h"
#include <stdbool.h>
struct aicon;                       /* icons.h: include it for ICON_* ids and appicon_get / fileicon_get */

/* theme colours (ui_th, kept current by libgui). New code: the Graphite token names */
#define UI_BG        (ui_th.window)
#define UI_PANEL     (ui_th.base)
#define UI_SURFACE   (ui_th.raised)
#define UI_HOVER     (ui_th.hover)          /* legacy opaque hover */
#define UI_PRESSED   (ui_th.pressed)
#define UI_BORDER    (ui_th.line)
#define UI_LINE      (ui_th.line)
#define UI_TEXT      (ui_th.text)
#define UI_TEXT_DIM  (ui_th.text2)
#define UI_TEXT2     (ui_th.text2)
#define UI_FAINT     (ui_th.text3)          /* placeholders, captions, shortcut hints */
#define UI_TEXT3     (ui_th.text3)
#define UI_ACCENT    (ui_th.accent)         /* fills only; accent-coloured text is UI_ACCENT_TEXT */
#define UI_ACCENT_TEXT (ui_th.accent_text)
#define UI_ACCENT2   (ui_th.accent)
#define UI_ON_ACCENT (ui_th.on_accent)      /* text on UI_ACCENT */
#define UI_SELECTION (ui_th.selection)
#define UI_INPUT     (ui_th.raised)
#define UI_GOOD      (ui_th.success)
#define UI_WARN      (ui_th.warning)
#define UI_BAD       (ui_th.danger)
#define UI_OV(a)     TH_A(ui_th.text, a)    /* text colour at alpha a: hover/press backplates (hover_ov = 15, press_ov = 31) */
#define UI_ACC(a)    TH_A(ui_th.accent, a)
#define UI_RADIUS    4                      /* R_2 */

/* button styles (low byte), OR'd with flags */
enum { UI_NORMAL_BTN = 0, UI_ACCENT_BTN, UI_GHOST_BTN, UI_DANGER_BTN };
#define UI_SECONDARY UI_NORMAL_BTN
#define UI_PRIMARY   UI_ACCENT_BTN
#define UI_GHOST     UI_GHOST_BTN
#define UI_DANGER    UI_DANGER_BTN
enum {
    UI_DISABLED = 0x100,    /* 38% opacity, no hover, not focusable */
    UI_LOADING  = 0x200,    /* button: a spinner replaces the label */
    UI_ON       = 0x400,    /* latched toolbar button */
    UI_SELECTED = 0x800,    /* rows */
    UI_ERROR    = 0x1000,   /* inputs: danger ring */
    UI_INACTIVE = 0x2000,   /* tab strip of an unfocused window */
    UI_MIXED    = 0x4000,   /* checkbox: indeterminate */
};
#define UI_NOICON (-1)

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
    bool themed;                    /* EV_THEME this frame: ui_th already holds the new colours */
    bool dropped;                   /* EV_DROP this frame: a drag ended over this window at drop_x, drop_y */
    int drop_x, drop_y;
    uint32_t last_click_ms;
    int last_click_x, last_click_y;
    uint32_t *back;                 /* private frame; ui_end copies + flips only what changed */
    int back_w, back_h;
    struct canvas bcv;
    /* ---- toolkit state (private) ---- */
    bool kbd;                       /* focus ring visible: keyboard used since the last pointer press */
    uint32_t now, wake;             /* uptime this frame; when ui_poll must return next (0 = no deadline) */
    uint32_t fl[64]; int nf;        /* focusable ids drawn this frame, in order */
    bool in_ov, modal, modal_prev;  /* drawing an overlay; a dialog is up (this / last frame) */
    uint32_t pop_id; const char *const *pop_items; int pop_n, pop_sel, pop_hi, pop_pick; struct rect pop_r, pop_anchor; bool pop_seen;
    uint32_t tip_id, tip_t0, tip_seen; const char *tip; struct rect tip_r; bool tip_show;
    char toast_msg[96], toast_act[24]; uint32_t toast_until; struct rect toast_r, toast_btn; bool toast_hit;
    uint32_t caret_id, caret_t0; int caret, scroll_x;
    int drag_off;
    int force;                      /* widget gallery only: draw as UI_F_HOVER | UI_F_PRESS | UI_F_FOCUS */
};
enum { UI_F_HOVER = 1, UI_F_PRESS = 2, UI_F_FOCUS = 4 };

void ui_init(struct ui *ui, struct gui_window *win);
bool ui_poll(struct ui *ui, int timeout_ms);   /* false once the window was closed */
void ui_end(struct ui *ui);                    /* draw overlays, flip the region that changed since last frame */
void ui_clear(struct ui *ui);                  /* fill with the window colour */

bool ui_hover(struct ui *ui, int x, int y, int w, int h);
bool ui_clicked(struct ui *ui, int x, int y, int w, int h);   /* press inside this frame */
/* pop the next unconsumed key press; false if none */
bool ui_key(struct ui *ui, struct gui_event *out);
/* the id a widget at this rectangle gets, e.g. to set ui->focus on a textbox programmatically */
uint32_t ui_id(int x, int y, int w, int h);

/* ---- text and icons ---- */
void ui_label(struct ui *ui, int x, int y, const char *s, color_t col);        /* T_BODY */
/* text clipped to maxw with a trailing ellipsis */
void ui_text_fit(struct ui *ui, const struct font *f, int x, int y, int maxw, const char *s, color_t col);
/* word-wrapped to w; returns the height used. col alpha 0 = measure only */
int  ui_text_wrap(struct ui *ui, const struct font *f, int x, int y, int w, const char *s, color_t col);
/* a Lucide icon (ICON_*) tinted col; sizes other than 16/20 are scaled from the 20px mask */
void ui_icon(struct ui *ui, int x, int y, int icon, int size, color_t col);
/* a premultiplied colour icon from appicon_get / fileicon_get */
void ui_aicon(struct ui *ui, int x, int y, const struct aicon *a);

/* ---- buttons (H_CTL tall; 0 12 padding; T_BODY_B) ---- */
bool ui_button(struct ui *ui, int x, int y, int w, int h, const char *label, int style);
/* icon (ICON_* or UI_NOICON) + label (NULL: icon only); style | UI_DISABLED / UI_LOADING / UI_ON */
bool ui_button_ex(struct ui *ui, int x, int y, int w, int h, int icon, const char *label, int style);
int  ui_button_w(int icon, const char *label);  /* natural width: padding + icon + gap + label */
/* 28x28 ghost icon button (toolbars, title areas); flags: UI_ON, UI_DISABLED. tip: tooltip text or NULL */
bool ui_icon_button(struct ui *ui, int x, int y, int icon, int flags, const char *tip);
/* flat toolbar button with a text label; `on` draws it latched. Returns clicked. */
bool ui_tool(struct ui *ui, int x, int y, int w, int h, const char *label, bool on);

/* ---- toggles and values; (x, y) is the top-left of the control's 28px row unless noted ---- */
bool ui_checkbox(struct ui *ui, int x, int y, const char *label, bool *v);
bool ui_checkbox_ex(struct ui *ui, int x, int y, const char *label, bool *v, int flags);   /* UI_MIXED, UI_DISABLED */
bool ui_radio(struct ui *ui, int x, int y, const char *label, int *sel, int val, int flags);
bool ui_switch(struct ui *ui, int x, int y, bool *v, int flags);          /* the 36x20 track at x, y */
/* 24px hit row at y with a 4px track; arrows step 1, PgUp/PgDn 10 */
bool ui_slider(struct ui *ui, int x, int y, int w, int *v, int min, int max);
bool ui_slider_ex(struct ui *ui, int x, int y, int w, int *v, int min, int max, int flags);
/* segmented control stretched over w x h (2-4 options); returns true when *sel changed */
bool ui_tabs(struct ui *ui, int x, int y, int w, int h, const char *const *names, int n, int *sel);
/* segmented control at natural width (28 tall): icons[i] (or NULL) and/or names[i] (or NULL) */
bool ui_segmented(struct ui *ui, int x, int y, const int *icons, const char *const *names, int n, int *sel);
/* in-window tab strip (H_TAB) with the amber signal line under the active tab; flags: UI_INACTIVE */
bool ui_tabbar(struct ui *ui, int x, int y, int w, const char *const *names, int n, int *sel, int flags);
/* dropdown/select: a secondary button showing items[*sel]; opens a menu list. Returns true when *sel changed */
bool ui_dropdown(struct ui *ui, int x, int y, int w, const char *const *items, int n, int *sel, int flags);

/* ---- text entry (click to focus; returns true when Enter is pressed) ---- */
bool ui_textbox(struct ui *ui, int x, int y, int w, int h, char *buf, int cap, const char *placeholder);
/* icon: leading ICON_* or UI_NOICON; flags: UI_ERROR, UI_DISABLED */
bool ui_input(struct ui *ui, int x, int y, int w, int h, char *buf, int cap, const char *placeholder, int icon, int flags);
/* H_SEARCH search field with a search icon and a clear button (Esc clears too) */
bool ui_search(struct ui *ui, int x, int y, int w, char *buf, int cap, const char *placeholder);

/* ---- lists and navigation ---- */
/* scrolling list; *sel / *scroll are owned by the caller. Returns the index activated (double click or Enter) or -1 */
int  ui_list(struct ui *ui, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll);
/* one row (H_ROW, or H_ROW2 with a subline): icon (ICON_*) or a colour icon `ai` (32px on two-line rows), title,
 * sub (NULL = one line), trail (right, T_CAPTION text3, or NULL). flags: UI_SELECTED. Returns true when pressed */
bool ui_row(struct ui *ui, int x, int y, int w, int icon, const struct aicon *ai, const char *title, const char *sub,
            const char *trail, int flags);
void ui_sidebar(struct ui *ui, int x, int y, int w, int h);                       /* the base-coloured column */
void ui_side_label(struct ui *ui, int x, int y, const char *s);                   /* group label, 14 tall at y */
/* sidebar row (28 tall, 8px outer margins); selected gets the selection fill and the amber marker */
bool ui_side_item(struct ui *ui, int x, int y, int w, int icon, const char *label, const char *trail, bool sel);
void ui_toolbar(struct ui *ui, int x, int y, int w);                               /* H_TOOLBAR strip, 1px rule below */
void ui_vsep(struct ui *ui, int x, int y, int h);                                  /* 1px line divider */
/* breadcrumbs (28 tall); returns the index clicked or -1. The last part is the current place */
int  ui_crumbs(struct ui *ui, int x, int y, int maxw, const char *const *parts, int n);
void ui_status(struct ui *ui, int x, int y, int w, const char *left, const char *right);   /* H_STATUS bar */
/* overlay scrollbar in the 12px strip at x (thumb 6px, 10px on hover; fades 800ms after scrolling stops);
 * for content of `total` rows with `visible` shown; *first is dragged, the track pages */
void ui_vscroll(struct ui *ui, int x, int y, int h, int total, int visible, int *first);

/* ---- containers and feedback ---- */
void ui_panel(struct ui *ui, int x, int y, int w, int h);                         /* = ui_card */
void ui_card(struct ui *ui, int x, int y, int w, int h);                          /* raised, R_3, 1px line ring */
/* a settings row inside a card: label (+ desc in T_CAPTION text2) at the left, a rule above unless first;
 * put the control at the right yourself */
void ui_card_row(struct ui *ui, int x, int y, int w, int h, const char *label, const char *desc, bool first);
void ui_progress(struct ui *ui, int x, int y, int w, int v, int max);             /* v < 0: indeterminate */
void ui_spinner(struct ui *ui, int x, int y);                                     /* 16px */
/* centred empty state: 48px symbolic icon, heading, one line of help, an optional secondary button (clicked: true) */
bool ui_empty(struct ui *ui, int x, int y, int w, int h, int icon, const char *title, const char *body, const char *btn);
/* show a tooltip after D_TOOLTIP when the pointer rests on this rectangle */
void ui_tooltip(struct ui *ui, int x, int y, int w, int h, const char *text);
/* in-window toast for D_TOAST ms, centred 24px above the bottom; action (or NULL) is a ghost button */
void ui_toast(struct ui *ui, const char *msg, const char *action);
bool ui_toast_action(struct ui *ui);           /* true once after the toast's action was clicked */

/* modal dialogs: draw them last, every frame, while open. Return 1 = confirmed, -1 = cancelled, 0 = still open.
 * Buttons are right-aligned, the primary rightmost; Enter activates the focused button (the primary, or Cancel
 * for UI_DANGER), Esc cancels. style: UI_PRIMARY or UI_DANGER */
int ui_dialog(struct ui *ui, const char *title, const char *body, const char *cancel, const char *ok, int style);
/* ui_prompt edits buf (focus is taken automatically); ui_confirm just asks */
int ui_prompt(struct ui *ui, const char *title, const char *okl, char *buf, int cap);
int ui_confirm(struct ui *ui, const char *title, const char *text, const char *okl);
