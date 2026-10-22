/* wm.h - in-kernel compositor and window manager */
#pragma once
#include "proc.h"
#include "gfx.h"

#define WM_TITLE_H   34
#define WM_RADIUS    10
#define WM_TASKBAR_H 44
#define WM_WS        4

struct window {
    int id;
    uint32_t born;              /* ticks_ms at creation, for the open animation */
    int x, y, w, h;                 /* client rect, screen coords */
    int flags;                      /* WIN_FLAG_* */
    char title[64];
    uint32_t *buf;                  /* stride = canvas.stride (>= screen width), sized for the whole screen */
    uint32_t buf_frames;
    struct canvas canvas;           /* over buf, w x h */
    int owner_pid;
    uint32_t owner_dir, user_va;
    struct vnode vn;                /* user windows: the fd's node */
    struct gui_event q[128];
    uint32_t qh, qt;
    bool minimized, maximized, visible, topmost;
    int ws;                         /* workspace 0..WM_WS-1, -1 = on all (toasts) */
    struct rect restore;
    /* kernel-side windows (terminals) */
    void (*on_event)(struct window *w, const struct gui_event *ev);
    void (*on_frame)(struct window *w);
    void (*on_destroy)(struct window *w);
    void *priv;
    struct window *next;            /* z order, bottom first */
};

void wm_init(void);                         /* takes over the screen, starts the compositor thread */
bool wm_running(void);
int  wm_set_mode(int w, int h);              /* switch the display mode, re-lay-out the desktop */
void wm_wake(void);
struct window *wm_create(const char *title, int x, int y, int w, int h, int flags);
void wm_destroy(struct window *w);
void wm_damage(int x, int y, int w, int h); /* screen coords */
void wm_damage_client(struct window *w, int x, int y, int cw, int ch);
void wm_set_title(struct window *w, const char *t);
void wm_focus(struct window *w);
struct window *wm_focused(void);
void wm_post(struct window *w, const struct gui_event *ev);   /* queue for a user window */
int  wm_spawn_app(const char *path, const char *arg);
void wm_notify(const char *msg, int n);                       /* "title\nbody[\nkind]", from /dev/notify */

/* gui/termwin.c */
struct window *termwin_open(const char *cmd);              /* NULL = interactive shell */
struct window *termwin_open_argv(char *const argv[], bool hold);
