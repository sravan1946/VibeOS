/* term.h - VT100/xterm-style terminal emulator core (cell grid + software renderer).
 * Shared by kernel terminal windows and userland.  Needs only gfx.h/font.h and
 * kmalloc/kfree/memset/memcpy/memmove from the host. */
#pragma once
#include <stdint.h>
#include "gfx.h"

#define TERM_DEFAULT_BG 0xFF11131Cu
#define TERM_DEFAULT_FG 0xFFC8CEE8u

struct term;

struct term *term_create(int cols, int rows);              /* NULL on OOM */
void term_destroy(struct term *t);
void term_write(struct term *t, const char *s, int n);     /* feed program output (UTF-8) */
void term_resize(struct term *t, int cols, int rows);
void term_size_for_pixels(int pw, int ph, int *cols, int *rows);
int  term_cell_w(void);
int  term_cell_h(void);
void term_render(struct term *t, struct canvas *c, int x, int y, int full, struct rect *damage);
void term_set_cursor_blink_phase(struct term *t, int on);
void term_set_focused(struct term *t, int focused);
void term_scroll_view(struct term *t, int lines);          /* +up/-down, 0 = bottom */
void term_set_reply(struct term *t, void (*fn)(void *ctx, const char *s, int n), void *ctx);
int  term_cols(struct term *t);
int  term_rows(struct term *t);
const char *term_title(struct term *t);
int  term_take_bell(struct term *t);                       /* returns and clears the BEL flag */
/* mouse selection in visible cell coordinates (clamped, either order); it follows the text as it scrolls */
void term_select(struct term *t, int c0, int r0, int c1, int r1);
void term_select_clear(struct term *t);
int  term_selection_text(struct term *t, char *buf, int n);  /* UTF-8, trailing blanks trimmed; 0 if none */

/* introspection (tests, selection/copy): row < 0 addresses scrollback, -1 = newest line */
uint32_t term_cell_char(struct term *t, int col, int row);
uint32_t term_cell_fg(struct term *t, int col, int row);   /* resolved 0xAARRGGBB, before bold/inverse */
uint32_t term_cell_bg(struct term *t, int col, int row);
int      term_cell_attr(struct term *t, int col, int row); /* TERM_A_* bits */
void     term_cursor(struct term *t, int *col, int *row);
int      term_scrollback_lines(struct term *t);
int      term_view_offset(struct term *t);
int      term_on_alt_screen(struct term *t);

#define TERM_A_BOLD    1
#define TERM_A_DIM     2
#define TERM_A_ITALIC  4
#define TERM_A_UNDER   8
#define TERM_A_INVERSE 16
#define TERM_A_STRIKE  32
