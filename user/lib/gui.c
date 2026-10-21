#include "gui.h"
#include <syscall.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

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
    if (ev->type == EV_RESIZE) {
        /* ASSUMPTION (abi.h is vague): WIN_SET_SIZE takes a struct win_info * with w/h set and
         * returns the new pixels/stride in it. */
        struct win_info wi;
        memset(&wi, 0, sizeof wi);
        wi.w = ev->w;
        wi.h = ev->h;
        if (win_set(win->fd, WIN_SET_SIZE, (uintptr_t)&wi) == 0 && wi.pixels)
            attach(win, wi.w, wi.h, wi.pixels, wi.stride);
    }
    return 1;
}
