/* gui.h - thin helper over the window syscalls. Link order: libgui.a before libc.a. */
#pragma once
#include "abi.h"
#include "gfx.h"
#include "theme.h"

struct gui_window {
    int fd;
    int w, h;
    uint32_t *pixels;       /* 0xAARRGGBB, stride pixels per row */
    int stride;
    struct canvas cv;       /* draw here, then gui_flip() */
};

/* NULL on failure (errno set). flags: WIN_FLAG_* */
struct gui_window *gui_window_create(const char *title, int w, int h, int flags);
void gui_window_destroy(struct gui_window *win);
void gui_flip(struct gui_window *win);
void gui_flip_rect(struct gui_window *win, int x, int y, int w, int h);
/* 1 = event stored, 0 = timeout, -1 = error. timeout_ms < 0 blocks. EV_RESIZE is applied
 * (canvas rebuilt) before it is returned. */
int gui_next_event(struct gui_window *win, struct gui_event *ev, int timeout_ms);
int gui_set_title(struct gui_window *win, const char *title);
int gui_resize(struct gui_window *win, int w, int h);

/* system clipboard (/dev/clipboard, shared with terminal windows) */
int   gui_clip_set(const char *s, int n);   /* 0 or -1 */
char *gui_clip_get(int *n);                 /* malloc'd, NUL-terminated; NULL if empty */

/* desktop notification (/dev/notify): a toast plus an entry in the notification center.
 * kind: NULL (accent), "good", "warn" or "bad". 0 or -1. */
int gui_notify(const char *title, const char *body, const char *kind);

/* the desktop theme (/dev/theme): loaded when the first window is created and refreshed when EV_THEME arrives,
 * so UI_* colours in ui.h are always current; draw everything again after that event */
extern struct theme ui_th;
int gui_theme_load(void);                       /* re-read /dev/theme into ui_th; 0 or -1 */
int gui_theme_state(struct theme_state *st);    /* index, accent override, animations, text size; 0 or -1 */
int gui_theme_set(const char *cmd);             /* write a /dev/theme command ("light", "accent=blue anim=0"); 0 or -1 */
int gui_theme_save(void);                       /* write the current settings to /etc/theme so the next boot applies them */
/* decode an image (PNG/JPEG/...), scale it to cover the screen and hand it to the desktop as the wallpaper; 0 or -1 */
int gui_wallpaper_push(const char *path);
