/* theme.h - one set of named colours for the compositor, terminals and libgui/ui widgets.
 * The kernel holds the active theme (/dev/theme); apps fetch it and repaint on EV_THEME. Freestanding.
 * docs/DESIGN.md is the spec; sizes, radii and durations live in design.h. */
#pragma once
#include "gfx.h"

/* terminal colours (common/term.c): the 16 ANSI colours plus what the emulator paints itself */
struct term_colors {
    color_t pal[16];
    color_t fg, bg, bold;           /* default text, background, bold default text */
    color_t sel, hit, hitcur;       /* selection, search matches, the current match */
    color_t hitcur_fg, link;        /* text on hitcur, hovered link */
};

struct theme {
    char name[16];                  /* id: `theme NAME`, /etc/theme */
    char label[20];
    int dark;                       /* 1 = light text on dark surfaces */
    /* ---- Graphite tokens (docs/DESIGN.md §2): what new and restyled code draws with ---- */
    color_t base, window, raised, overlay;          /* surfaces, one tone step apart (dark: each lighter) */
    color_t line, line_strong, control;             /* separators/overlay outlines; secondary-button outline; 3:1 control edges and tracks */
    color_t text, text2, text3, disabled;           /* primary, secondary (>=4.5 everywhere), placeholder/caption (>=4.5 on base/window/raised) */
    color_t accent, accent_hover, accent_press, on_accent;
    color_t accent_text, accent_line;               /* accent as text/links/focus ring (>=4.5); signal line and accent-fill rim (>=3) */
    color_t selection, hover_ov, press_ov, scrim;   /* opaque selection fill; translucent hover/press backplates; modal scrim */
    color_t danger, on_danger, warning, success, info;
    color_t thumb, hilite, ring;                    /* switch thumb; dark-window inner top highlight; E2/E3 window ring */
    int elev[5];                                    /* shadow alpha 0..255 for E0..E4 (geometry in design.h) */
    /* terminal */
    struct term_colors term;
};

/* what /dev/theme returns on read; writes are text: "NAME accent=RRGGBB|default anim=0|1 zoom=0..3" in any combination.
 * /etc/theme holds the same text and is applied at boot. */
struct theme_state {
    uint32_t size;                  /* sizeof(struct theme_state) */
    uint32_t serial;                /* changes on every switch */
    int32_t index;                  /* into the built-in table */
    uint32_t accent;                /* 0xFFRRGGBB override, 0 = the theme's own */
    uint32_t anim;                  /* window animations on */
    uint32_t zoom;                  /* terminal text size step 0..3 */
    struct theme th;                /* resolved: theme + accent override */
};

/* overlay / accent tints with an alpha, for any struct theme *t */
#define TH_A(col, a) (((col) & 0xFFFFFFu) | (uint32_t)(a) << 24)

#define THEME_ACCENTS 9
struct theme_accent { const char *name; color_t rgb; };
extern const struct theme_accent theme_accents[THEME_ACCENTS];

int theme_count(void);
const struct theme *theme_get(int i);                  /* the built-in table; i wraps */
int theme_find(const char *name);                      /* -1 if unknown; "dark"/"light" still name graphite/graphite-light */
int theme_accent_find(const char *name);               /* index into theme_accents, -1 */
/* t's accent set to rgb, with every accent-derived colour recomputed and nudged to its contrast floor (rgb == 0: untouched) */
void theme_set_accent(struct theme *t, color_t rgb);
/* WCAG contrast ratio of two opaque colours, times 100 (21:1 -> 2100) */
int theme_contrast(color_t a, color_t b);
