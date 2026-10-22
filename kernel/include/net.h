/* net.h - PCI, e1000 and the network stack. IPs and ports are kept in network byte order. */
#pragma once
#include "proc.h"

#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)
#define IP4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define TCP_MSS 1460

/* dev/pci.c */
struct pci_dev { uint8_t bus, dev, fn, irq; uint16_t vendor, device; uint32_t cls, bar[6]; };
void     pci_init(void);
int      pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);
uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);

/* dev/e1000.c */
int  e1000_init(void);                                   /* 0 or -ENODEV */
void e1000_send(const void *frame, uint32_t len);
bool e1000_rx_peek(uint8_t **frame, uint32_t *len);      /* next received frame, valid until e1000_rx_pop */
void e1000_rx_pop(void);
extern uint8_t nic_mac[6];

/* net/net.c */
extern struct netinfo ni;
extern char net_chan;                                    /* wakes the net thread */
extern struct task *net_task;
void     net_init(void);
int      net_wait_cfg(void);                             /* wait for the DHCP lease: 0 / -ENODEV / -ENETDOWN */
int      nwait(void *chan, uint32_t ms);                 /* sleep_on with irqs masked */
int      ip_send(uint8_t proto, uint32_t src, uint32_t dst, const void *p, uint32_t n);
int      udp_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *p, uint32_t n);
uint16_t l4_csum(uint8_t proto, uint32_t src, uint32_t dst, const void *seg, uint32_t n);
uint16_t ip_csum(const void *p, uint32_t n);
int      ip_parse(const char *s, uint32_t *ip);

/* net/tcp.c + net/sock.c */
enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTAB, TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_CLOSING,
       TCP_TIME_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_LISTEN, TCP_SYN_RCVD };
#define RXSZ 32768
#define TXSZ 16384

struct dgram { struct dgram *next; uint32_t ip; uint16_t port, len; uint8_t data[]; };
struct sock {
    struct sock *next;
    int type;                                  /* SOCK_* */
    uint16_t lport, rport;
    uint32_t rip;
    bool connected, orphan;
    struct sock *parent;                       /* listener, until accept() takes it */
    int backlog;                               /* listener: max half-open + unaccepted children */
    struct dgram *qh, *qt;                     /* UDP / raw receive queue */
    int qn;
    /* TCP */
    int state, err, retries;
    uint32_t iss, snd_una, snd_nxt, rcv_nxt, snd_wnd, mss;
    uint8_t *rxb, *txb;
    uint32_t rx_h, rx_t;                       /* free-running counters */
    uint32_t tx_off, tx_len;                   /* ring start (= snd_una) and buffered bytes */
    bool fin_wanted, fin_sent, rcvd_fin;
    uint64_t rto_at, rto, tw_at, dead_at;
};
extern struct sock *socks;
void tcp_rx(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t n);
void tcp_timer(void);
void tcp_output(struct sock *s);
int  tcp_connect(struct sock *s);
int  tcp_write(struct sock *s, const uint8_t *buf, uint32_t n);
int  tcp_read(struct sock *s, uint8_t *buf, uint32_t n);
void tcp_close(struct sock *s);
struct sock *sock_new(int type);
void sock_free(struct sock *s);
void sock_udp_rx(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *p, uint32_t n);
void sock_raw_rx(uint32_t src, const uint8_t *p, uint32_t n);
int  net_syscall(int num, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
