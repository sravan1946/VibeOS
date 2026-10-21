/* pipe.c - anonymous pipes */
#include "proc.h"

#define PIPE_BUF_SIZE 8192
struct pipe {
    struct vnode vn;
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head, tail;
    int readers, writers;
};

static int pipe_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) {
    struct pipe *p = v->priv;
    uint32_t f = irq_save();
    while (p->head == p->tail) {
        if (!p->writers) { irq_restore(f); return 0; }
        if (sleep_on(p, 0) < 0) { irq_restore(f); return -EINTR; }
    }
    uint32_t got = 0;
    while (got < n && p->head != p->tail) ((uint8_t *)buf)[got++] = p->buf[p->tail++ % PIPE_BUF_SIZE];
    wakeup(p);
    irq_restore(f);
    return got;
}

static int pipe_write(struct vnode *v, uint32_t off, const void *buf, uint32_t n) {
    struct pipe *p = v->priv;
    uint32_t f = irq_save(), put = 0;
    while (put < n) {
        if (!p->readers) { irq_restore(f); return put ? (int)put : -EPIPE; }
        if (p->head - p->tail == PIPE_BUF_SIZE) {
            wakeup(p);
            if (sleep_on(p, 0) < 0) { irq_restore(f); return put ? (int)put : -EINTR; }
            continue;
        }
        p->buf[p->head++ % PIPE_BUF_SIZE] = ((const uint8_t *)buf)[put++];
    }
    wakeup(p);
    irq_restore(f);
    return put;
}

static int pipe_poll(struct vnode *v, int ev) {
    struct pipe *p = v->priv;
    int r = 0;
    if (p->head != p->tail || !p->writers) r |= POLLIN;
    if (p->head - p->tail < PIPE_BUF_SIZE || !p->readers) r |= POLLOUT;
    if (!p->writers || !p->readers) r |= POLLHUP;
    return r & (ev | POLLHUP);
}

static void pipe_close(struct vnode *v, int flags) {
    struct pipe *p = v->priv;
    if ((flags & O_ACCMODE) == O_RDONLY) p->readers--; else p->writers--;
    wakeup(p);
}

static void pipe_release(struct vnode *v) { kfree(v->priv); }

static const struct vnode_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .poll = pipe_poll, .close = pipe_close, .release = pipe_release,
};

int pipe_create(struct file **rd, struct file **wr) {
    struct pipe *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    p->vn.mode = S_IFIFO | 0600;
    p->vn.ops = &pipe_ops;
    p->vn.priv = p;
    p->readers = p->writers = 1;
    *rd = vfs_open_vnode(&p->vn, O_RDONLY);
    *wr = vfs_open_vnode(&p->vn, O_WRONLY);
    return 0;
}
