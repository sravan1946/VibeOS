/* theme.h - one set of named colours for the compositor, terminals and libgui/ui widgets.
 * The kernel holds the active theme (/dev/theme); apps fetch it and repaint on EV_THEME. Freestanding. */
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
    char name[12];                  /* id: `theme NAME`, /etc/theme */
    char label[20];
    int dark;                       /* 1 = light text on dark surfaces */
    /* widgets (libgui/ui.h) */
    color_t bg, panel, surface, hover, pressed, border, input;
    color_t text, dim, faint;       /* body text, secondary text, decorative (line numbers, close glyphs) */
    color_t accent, accent_hi, accent_lo, on_accent, accent2;
    color_t bad, warn, good;        /* status colours; also readable as text on bg/panel */
    color_t knob, knob_on, ov;      /* ov: the colour hover/divider overlays are drawn in (white on dark, black on light) */
    color_t key, key_hi, key_lo, key_fg, gutter;   /* accent-tinted key (Calculator operators), editor gutter */
    int edge;                       /* outline strength, percent (high contrast: strong) */
    int shadow;                     /* window shadow strength, percent */
    /* compositor */
    color_t frame, frame_off, btn_off, cdim, body, on_pill;
    color_t bar, launcher, popup, toast, switcher;     /* translucent, so alpha counts */
    /* terminal */
    struct term_colors term;
    color_t tbar, tab, tab_off, tab_fg, tab_dim, tab_faint, tfind, tab_acc;
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
int theme_find(const char *name);                      /* -1 if unknown */
int theme_accent_find(const char *name);               /* index into theme_accents, -1 */
/* t's accent set to rgb, with the hover/pressed/key/on-accent colours derived from it (rgb == 0: untouched) */
void theme_set_accent(struct theme *t, color_t rgb);
/* WCAG contrast ratio of two opaque colours, times 100 (21:1 -> 2100) */
int theme_contrast(color_t a, color_t b);
