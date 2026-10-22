/* design.h - the Graphite size, radius, elevation, type and motion tokens (docs/DESIGN.md), for the kernel and userland.
 * Colours are per theme (theme.h). Widget code uses these names, never literals. */
#pragma once
#include "font.h"

/* spacing: 4px base, 2 only for optical nudges */
enum { SP_0 = 0, SP_1 = 2, SP_2 = 4, SP_3 = 8, SP_4 = 12, SP_5 = 16, SP_6 = 20, SP_7 = 24, SP_8 = 32, SP_9 = 48 };

/* radii by role; R_F(h) is fully round for a shape h tall */
enum { R_0 = 0, R_1 = 2, R_2 = 4, R_3 = 8 };
#define R_F(h) ((h) / 2)

/* sizes (px at 1x) */
enum {
    H_CTL = 28, H_CTL_SM = 24,                  /* buttons, inputs, dropdowns, segmented; compact toolbar buttons */
    H_SEARCH = 32, H_SEARCH_LG = 36,            /* search fields; the launcher search (the only 36) */
    H_ROW = 28, H_ROW2 = 44,                    /* list/menu rows: one line, two lines */
    H_TITLE = 32, H_HEADER = 44,                /* window title bar; merged header bar */
    H_PANEL = 40, H_TOOLBAR = 40, H_TAB = 32, H_STATUS = 28, H_FOOTER = 40,
    W_CTL_MIN = 72,                             /* dialog button min width */
    W_SIDEBAR = 200, W_SIDEBAR_WIDE = 220,
    W_MENU_MIN = 180, W_MENU_MAX = 320, W_TIP_MAX = 280,
    W_ALERT = 400, W_FORM = 480, W_NOTIF = 360, W_LAUNCHER = 560, H_LAUNCHER = 520,
    W_TASK_MIN = 120, W_TASK_MAX = 200, H_TASK = 32,
    W_WINCTL = 32, H_WINCTL = 24,               /* title bar control hit area */
    ICON_SM = 16, ICON_MD = 20, ICON_LG = 24, ICON_XL = 32, ICON_APP = 48,
    CHECK = 16, SWITCH_W = 36, SWITCH_H = 20, SWITCH_THUMB = 14, SLIDER_TRACK = 4, SLIDER_THUMB = 16, SLIDER_THUMB_HOT = 18,
    SCROLL_W = 6, SCROLL_W_HOT = 10, SCROLL_MIN = 32, SIGNAL = 2, MARKER_W = 3, MARKER_H = 16,
    RESIZE_BORDER = 6, HIT_MIN = 24,
};

/* focus ring: 2px accent_text, 1px outside the control, radius + offset (gfx_focus_ring) */
enum { FOCUS_W = 2, FOCUS_GAP = 1 };

/* elevation: black shadows offset down, blurred (sigma = blur / 2); alpha comes from theme.elev[level].
 * E0 in-window, E1 thumbs, E2 menus/popovers/tooltips/toasts/inactive windows, E3 focused window, E4 dialogs/launcher. */
enum { E0, E1, E2, E3, E4 };
struct elevation { signed char dy, blur; };
static const struct elevation ELEV[5] = { { 0, 0 }, { 1, 2 }, { 4, 12 }, { 8, 24 }, { 16, 40 } };

/* motion, ms; exits run at 0.7x; reduce-motion turns every duration to 0 except fades (D_REDUCED) */
enum { D_INSTANT = 0, D_FAST = 100, D_NORMAL = 150, D_SLOW = 200, D_WINDOW = 250, D_REDUCED = 83,
       D_TOOLTIP = 600, D_TOOLTIP_CHAIN = 1500, D_SUBMENU = 200, D_TOAST = 4000, D_NOTIF = 5000, D_SCROLL_FADE = 800,
       D_CARET = 530, D_CARET_IDLE = 10000 };
#define D_EXIT(d) ((d) * 7 / 10)

/* type roles (docs/DESIGN.md §3). FONT_RAMP is defined by the generated font.h once tools/mkfont.py bakes the Inter
 * ramp; until then the roles fall back to the nearest Noto cut so code can switch to the role names now. */
#ifdef FONT_RAMP
#define T_CAPTION      (&font_caption)          /* 11/14 400 */
#define T_CAPTION_B    (&font_caption_bold)     /* 11/14 600 */
#define T_BODY         (&font_body)             /* 13/18 400 */
#define T_BODY_B       (&font_body_bold)        /* 13/18 600 */
#define T_HEADING      (&font_heading)          /* 15/20 600 */
#define T_TITLE        (&font_title)            /* 20/26 600 */
#define T_DISPLAY      (&font_display)          /* 28/34 600 */
#else
#define T_CAPTION      (&font_ui)
#define T_CAPTION_B    (&font_ui_bold)
#define T_BODY         (&font_ui)
#define T_BODY_B       (&font_ui_bold)
#define T_HEADING      (&font_ui_bold)
#define T_TITLE        (&font_title)
#define T_DISPLAY      (&font_title)
#endif
#define T_MONO         (&font_mono)             /* 13/18 400: terminal, code, hex, logs only */
