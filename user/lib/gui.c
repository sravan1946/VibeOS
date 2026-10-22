#include "gui.h"
#include <syscall.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

static void attach(struct gui_window *win, int w, int h, uint32_t *px, int stride) {
    win->w = w;
    win->h = h;
    win->pixels = px;
    win->stride = stride;
    canvas_init(&win->cv, px, w, h, stride);
}

struct gui_window *gui_window_create(const char *title, int w, int h, int flags) {
    struct win_info wi;
    memset(&wi, 0, sizeof wi);
    wi.x = wi.y = -1;
    wi.w = w;
    wi.h = h;
    wi.flags = flags;
    strlcpy(wi.title, title, sizeof wi.title);
    int fd = win_create(&wi);
    if (fd < 0) return NULL;
    struct gui_window *win = calloc(1, sizeof *win);
    if (!win) { close(fd); errno = ENOMEM; return NULL; }
    win->fd = fd;
    attach(win, wi.w, wi.h, wi.pixels, wi.stride);
    return win;
}

void gui_window_destroy(struct gui_window *win) {
    if (!win) return;
    close(win->fd);
    free(win);
}

void gui_flip(struct gui_window *win) { win_flip(win->fd, NULL); }

void gui_flip_rect(struct gui_window *win, int x, int y, int w, int h) {
    struct win_rect r = { x, y, w, h };
    win_flip(win->fd, &r);
}

int gui_set_title(struct gui_window *win, const char *title) {
    char buf[64];
    strlcpy(buf, title, sizeof buf);
    return win_set(win->fd, WIN_SET_TITLE, (uintptr_t)buf);
}

int gui_next_event(struct gui_window *win, struct gui_event *ev, int timeout_ms) {
    struct pollfd p = { win->fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return r;
    if (read(win->fd, ev, sizeof *ev) != sizeof *ev) return -1;
    if (ev->type == EV_RESIZE)      /* same mapping; x = the new stride after a display mode change, else 0 */
        attach(win, ev->w, ev->h, win->pixels, ev->x ? ev->x : win->stride);
    return 1;
}

/* ask the window manager for a new client size; the EV_RESIZE event confirms it */
int gui_resize(struct gui_window *win, int w, int h) {
    struct win_rect r = { 0, 0, w, h };
    return win_set(win->fd, WIN_SET_SIZE, (uintptr_t)&r);
}

int gui_clip_set(const char *s, int n) {
    int fd = open("/dev/clipboard", O_WRONLY | O_TRUNC), r = -1;
    if (fd >= 0) { r = write(fd, s, n) == n ? 0 : -1; close(fd); }
    return r;
}

int gui_notify(const char *title, const char *body, const char *kind) {
    char m[256];
    int n = snprintf(m, sizeof m, "%s\n%s\n%s", title, body ? body : "", kind ? kind : ""), r = -1;
    if (n >= (int)sizeof m) n = sizeof m - 1;
    int fd = open("/dev/notify", O_WRONLY);
    if (fd >= 0) { r = write(fd, m, n) == n ? 0 : -1; close(fd); }
    return r;
}

char *gui_clip_get(int *n) {
    int fd = open("/dev/clipboard", O_RDONLY), len = 0, cap = 0, r = 0;
    char *b = NULL;
    if (fd < 0) return NULL;
    do {
        if (len + 4096 + 1 > cap) { char *nb = realloc(b, cap = cap * 2 + 4096 + 1); if (!nb) break; b = nb; }
        r = read(fd, b + len, cap - len - 1);
        if (r > 0) len += r;
    } while (r > 0);
    close(fd);
    if (!len) { free(b); return NULL; }
    b[len] = 0;
    if (n) *n = len;
    return b;
}
