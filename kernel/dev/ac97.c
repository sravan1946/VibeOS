/* ac97.c - Intel ICH AC'97 (QEMU `-device AC97`, 8086:2415): /dev/dsp = 48 kHz stereo s16le playback.
 *
 * PCM-out DMA walks a 32-entry buffer descriptor list. Every write() chunk (<= SLOT bytes) becomes one
 * descriptor with its exact length, so there are no partial buffers to pad; the IRQ (IOC per descriptor)
 * retires finished ones and wakes blocked writers. The engine is started once and then only LVI moves.
 * ponytail: tiny writes burn a whole descriptor each (32 in flight); batch >= ~1 KiB. No VRA, 48k only. */
#include "net.h"

#define NB 32
#define SLOT 4096
/* NABM (bus master) PCM-out registers */
#define PO_BDBAR 0x10
#define PO_CIV   0x14
#define PO_LVI   0x15
#define PO_SR    0x16
#define PO_CR    0x1B
#define GLOB_CNT 0x2C
#define GLOB_STA 0x30
#define CR_RUN 1
#define CR_RESET 2
#define CR_LVBIE 4
#define CR_IOCE 16
#define SR_DCH 1

struct bde { uint32_t addr; uint16_t len; uint16_t flags; } PACKED;

static uint16_t nam, nabm;
static struct bde *bdl;
static uint8_t *buf;
static uint16_t slot_len[NB];
static int rd, wr, nq, started, vol = 80;
static char chan;

/* retire descriptors the engine has finished; irqs must be off */
static void reap(void) {
    uint16_t sr = inw(nabm + PO_SR);
    if (started && (sr & SR_DCH)) { rd = wr; nq = 0; return; }   /* halted at LVI: all done */
    int civ = inb(nabm + PO_CIV) % NB;
    while (nq && rd != civ) { rd = (rd + 1) % NB; nq--; }
}

static void ac97_irq(struct regs *r) {
    uint16_t sr = inw(nabm + PO_SR);
    if (!(sr & 0x1C)) return;                       /* LVBCI | BCIS | FIFOE */
    outw(nabm + PO_SR, sr & 0x1C);
    reap();
    wakeup(&chan);
}

static void set_vol(int v) {
    vol = v < 0 ? 0 : v > 100 ? 100 : v;
    int att = (100 - vol) * 31 / 100;               /* 1.5 dB steps, 0 = loudest */
    outw(nam + 0x02, vol ? att << 8 | att : 0x8000);
    outw(nam + 0x18, 0x0808);                       /* PCM out 0 dB */
}

static int queued(void) {
    uint32_t f = irq_save();
    reap();
    int b = 0;
    for (int i = 0, s = rd; i < nq; i++, s = (s + 1) % NB) b += slot_len[s];
    irq_restore(f);
    return b;
}

static int dsp_write(struct vnode *v, uint32_t off, const void *src, uint32_t n) {
    n &= ~3u;                                       /* whole stereo frames */
    uint32_t done = 0;
    while (done < n) {
        uint32_t f = irq_save();
        reap();
        if (nq == NB) {
            int r = sleep_on(&chan, 100);
            irq_restore(f);
            if (r < 0) return done ? (int)done : -EINTR;
            continue;
        }
        uint32_t c = MIN(n - done, (uint32_t)SLOT);
        memcpy(buf + wr * SLOT, (const uint8_t *)src + done, c);
        slot_len[wr] = c;
        bdl[wr].len = c / 2;                        /* in 16-bit samples */
        int last = wr;
        wr = (wr + 1) % NB; nq++; done += c;
        outb(nabm + PO_LVI, last);
        if (!started) { started = 1; outb(nabm + PO_CR, CR_RUN | CR_IOCE | CR_LVBIE); }
        irq_restore(f);
    }
    return done;
}

static int dsp_ioctl(struct vnode *v, int req, void *arg) {
    int *p = arg;
    switch (req) {
    case AUDIO_GETQ: *p = queued(); return 0;
    case AUDIO_SETVOL: set_vol(*p); return 0;
    case AUDIO_GETVOL: *p = vol; return 0;
    }
    return -ENOTTY;
}

static int dsp_poll(struct vnode *v, int ev) {
    uint32_t f = irq_save();
    reap();
    int r = nq < NB ? POLLOUT : 0;
    irq_restore(f);
    return r & ev;
}

static void dsp_close(struct vnode *v, int flags) {  /* drain: wait for the ring to empty (max 5 s) */
    if ((flags & O_ACCMODE) == O_RDONLY) return;
    for (int i = 0; i < 100; i++) {
        uint32_t f = irq_save();
        reap();
        if (!nq) { irq_restore(f); return; }
        int r = sleep_on(&chan, 50);
        irq_restore(f);
        if (r < 0) return;
    }
}

const struct vnode_ops ac97_ops = { .write = dsp_write, .ioctl = dsp_ioctl, .poll = dsp_poll, .close = dsp_close };

int ac97_init(void) {
    struct pci_dev d;
    if (pci_find(0x8086, 0x2415, &d) < 0) return -ENODEV;
    pci_write(d.bus, d.dev, d.fn, 4, pci_read(d.bus, d.dev, d.fn, 4) | 5);    /* io + bus master */
    nam = d.bar[0] & 0xFFFC; nabm = d.bar[1] & 0xFFFC;
    uint32_t pg = pmm_alloc(), b = pmm_alloc_contig(NB * SLOT / PAGE_SIZE);
    if (!pg || !b) return -ENOMEM;
    bdl = (struct bde *)pg; buf = (uint8_t *)b;
    for (int i = 0; i < NB; i++) { bdl[i].addr = b + i * SLOT; bdl[i].flags = 0x8000; }   /* IOC */

    outl(nabm + GLOB_CNT, 2);                       /* cold reset off, irqs still off */
    for (int i = 0; i < 100 && !(inl(nabm + GLOB_STA) & 0x100); i++) sleep_ms(2);   /* codec ready */
    outw(nam, 0);                                   /* codec reset */
    for (int i = 0; i < 100 && (inw(nam + 0x26) & 0xF) != 0xF; i++) sleep_ms(2);
    outb(nabm + PO_CR, CR_RESET);
    for (int i = 0; i < 100 && (inb(nabm + PO_CR) & CR_RESET); i++) sleep_ms(1);
    outl(nabm + PO_BDBAR, pg);
    set_vol(vol);
    irq_register(d.irq, ac97_irq);
    kprintf("ac97: irq %d, 48 kHz stereo\n", d.irq);
    return 0;
}
