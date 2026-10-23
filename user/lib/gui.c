#include "gui.h"
#include <syscall.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include "image.h"

static void attach(struct gui_window *win, int w, int h, uint32_t *px, int stride) {
    win->w = w;
    win->h = h;
    win->pixels = px;
    win->stride = stride;
    canvas_init(&win->cv, px, w, h, stride);
}

struct theme ui_th;
int ui_motion = 1;

int gui_theme_state(struct theme_state *st) {
    int fd = open("/dev/theme", O_RDONLY), r = -1;
    if (fd >= 0) { r = read(fd, st, sizeof *st) == (int)sizeof *st && st->size == sizeof *st ? 0 : -1; close(fd); }
    return r;
}

int gui_theme_load(void) {
    struct theme_state st;
    if (gui_theme_state(&st) < 0) { if (!ui_th.name[0]) ui_th = *theme_get(0); return -1; }   /* no /dev/theme: the built-in default */
    ui_th = st.th;
    ui_motion = st.anim != 0;
    return 0;
}

int gui_theme_set(const char *cmd) {
    int fd = open("/dev/theme", O_WRONLY), r = -1, n = strlen(cmd);
    if (fd >= 0) { r = write(fd, cmd, n) == n ? 0 : -1; close(fd); }
    return r;
}

int gui_theme_save(void) {
    struct theme_state st;
    if (gui_theme_state(&st) < 0) return -1;
    char acc[16] = "default", m[96];
    if (st.accent) snprintf(acc, sizeof acc, "%06X", st.accent & 0xFFFFFF);
    int n = snprintf(m, sizeof m, "%s accent=%s anim=%d zoom=%d\n", theme_get(st.index)->name, acc, (int)st.anim, (int)st.zoom), r = -1;
    int fd = open("/etc/theme", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { r = write(fd, m, n) == n ? 0 : -1; close(fd); }
    return r;
}

int gui_wallpaper_push(const char *path) {
    struct image im;
    struct sysinfo si;
    if (image_load(path, &im) < 0) return -1;
    sysinfo(&si);
    int sw = si.screen_w, sh = si.screen_h, r = -1;
    size_t bytes = 12 + (size_t)sw * sh * 4;
    uint32_t *buf = malloc(bytes);
    if (buf) {
        float k = (float)sw / im.w > (float)sh / im.h ? (float)sw / im.w : (float)sh / im.h;    /* cover, centred */
        buf[0] = 0x4D495057u; buf[1] = sw; buf[2] = sh;
        image_sample(&im, buf + 3, sw, sh, sw, im.w / 2.0f - sw / (2 * k), im.h / 2.0f - sh / (2 * k), k, 1);
        for (int i = 0; i < sw * sh; i++) buf[3 + i] = blend(0xFF000000u, buf[3 + i]);              /* transparency over black */
        int fd = open("/dev/wallpaper", O_WRONLY);
        if (fd >= 0) {
            size_t off = 0;
            while (off < bytes) {
                int k2 = write(fd, (char *)buf + off, bytes - off > 262144 ? 262144 : bytes - off);
                if (k2 <= 0) break;
                off += k2;
            }
            r = off == bytes ? 0 : -1;
            close(fd);
        }
        free(buf);
    }
    image_free(&im);
    return r;
}

struct gui_window *gui_window_create(const char *title, int w, int h, int flags) {
    if (!ui_th.name[0]) gui_theme_load();
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
    if (ev->type == EV_THEME) gui_theme_load();
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
