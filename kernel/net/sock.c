/* sock.c - sockets as fds (UDP, raw ICMP, TCP), the DNS resolver and the net syscalls */
#include "net.h"

struct sock *socks;
static uint16_t eph = 49151;

static bool uok(const void *p, uint32_t n, bool w) { return n == 0 || vmm_user_ok(current->dir, (uint32_t)p, n, w); }

static struct sock *sock_new(int type) {
    struct sock *s = kzalloc(sizeof *s);
    if (!s) return 0;
    s->type = type;
    s->next = socks; socks = s;
    return s;
}

void sock_free(struct sock *s) {
    for (struct sock **pp = &socks; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    while (s->qh) { struct dgram *d = s->qh; s->qh = d->next; kfree(d); }
    kfree(s->rxb); kfree(s->txb);
    kfree(s);
}

static bool port_used(uint16_t port) {
    for (struct sock *s = socks; s; s = s->next) if (s->lport == port) return true;
    return false;
}

static void autobind(struct sock *s) {
    if (s->lport) return;
    do { if (++eph < 49152) eph = 49152; } while (port_used(htons(eph)));
    s->lport = htons(eph);
}

/* ---- datagram queues ---- */
static void enqueue(struct sock *s, uint32_t ip, uint16_t port, const uint8_t *p, uint32_t n) {
    if (s->qn >= 32) return;
    struct dgram *d = kmalloc(sizeof *d + n);
    if (!d) return;
    d->next = 0; d->ip = ip; d->port = port; d->len = n;
    memcpy(d->data, p, n);
    if (s->qt) s->qt->next = d; else s->qh = d;
    s->qt = d; s->qn++;
    wakeup(s);
}

static struct dgram *dequeue(struct sock *s) {
    struct dgram *d = s->qh;
    if (!d) return 0;
    if (!(s->qh = d->next)) s->qt = 0;
    s->qn--;
    return d;
}

void sock_udp_rx(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *p, uint32_t n) {
    for (struct sock *s = socks; s; s = s->next)
        if (s->type == SOCK_DGRAM && s->lport == dport && (!s->connected || (s->rip == src && s->rport == sport)))
            enqueue(s, src, sport, p, n);
}

void sock_raw_rx(uint32_t src, const uint8_t *p, uint32_t n) {
    for (struct sock *s = socks; s; s = s->next)
        if (s->type == SOCK_RAW) enqueue(s, src, 0, p, n);
}

/* ---- DNS (A records over UDP to the DHCP-provided server) ---- */
static int skip_name(const uint8_t *p, uint32_t n, uint32_t *pos) {
    for (;;) {
        if (*pos >= n) return -1;
        uint8_t c = p[*pos];
        if (!c) { (*pos)++; return 0; }
        if ((c & 0xC0) == 0xC0) { *pos += 2; return 0; }
        *pos += c + 1;
    }
}

static int dns_parse(const uint8_t *p, uint32_t n, uint16_t id, uint32_t *out) {
    if (n < 12 || (p[0] << 8 | p[1]) != id || !(p[2] & 0x80)) return -EAGAIN;   /* not our answer */
    if (p[3] & 15) return -ENOENT;
    uint32_t pos = 12, qd = p[4] << 8 | p[5], an = p[6] << 8 | p[7];
    for (uint32_t i = 0; i < qd; i++) {
        if (skip_name(p, n, &pos) < 0) return -EIO;
        pos += 4;
    }
    for (uint32_t i = 0; i < an; i++) {
        if (skip_name(p, n, &pos) < 0 || pos + 10 > n) return -EIO;
        uint32_t type = p[pos] << 8 | p[pos + 1], rdlen = p[pos + 8] << 8 | p[pos + 9];
        if (type == 1 && rdlen == 4 && pos + 14 <= n) { memcpy(out, p + pos + 10, 4); return 0; }
        pos += 10 + rdlen;
    }
    return -ENOENT;
}

static int dns_resolve(const char *name, uint32_t *out) {
    if (!ip_parse(name, out)) return 0;
    int r = net_wait_cfg();
    if (r < 0) return r;
    if (!ni.dns) return -ENETDOWN;
    uint8_t q[300];
    uint16_t id = (uint16_t)(ticks_ms * 40503u);
    uint32_t ql = 12;
    memset(q, 0, 12);
    q[0] = id >> 8; q[1] = id; q[2] = 1; q[5] = 1;                 /* recursion desired, 1 question */
    for (const char *p = name; *p; ) {
        const char *e = p;
        while (*e && *e != '.') e++;
        uint32_t l = e - p;
        if (!l || l > 63 || ql + l + 6 > sizeof q) return -EINVAL;
        q[ql++] = l; memcpy(q + ql, p, l); ql += l;
        p = *e ? e + 1 : e;
    }
    q[ql++] = 0; q[ql++] = 0; q[ql++] = 1; q[ql++] = 0; q[ql++] = 1;   /* type A, class IN */
    struct sock *s = sock_new(SOCK_DGRAM);
    if (!s) return -ENOMEM;
    autobind(s);
    r = -ETIMEDOUT;
    for (int t = 0; t < 3 && r == -ETIMEDOUT; t++) {
        udp_send(0, s->lport, ni.dns, htons(53), q, ql);
        uint64_t end = ticks_ms + 1500;
        while (ticks_ms < end) {
            if (!s->qh && nwait(s, end - ticks_ms) < 0) { r = -EINTR; break; }
            struct dgram *d = dequeue(s);
            if (!d) continue;
            int rr = d->ip == ni.dns ? dns_parse(d->data, d->len, id, out) : -EAGAIN;
            kfree(d);
            if (rr != -EAGAIN) { r = rr; break; }
        }
        if (r == -EINTR) break;
    }
    sock_free(s);
    return r;
}

/* ---- vnode ops ---- */
static int sock_sendto(struct sock *s, const uint8_t *buf, uint32_t n, const struct sockaddr_in *to) {
    if (s->type == SOCK_STREAM) return tcp_write(s, buf, n);
    uint32_t ip = s->rip; uint16_t port = s->rport;
    if (to) { ip = to->sin_addr; port = to->sin_port; }
    else if (!s->connected) return -ENOTCONN;
    if (n > 1472) return -EMSGSIZE;
    int r = net_wait_cfg();
    if (r < 0) return r;
    uint8_t k[1480];
    memcpy(k, buf, n);
    if (s->type == SOCK_RAW) {                                      /* buf is a whole ICMP message */
        if (n < 8) return -EINVAL;
        k[2] = k[3] = 0;
        uint16_t c = ip_csum(k, n);
        memcpy(k + 2, &c, 2);
        r = ip_send(1, 0, ip, k, n);
    } else {
        autobind(s);
        r = udp_send(0, s->lport, ip, port, k, n);
    }
    return r < 0 ? r : (int)n;
}

static int sock_recvfrom(struct sock *s, uint8_t *buf, uint32_t n, int flags, struct sockaddr_in *from) {
    if (s->type == SOCK_STREAM) {
        if (flags & MSG_DONTWAIT && s->rx_h == s->rx_t && !s->rcvd_fin && !s->err && s->state != TCP_CLOSED) return -EAGAIN;
        return tcp_read(s, buf, n);
    }
    while (!s->qh) {
        if (flags & MSG_DONTWAIT) return -EAGAIN;
        if (nwait(s, 0) < 0) return -EINTR;
    }
    struct dgram *d = dequeue(s);
    uint32_t k = MIN(n, (uint32_t)d->len);
    memcpy(buf, d->data, k);
    if (from) *from = (struct sockaddr_in){ AF_INET, d->port, d->ip, { 0 } };
    kfree(d);
    return k;
}

static int vn_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) { return sock_recvfrom(v->priv, buf, n, 0, 0); }
static int vn_write(struct vnode *v, uint32_t off, const void *buf, uint32_t n) { return sock_sendto(v->priv, buf, n, 0); }

static int vn_poll(struct vnode *v, int ev) {
    struct sock *s = v->priv;
    int r = 0;
    if (s->type != SOCK_STREAM) return (s->qh ? POLLIN : 0) | POLLOUT;
    if (s->rx_h != s->rx_t || s->rcvd_fin || s->err) r |= POLLIN;
    if ((s->state == TCP_ESTAB || s->state == TCP_CLOSE_WAIT) && s->tx_len < TXSZ) r |= POLLOUT;
    if (s->state == TCP_CLOSED) r |= POLLHUP | POLLIN;
    return r & (ev | POLLHUP);
}

static int vn_ioctl(struct vnode *v, int req, void *arg) {
    struct sock *s = v->priv;
    if (req != FIONREAD) return -ENOTTY;
    *(int *)arg = s->type == SOCK_STREAM ? (int)(s->rx_h - s->rx_t) : s->qh ? s->qh->len : 0;
    return 0;
}

static void vn_close(struct vnode *v, int flags) {
    struct sock *s = v->priv;
    if (s->type == SOCK_STREAM && s->state != TCP_CLOSED) tcp_close(s);   /* lingers until the handshake ends */
    else sock_free(s);
}

static void vn_release(struct vnode *v) { kfree(v); }

static const struct vnode_ops sock_ops = {
    .read = vn_read, .write = vn_write, .poll = vn_poll, .ioctl = vn_ioctl, .close = vn_close, .release = vn_release,
};

static struct sock *fd_sock(int fd) {
    struct file *f = fd_get(fd);
    return f && f->vn->ops == &sock_ops ? f->vn->priv : 0;
}

/* ---- syscalls ---- */
static int sys_socket(int domain, int type, int proto) {
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW) return -EPROTONOSUPPORT;
    if (type == SOCK_RAW && proto != IPPROTO_ICMP) return -EPROTONOSUPPORT;
    if (!ni.present) return -ENODEV;
    struct sock *s = sock_new(type);
    struct vnode *v = kzalloc(sizeof *v);
    if (!s || !v) { kfree(v); if (s) sock_free(s); return -ENOMEM; }
    v->mode = S_IFSOCK | 0600; v->ops = &sock_ops; v->priv = s;
    struct file *f = vfs_open_vnode(v, O_RDWR);
    if (!f) { kfree(v); sock_free(s); return -ENOMEM; }
    int fd = fd_alloc(f);
    if (fd < 0) vfs_close(f);                    /* close op frees the sock, release the vnode */
    return fd;
}

static int get_addr(const void *u, uint32_t len, struct sockaddr_in *a) {
    if (len < sizeof *a || !uok(u, sizeof *a, false)) return -EFAULT;
    *a = *(const struct sockaddr_in *)u;
    return a->sin_family == AF_INET ? 0 : -EAFNOSUPPORT;
}

static int sys_connect(int fd, const void *ua, uint32_t len) {
    struct sock *s = fd_sock(fd);
    struct sockaddr_in a;
    if (!s) return -EBADF;
    int r = get_addr(ua, len, &a);
    if (r < 0) return r;
    if ((r = net_wait_cfg()) < 0) return r;
    s->rip = a.sin_addr; s->rport = a.sin_port;
    if (s->type == SOCK_STREAM) {
        if (s->rxb) return -EINVAL;              /* one connect per socket */
        autobind(s);
        return tcp_connect(s);
    }
    s->connected = true;
    if (s->type == SOCK_DGRAM) autobind(s);
    return 0;
}

static int sys_bind(int fd, const void *ua, uint32_t len) {
    struct sock *s = fd_sock(fd);
    struct sockaddr_in a;
    if (!s) return -EBADF;
    int r = get_addr(ua, len, &a);
    if (r < 0) return r;
    if (s->lport) return -EINVAL;
    if (a.sin_port && port_used(a.sin_port)) return -EADDRINUSE;
    s->lport = a.sin_port;
    autobind(s);
    return 0;
}

static int sys_resolve(const char *uname, uint32_t *uip) {
    char name[256];
    if (!uok(uip, 4, true)) return -EFAULT;
    for (uint32_t i = 0; i < sizeof name; i++) {
        if (!uok(uname + i, 1, false)) return -EFAULT;
        if (!(name[i] = uname[i])) break;
        if (i == sizeof name - 1) return -ENAMETOOLONG;
    }
    uint32_t ip;
    int r = dns_resolve(name, &ip);
    if (r == 0) *uip = ip;
    return r;
}

int net_syscall(int num, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    struct sock *s;
    switch (num) {
    case SYS_socket:  return sys_socket(a, b, c);
    case SYS_connect: return sys_connect(a, (const void *)b, c);
    case SYS_bind:    return sys_bind(a, (const void *)b, c);
    case SYS_sendto: {
        struct sockaddr_in to;
        if (!(s = fd_sock(a))) return -EBADF;
        if (!uok((void *)b, c, false)) return -EFAULT;
        if (e) { int r = get_addr((void *)e, sizeof to, &to); if (r < 0) return r; }
        return sock_sendto(s, (const uint8_t *)b, c, e ? &to : 0);
    }
    case SYS_recvfrom: {
        if (!(s = fd_sock(a))) return -EBADF;
        if (!uok((void *)b, c, true) || (e && !uok((void *)e, sizeof(struct sockaddr_in), true))) return -EFAULT;
        return sock_recvfrom(s, (uint8_t *)b, c, d, (struct sockaddr_in *)e);
    }
    case SYS_resolve: return sys_resolve((const char *)a, (uint32_t *)b);
    case SYS_netinfo:
        if (!uok((void *)a, sizeof ni, true)) return -EFAULT;
        *(struct netinfo *)a = ni;
        return 0;
    }
    return -ENOSYS;
}
