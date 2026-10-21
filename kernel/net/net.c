/* net.c - Ethernet, ARP, IPv4, ICMP, UDP, DHCP and the net thread.
 * The kernel doesn't preempt kernel code, so stack state needs no locks; the only IRQ work is
 * waking net_thread, which does all RX processing. Code running on that thread must never sleep. */
#include "net.h"

struct netinfo ni;
char net_chan, cfg_chan, arp_chan;
struct task *net_task;
static bool dhcp_failed;

struct eth  { uint8_t dst[6], src[6]; uint16_t type; } PACKED;
struct arp  { uint16_t hw, proto; uint8_t hl, pl; uint16_t op; uint8_t sha[6]; uint32_t spa; uint8_t tha[6]; uint32_t tpa; } PACKED;
struct iph  { uint8_t vihl, tos; uint16_t len, id, frag; uint8_t ttl, proto; uint16_t csum; uint32_t src, dst; } PACKED;
struct icmp { uint8_t type, code; uint16_t csum, id, seq; } PACKED;
struct udph { uint16_t sport, dport, len, csum; } PACKED;

#define ETH_IP  0x0008      /* htons constants, spelled out for static init */
#define ETH_ARP 0x0608
static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };

int nwait(void *chan, uint32_t ms) {
    uint32_t f = irq_save();
    int r = sleep_on(chan, ms);
    irq_restore(f);
    return r;
}

/* ---- checksums ---- */
static uint32_t csum_add(uint32_t s, const void *p, uint32_t n) {
    const uint8_t *b = p;
    for (; n > 1; n -= 2, b += 2) s += b[0] << 8 | b[1];
    if (n) s += b[0] << 8;
    return s;
}
static uint16_t csum_fin(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return htons((uint16_t)~s);
}
uint16_t ip_csum(const void *p, uint32_t n) { return csum_fin(csum_add(0, p, n)); }
uint16_t l4_csum(uint8_t proto, uint32_t src, uint32_t dst, const void *seg, uint32_t n) {
    uint32_t s = csum_add(0, &src, 4);
    s = csum_add(s, &dst, 4);
    s += proto + n;
    return csum_fin(csum_add(s, seg, n));
}

int ip_parse(const char *s, uint32_t *ip) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + *s++ - '0'; if (v > 255) return -1; }
        b[i] = v;
        if (i < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    memcpy(ip, b, 4);
    return 0;
}

/* ---- ARP ---- */
static struct { uint32_t ip; uint8_t mac[6]; uint32_t seq; } arpc[8];
static uint32_t arp_seq;

static bool arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < 8; i++)
        if (arpc[i].ip == ip && ip) { memcpy(mac, arpc[i].mac, 6); return true; }
    return false;
}

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    int v = 0;
    for (int i = 0; i < 8; i++) {
        if (arpc[i].ip == ip) { v = i; break; }
        if (arpc[i].seq < arpc[v].seq) v = i;       /* oldest */
    }
    arpc[v].ip = ip; memcpy(arpc[v].mac, mac, 6); arpc[v].seq = ++arp_seq;
    wakeup(&arp_chan);
}

static void eth_send(const uint8_t *dst, uint16_t type, const void *p, uint32_t n) {
    uint8_t f[1520];
    struct eth *e = (void *)f;
    memcpy(e->dst, dst, 6); memcpy(e->src, nic_mac, 6); e->type = type;
    memcpy(f + sizeof *e, p, n);
    e1000_send(f, sizeof *e + n);
    ni.tx_pkts++; ni.tx_bytes += sizeof *e + n;
}

static void arp_send(uint16_t op, const uint8_t *dmac, uint32_t tip) {
    struct arp a = { htons(1), ETH_IP, 6, 4, htons(op) };
    memcpy(a.sha, nic_mac, 6); a.spa = ni.ip;
    if (op == 2) memcpy(a.tha, dmac, 6);
    a.tpa = tip;
    eth_send(op == 1 ? bcast : dmac, ETH_ARP, &a, sizeof a);
}

static int arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_lookup(ip, mac)) return 0;
    for (int i = 0; i < 4; i++) {
        arp_send(1, 0, ip);
        if (current == net_task) return -EHOSTUNREACH;   /* can't sleep here: the caller retries */
        nwait(&arp_chan, 250);
        if (arp_lookup(ip, mac)) return 0;
    }
    return -EHOSTUNREACH;
}

static void arp_rx(const uint8_t *p, uint32_t n) {
    const struct arp *a = (const void *)p;
    if (n < sizeof *a || a->hw != htons(1) || a->proto != ETH_IP || a->tpa != ni.ip || !ni.ip) return;
    arp_learn(a->spa, a->sha);
    if (a->op == htons(1)) arp_send(2, a->sha, a->spa);
}

/* ---- IPv4 ---- */
int ip_send(uint8_t proto, uint32_t src, uint32_t dst, const void *p, uint32_t n) {
    static uint16_t id;
    uint8_t mac[6];
    if (n > 1480) return -EMSGSIZE;
    if (dst == 0xFFFFFFFF || (ni.mask && dst == (ni.ip | ~ni.mask))) memcpy(mac, bcast, 6);
    else {
        uint32_t nh = ((dst ^ ni.ip) & ni.mask) == 0 ? dst : ni.gw;
        if (!nh) return -ENETDOWN;
        int r = arp_resolve(nh, mac);                   /* may sleep: build the packet afterwards */
        if (r < 0) return r;
    }
    uint8_t pkt[1500];
    struct iph *h = (void *)pkt;
    *h = (struct iph){ 0x45, 0, htons(sizeof *h + n), htons(++id), 0, 64, proto, 0, src ? src : ni.ip, dst };
    h->csum = ip_csum(h, sizeof *h);
    memcpy(pkt + sizeof *h, p, n);
    eth_send(mac, ETH_IP, pkt, sizeof *h + n);
    return 0;
}

static void icmp_rx(uint32_t src, const uint8_t *p, uint32_t n) {
    const struct icmp *ic = (const void *)p;
    if (n < sizeof *ic) return;
    if (ic->type == 8 && ic->code == 0) {               /* echo request -> reply */
        uint8_t r[1480];
        memcpy(r, p, n);
        ((struct icmp *)r)->type = 0;
        ((struct icmp *)r)->csum = 0;
        ((struct icmp *)r)->csum = ip_csum(r, n);
        ip_send(1, 0, src, r, n);
    } else if (ic->type == 0 || ic->type == 3 || ic->type == 11) {
        sock_raw_rx(src, p, n);
    }
}

/* ---- UDP ---- */
int udp_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *p, uint32_t n) {
    uint8_t d[1480];
    if (n > 1472) return -EMSGSIZE;
    struct udph *u = (void *)d;
    *u = (struct udph){ sport, dport, htons(sizeof *u + n), 0 };
    memcpy(d + sizeof *u, p, n);
    u->csum = l4_csum(17, src ? src : ni.ip, dst, d, sizeof *u + n);
    if (!u->csum) u->csum = 0xFFFF;
    return ip_send(17, src, dst, d, sizeof *u + n);
}

/* ---- DHCP (boot-time client; no lease renewal) ---- */
static int dhcp_state;                 /* 0 done/idle, 1 discover sent, 2 request sent */
static uint32_t dhcp_xid, dhcp_offer, dhcp_srv;
static uint64_t dhcp_t;
static int dhcp_tries;

static void dhcp_send(int type) {
    uint8_t p[300];
    memset(p, 0, sizeof p);
    p[0] = 1; p[1] = 1; p[2] = 6;
    *(uint32_t *)(p + 4) = dhcp_xid;
    *(uint16_t *)(p + 10) = htons(0x8000);              /* ask for a broadcast reply: we have no IP yet */
    memcpy(p + 28, nic_mac, 6);
    p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99;
    uint8_t *o = p + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == 3) {
        *o++ = 50; *o++ = 4; memcpy(o, &dhcp_offer, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &dhcp_srv, 4); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 255;
    udp_send(0, htons(68), 0xFFFFFFFF, htons(67), p, sizeof p);
    dhcp_t = ticks_ms;
}

static void dhcp_start(void) {
    dhcp_xid = (uint32_t)ticks_ms * 2654435761u ^ (nic_mac[4] << 8 | nic_mac[5]);
    dhcp_state = 1; dhcp_tries = 0;
    dhcp_send(1);
}

static void dhcp_rx(const uint8_t *p, uint32_t n) {
    if (n < 244 || p[0] != 2 || *(const uint32_t *)(p + 4) != dhcp_xid || !dhcp_state) return;
    if (memcmp(p + 28, nic_mac, 6) || p[236] != 99) return;
    uint32_t mask = 0, gw = 0, dns = 0, srv = 0;
    int type = 0;
    for (uint32_t i = 240; i + 1 < n && p[i] != 255; ) {
        if (p[i] == 0) { i++; continue; }
        uint8_t code = p[i], l = p[i + 1];
        if (i + 2 + l > n) break;
        const uint8_t *v = p + i + 2;
        if (code == 53) type = v[0];
        else if (code == 1 && l >= 4) memcpy(&mask, v, 4);
        else if (code == 3 && l >= 4) memcpy(&gw, v, 4);
        else if (code == 6 && l >= 4) memcpy(&dns, v, 4);
        else if (code == 54 && l >= 4) memcpy(&srv, v, 4);
        i += 2 + l;
    }
    if (type == 2 && dhcp_state == 1) {
        memcpy(&dhcp_offer, p + 16, 4);
        dhcp_srv = srv;
        dhcp_state = 2; dhcp_tries = 0;
        dhcp_send(3);
    } else if (type == 5 && dhcp_state == 2) {
        memcpy(&ni.ip, p + 16, 4);
        ni.mask = mask ? mask : IP4(255, 255, 255, 0);
        ni.gw = gw; ni.dns = dns ? dns : gw;
        ni.up = 1; dhcp_state = 0;
        kprintf("net: e1000 %02x:%02x:%02x:%02x:%02x:%02x %u.%u.%u.%u/%d gw %u.%u.%u.%u\n",
                nic_mac[0], nic_mac[1], nic_mac[2], nic_mac[3], nic_mac[4], nic_mac[5],
                ni.ip & 255, ni.ip >> 8 & 255, ni.ip >> 16 & 255, ni.ip >> 24,
                __builtin_popcount(ni.mask), ni.gw & 255, ni.gw >> 8 & 255, ni.gw >> 16 & 255, ni.gw >> 24);
        wakeup(&cfg_chan);
    } else if (type == 6) {                              /* NAK: start over */
        dhcp_start();
    }
}

static void dhcp_timer(void) {
    if (!dhcp_state || ticks_ms - dhcp_t < 2000) return;
    if (++dhcp_tries > 4) {
        dhcp_state = 0; dhcp_failed = true;
        kprintf("net: e1000 %02x:%02x:%02x:%02x:%02x:%02x no DHCP lease\n",
                nic_mac[0], nic_mac[1], nic_mac[2], nic_mac[3], nic_mac[4], nic_mac[5]);
        wakeup(&cfg_chan);
        return;
    }
    dhcp_send(dhcp_state == 1 ? 1 : 3);
}

int net_wait_cfg(void) {
    if (!ni.present) return -ENODEV;
    uint64_t end = ticks_ms + 12000;
    while (!ni.up) {
        if (dhcp_failed || ticks_ms >= end) return -ENETDOWN;
        if (nwait(&cfg_chan, end - ticks_ms) < 0) return -EINTR;
    }
    return 0;
}

/* ---- receive path ---- */
static void ip_rx(const uint8_t *p, uint32_t n) {
    const struct iph *h = (const void *)p;
    if (n < sizeof *h || h->vihl >> 4 != 4) return;
    uint32_t hl = (h->vihl & 15) * 4, tl = ntohs(h->len);
    if (hl < sizeof *h || tl < hl || tl > n) return;
    if (h->frag & htons(0x3FFF)) return;                 /* fragments: not reassembled */
    bool mine = h->dst == ni.ip || h->dst == 0xFFFFFFFF || (ni.mask && h->dst == (ni.ip | ~ni.mask)) || !ni.ip;
    if (!mine) return;
    p += hl; tl -= hl;
    switch (h->proto) {
    case 1:  icmp_rx(h->src, p, tl); break;
    case 6:  tcp_rx(h->src, h->dst, p, tl); break;
    case 17: {
        const struct udph *u = (const void *)p;
        if (tl < sizeof *u) return;
        uint32_t ul = MIN((uint32_t)ntohs(u->len), tl);
        if (ul < sizeof *u) return;
        if (u->dport == htons(68)) dhcp_rx(p + sizeof *u, ul - sizeof *u);
        else sock_udp_rx(h->src, u->sport, u->dport, p + sizeof *u, ul - sizeof *u);
        break;
    }
    }
}

static void net_rx(const uint8_t *f, uint32_t n) {
    if (n < sizeof(struct eth)) return;
    ni.rx_pkts++; ni.rx_bytes += n;
    const struct eth *e = (const void *)f;
    if (e->type == ETH_ARP) arp_rx(f + sizeof *e, n - sizeof *e);
    else if (e->type == ETH_IP) ip_rx(f + sizeof *e, n - sizeof *e);
}

static int net_thread(void *arg) {
    dhcp_start();
    for (;;) {
        uint32_t f = irq_save();
        uint8_t *fr; uint32_t n;
        if (!e1000_rx_peek(&fr, &n)) sleep_on(&net_chan, 50);
        irq_restore(f);
        while (e1000_rx_peek(&fr, &n)) { net_rx(fr, n); e1000_rx_pop(); }
        tcp_timer();
        dhcp_timer();
    }
    return 0;
}

void net_init(void) {
    pci_init();
    if (e1000_init() < 0) return;                        /* no NIC: sockets return -ENODEV */
    memcpy(ni.mac, nic_mac, 6);
    ni.present = 1;
    net_task = kthread_create("net", net_thread, 0);
}
