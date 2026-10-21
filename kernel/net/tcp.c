/* tcp.c - minimal TCP client: active open, in-order receive, go-back-N retransmit on timeout,
 * FIN/RST handling. No listen, no SACK/window scaling, no congestion control, fixed 500 ms base RTO. */
#include "net.h"

struct tcph { uint16_t sport, dport; uint32_t seq, ack; uint8_t off, flags; uint16_t win, csum, urg; } PACKED;
#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16
#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)

static uint32_t rx_free(struct sock *s) { return RXSZ - (s->rx_h - s->rx_t); }

static void seg_send(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t win, const uint8_t *data, uint32_t n) {
    uint8_t b[sizeof(struct tcph) + 4 + TCP_MSS];
    struct tcph *t = (void *)b;
    uint32_t hl = sizeof *t + (flags & SYN ? 4 : 0);
    *t = (struct tcph){ sport, dport, htonl(seq), htonl(ack), hl / 4 << 4, flags, htons(win), 0, 0 };
    if (flags & SYN) { b[20] = 2; b[21] = 4; b[22] = TCP_MSS >> 8; b[23] = TCP_MSS & 255; }
    if (n) memcpy(b + hl, data, n);
    t->csum = l4_csum(6, src ? src : ni.ip, dst, b, hl + n);
    ip_send(6, src, dst, b, hl + n);
}

static void seg(struct sock *s, uint32_t seq, uint8_t flags, const uint8_t *d, uint32_t n) {
    if (s->state != TCP_SYN_SENT) flags |= ACK;
    seg_send(0, s->rip, s->lport, s->rport, seq, s->rcv_nxt, flags, MIN(rx_free(s), 65535u), d, n);
}

static void arm(struct sock *s) { if (!s->rto_at) s->rto_at = ticks_ms + s->rto; }

/* send whatever the peer's window allows, plus the FIN once the buffer has drained */
void tcp_output(struct sock *s) {
    if (s->state != TCP_ESTAB && s->state != TCP_CLOSE_WAIT && s->state != TCP_FIN_WAIT1 &&
        s->state != TCP_LAST_ACK && s->state != TCP_CLOSING) return;
    for (;;) {
        uint32_t inflight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
        uint32_t avail = s->tx_len - inflight;
        uint32_t room = s->snd_wnd > inflight ? s->snd_wnd - inflight : 0;
        if (!inflight && !room && avail) room = 1;               /* zero-window probe */
        uint32_t n = MIN(MIN(avail, room), s->mss);
        bool fin = s->fin_wanted && !s->fin_sent && n == avail;
        if (!n && !fin) break;
        uint8_t tmp[TCP_MSS];
        for (uint32_t i = 0; i < n; i++) tmp[i] = s->txb[(s->tx_off + inflight + i) % TXSZ];
        seg(s, s->snd_nxt, (n ? PSH : 0) | (fin ? FIN : 0), tmp, n);
        s->snd_nxt += n + fin;
        arm(s);
        if (fin) {
            s->fin_sent = true;
            if (s->state == TCP_ESTAB) s->state = TCP_FIN_WAIT1;
            else if (s->state == TCP_CLOSE_WAIT) s->state = TCP_LAST_ACK;
            break;
        }
    }
}

int tcp_connect(struct sock *s) {
    s->rxb = kmalloc(RXSZ); s->txb = kmalloc(TXSZ);
    if (!s->rxb || !s->txb) return -ENOMEM;
    s->iss = (uint32_t)ticks_ms * 2654435761u + s->lport;
    s->snd_una = s->iss; s->snd_nxt = s->iss + 1;
    s->rto = 500; s->mss = 536;
    s->state = TCP_SYN_SENT;
    s->rto_at = ticks_ms + s->rto;
    seg(s, s->iss, SYN, 0, 0);
    while (s->state == TCP_SYN_SENT)
        if (nwait(s, 0) < 0) { s->state = TCP_CLOSED; return -EINTR; }
    return s->state == TCP_ESTAB ? 0 : -s->err;
}

int tcp_write(struct sock *s, const uint8_t *buf, uint32_t n) {
    uint32_t done = 0;
    while (done < n) {
        if (s->state != TCP_ESTAB && s->state != TCP_CLOSE_WAIT) return done ? (int)done : s->err ? -s->err : -EPIPE;
        uint32_t room = TXSZ - s->tx_len;
        if (!room) {
            if (nwait(s, 0) < 0) return done ? (int)done : -EINTR;
            continue;
        }
        uint32_t k = MIN(room, n - done);
        for (uint32_t i = 0; i < k; i++) s->txb[(s->tx_off + s->tx_len + i) % TXSZ] = buf[done + i];
        s->tx_len += k; done += k;
        tcp_output(s);
    }
    return done;
}

int tcp_read(struct sock *s, uint8_t *buf, uint32_t n) {
    while (s->rx_h == s->rx_t) {
        if (s->err) { int e = s->err; s->err = 0; return -e; }
        if (s->rcvd_fin || s->state == TCP_CLOSED) return 0;
        if (s->state == TCP_SYN_SENT || !s->rxb) return -ENOTCONN;
        if (nwait(s, 0) < 0) return -EINTR;
    }
    uint32_t was = rx_free(s), k = MIN(n, s->rx_h - s->rx_t);
    for (uint32_t i = 0; i < k; i++) buf[i] = s->rxb[(s->rx_t + i) % RXSZ];
    s->rx_t += k;
    if (was < s->mss && rx_free(s) >= s->mss) seg(s, s->snd_nxt, 0, 0, 0);   /* window update */
    return k;
}

void tcp_close(struct sock *s) {
    s->orphan = true;
    s->dead_at = ticks_ms + 30000;
    if (s->state == TCP_ESTAB || s->state == TCP_CLOSE_WAIT) { s->fin_wanted = true; tcp_output(s); }
    else if (s->state == TCP_SYN_SENT) s->state = TCP_CLOSED;
}

static void abort_conn(struct sock *s, int err) {
    s->state = TCP_CLOSED; s->err = err; s->rto_at = 0;
    wakeup(s);
}

void tcp_rx(uint32_t src, uint32_t dst, const uint8_t *p, uint32_t n) {
    const struct tcph *t = (const void *)p;
    if (n < sizeof *t) return;
    uint32_t hl = (t->off >> 4) * 4;
    if (hl < sizeof *t || hl > n) return;
    const uint8_t *d = p + hl;
    uint32_t dl = n - hl, seq = ntohl(t->seq), ack = ntohl(t->ack), wnd = ntohs(t->win);
    uint8_t fl = t->flags;
    struct sock *s;
    for (s = socks; s; s = s->next)
        if (s->type == SOCK_STREAM && s->state != TCP_CLOSED && s->lport == t->dport && s->rport == t->sport && s->rip == src) break;
    if (!s) {                                           /* nobody home: RST (RFC 793 p.65) */
        if (fl & RST) return;
        if (fl & ACK) seg_send(dst, src, t->dport, t->sport, ack, 0, RST, 0, 0, 0);
        else seg_send(dst, src, t->dport, t->sport, 0, seq + dl + !!(fl & (SYN | FIN)), RST | ACK, 0, 0, 0);
        return;
    }
    if (s->state == TCP_SYN_SENT) {
        if ((fl & ACK) && ack != s->iss + 1) { if (!(fl & RST)) seg_send(0, src, t->dport, t->sport, ack, 0, RST, 0, 0, 0); return; }
        if (fl & RST) { if (fl & ACK) abort_conn(s, ECONNREFUSED); return; }
        if ((fl & (SYN | ACK)) != (SYN | ACK)) return;
        s->rcv_nxt = seq + 1; s->snd_una = ack; s->snd_wnd = wnd;
        for (uint32_t i = sizeof *t; i + 3 < hl; ) {      /* MSS option */
            if (p[i] == 0) break;
            if (p[i] == 1) { i++; continue; }
            if (p[i] == 2 && p[i + 1] == 4) s->mss = MIN((uint32_t)(p[i + 2] << 8 | p[i + 3]), (uint32_t)TCP_MSS);
            if (p[i + 1] < 2) break;
            i += p[i + 1];
        }
        s->state = TCP_ESTAB; s->retries = 0; s->rto_at = 0;
        seg(s, s->snd_nxt, 0, 0, 0);
        wakeup(s);
        tcp_output(s);
        return;
    }
    if (fl & RST) {
        if (SEQ_LE(s->rcv_nxt, seq) && SEQ_LT(seq, s->rcv_nxt + RXSZ)) abort_conn(s, s->state == TCP_CLOSE_WAIT ? EPIPE : ECONNRESET);
        return;
    }
    if (fl & ACK) {
        if (SEQ_LE(s->snd_una, ack) && SEQ_LE(ack, s->snd_nxt)) {
            uint32_t nack = ack - s->snd_una;
            if (nack) {
                bool finack = s->fin_sent && ack == s->snd_nxt;
                uint32_t dk = nack - finack;
                s->tx_off = (s->tx_off + dk) % TXSZ; s->tx_len -= dk;
                s->snd_una = ack; s->retries = 0; s->rto = 500;
                s->rto_at = s->snd_una == s->snd_nxt ? 0 : ticks_ms + s->rto;
                if (finack) {
                    if (s->state == TCP_FIN_WAIT1) s->state = TCP_FIN_WAIT2;
                    else if (s->state == TCP_CLOSING) { s->state = TCP_TIME_WAIT; s->tw_at = ticks_ms + 2000; }
                    else if (s->state == TCP_LAST_ACK) { s->state = TCP_CLOSED; wakeup(s); }
                }
                wakeup(s);
            }
            s->snd_wnd = wnd;
        }
    }
    if (dl || (fl & FIN)) {
        int32_t off = (int32_t)(s->rcv_nxt - seq);
        if (off < 0 || off > (int32_t)dl || (off == (int32_t)dl && !(fl & FIN))) {   /* out of order / old: dup ACK */
            seg(s, s->snd_nxt, 0, 0, 0);
            return;
        }
        d += off; dl -= off;
        uint32_t k = MIN(dl, rx_free(s));
        for (uint32_t i = 0; i < k; i++) s->rxb[(s->rx_h + i) % RXSZ] = d[i];
        s->rx_h += k; s->rcv_nxt += k;
        if ((fl & FIN) && k == dl && !s->rcvd_fin) {
            s->rcv_nxt++; s->rcvd_fin = true;
            if (s->state == TCP_ESTAB) s->state = TCP_CLOSE_WAIT;
            else if (s->state == TCP_FIN_WAIT1) s->state = TCP_CLOSING;
            else if (s->state == TCP_FIN_WAIT2) { s->state = TCP_TIME_WAIT; s->tw_at = ticks_ms + 2000; }
        }
        seg(s, s->snd_nxt, 0, 0, 0);
        wakeup(s);
    }
    tcp_output(s);
}

void tcp_timer(void) {
    uint64_t now = ticks_ms;
    for (struct sock *s = socks, *nx; s; s = nx) {
        nx = s->next;
        if (s->type != SOCK_STREAM) continue;
        if (s->state == TCP_TIME_WAIT && now >= s->tw_at) { s->state = TCP_CLOSED; wakeup(s); }
        if (s->rto_at && now >= s->rto_at && s->state != TCP_CLOSED) {
            if (++s->retries > (s->state == TCP_SYN_SENT ? 4 : 7)) abort_conn(s, ETIMEDOUT);
            else {
                s->rto = MIN(s->rto * 2, 8000);
                s->rto_at = now + s->rto;
                if (s->state == TCP_SYN_SENT) seg(s, s->iss, SYN, 0, 0);
                else { s->fin_sent = false; s->snd_nxt = s->snd_una; tcp_output(s); }
            }
        }
        if (s->orphan && (s->state == TCP_CLOSED || now >= s->dead_at)) sock_free(s);
    }
}
