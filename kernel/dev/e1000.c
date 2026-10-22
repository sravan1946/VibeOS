/* e1000.c - Intel 82540EM (QEMU's default NIC). RX is drained by the net thread; the IRQ only wakes it. */
#include "net.h"

#define R(o) (*(volatile uint32_t *)(mmio + (o)))
#define NRX 128
#define NTX 16
#define BUF 2048
struct rxd { uint64_t addr; uint16_t len, csum; uint8_t status, err; uint16_t special; } PACKED;
struct txd { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } PACKED;

static uint8_t *mmio, *rxbuf, *txbuf;
static struct rxd *rx;
static struct txd *tx;
static uint32_t rx_cur, tx_cur;
uint8_t nic_mac[6];

static void e1000_irq(struct regs *r) {
    if (R(0xC0)) wakeup(&net_chan);              /* reading ICR acks the interrupt */
}

int e1000_init(void) {
    struct pci_dev d;
    if (pci_find(0x8086, 0x100E, &d) < 0) return -ENODEV;
    pci_write(d.bus, d.dev, d.fn, 4, pci_read(d.bus, d.dev, d.fn, 4) | 7);   /* io + mem + bus master */
    uint32_t bar = d.bar[0] & ~0xFu;
    vmm_map_mmio(bar, 0x20000);
    mmio = (uint8_t *)bar;

    R(0xD8) = ~0u;                               /* mask everything, then reset */
    R(0) |= 1u << 26;
    for (int i = 0; i < 100 && (R(0) & (1u << 26)); i++) sleep_ms(1);
    R(0xD8) = ~0u;
    R(0) = (R(0) | 0x40 | 0x20) & ~((1u << 31) | (1u << 30));   /* SLU, ASDE; no PHY reset */

    uint32_t lo = R(0x5400), hi = R(0x5404);
    for (int i = 0; i < 4; i++) nic_mac[i] = lo >> (i * 8);
    nic_mac[4] = hi; nic_mac[5] = hi >> 8;

    uint32_t pg = pmm_alloc_contig(2);
    uint32_t rb = pmm_alloc_contig(NRX * BUF / PAGE_SIZE), tb = pmm_alloc_contig(NTX * BUF / PAGE_SIZE);
    if (!pg || !rb || !tb) return -ENOMEM;
    memset((void *)pg, 0, 2 * PAGE_SIZE);
    rx = (struct rxd *)pg; tx = (struct txd *)(pg + PAGE_SIZE);
    rxbuf = (uint8_t *)rb; txbuf = (uint8_t *)tb;
    for (int i = 0; i < 128; i++) R(0x5200 + i * 4) = 0;         /* multicast table */

    for (int i = 0; i < NRX; i++) rx[i].addr = rb + i * BUF;
    R(0x2800) = pg; R(0x2804) = 0; R(0x2808) = NRX * 16; R(0x2810) = 0; R(0x2818) = NRX - 1;
    R(0x100) = (1u << 1) | (1u << 15) | (1u << 26);              /* EN, accept broadcast, strip CRC; 2 KiB bufs */
    for (int i = 0; i < NTX; i++) tx[i].status = 1;              /* DD: free */
    R(0x3800) = pg + PAGE_SIZE; R(0x3804) = 0; R(0x3808) = NTX * 16; R(0x3810) = 0; R(0x3818) = 0;
    R(0x410) = 0x0060200A;
    R(0x400) = (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12);   /* EN, pad short, CT, COLD */

    irq_register(d.irq, e1000_irq);
    (void)R(0xC0);
    R(0xD0) = (1u << 2) | (1u << 4) | (1u << 6) | (1u << 7);     /* LSC, RXDMT0, RXO, RXT0 */
    return 0;
}

void e1000_send(const void *f, uint32_t len) {
    volatile struct txd *d = &tx[tx_cur];
    if (len > BUF) return;
    for (int i = 0; !(d->status & 1); i++)
        if (i > 1000000) return;                                 /* ring stuck: drop */
    uint8_t *b = txbuf + tx_cur * BUF;
    memcpy(b, f, len);
    d->addr = (uint32_t)b;
    d->len = MAX(len, 60);
    d->cmd = 0x0B;                                               /* EOP | IFCS | RS */
    d->status = 0;
    tx_cur = (tx_cur + 1) % NTX;
    __asm__ volatile("" ::: "memory");
    R(0x3818) = tx_cur;
}

bool e1000_rx_peek(uint8_t **f, uint32_t *len) {
    volatile struct rxd *d = &rx[rx_cur];
    if (!mmio || !(d->status & 1)) return false;
    *f = rxbuf + rx_cur * BUF;
    *len = (d->status & 2) && !d->err ? d->len : 0;              /* EOP and no errors, else a 0-length drop */
    return true;
}

void e1000_rx_pop(void) {
    rx[rx_cur].status = 0;
    __asm__ volatile("" ::: "memory");
    R(0x2818) = rx_cur;
    rx_cur = (rx_cur + 1) % NRX;
}
