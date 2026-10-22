/* ac97.c - Intel ICH AC'97 (QEMU `-device AC97`, 8086:2415): /dev/dsp = 48 kHz stereo s16le playback, mixed in software.
 *
 * Every open of /dev/dsp is its own stream (the devfs lookup hands out a fresh vnode, see `clone`) with a
 * 64 KiB queue of its own; write() only fills that queue. PCM-out DMA walks a ring of NB descriptors of up
 * to MIXF frames each. The IRQ (IOC per descriptor) retires finished ones and tops the ring up to LEAD
 * descriptors in flight by summing what the streams have queued (per-stream gain, clamped to 16 bits), so a
 * stream adds latency only through its own queue (~16-21 ms of DMA lead, the same for everybody). With
 * nothing queued nothing is mixed, the engine halts at LVI and restarts when LVI moves; a stream that was
 * opened but never written owns no memory beyond its vnode and is skipped.
 * ponytail: linear gain steps, no resampling (48k only); mixing runs in the IRQ, fine at <= 16 streams. */
#include "net.h"

#define NB 32                   /* descriptors (the hardware index wraps at 32, so the list must be full length) */
#define MIXF 256                /* frames per descriptor (5.3 ms) */
#define LEAD 4                  /* descriptors kept in flight: the playing one + ~3 mixed ahead */
#define SQ 65536                /* per-stream queue, bytes (a multiple of 4) */
#define MAXS 16                 /* open streams */
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

static const struct vnode_ops ac97_stream_ops;

struct bde { uint32_t addr; uint16_t len; uint16_t flags; } PACKED;

struct stream {
    struct vnode vn;            /* first: the vnode is the stream */
    struct stream *next;
    uint8_t *q;                 /* SQ-byte ring, allocated on the first write */
    int head, n;                /* consumer position, bytes queued */
    int gain;                   /* 0..256, 256 = unity */
};

static uint16_t nam, nabm;
static struct bde *bdl;
static uint8_t *buf;
static uint16_t slot_len[NB];
static int rd, wr, nq, started, vol = 80, nstreams;
static uint32_t rseq, wseq;                         /* descriptors retired / enqueued since boot (wrapping) */
static struct stream *streams;
static char chan;

/* retire descriptors the engine has finished; irqs must be off */
static void reap(void) {
    uint16_t sr = inw(nabm + PO_SR);
    if (started && (sr & SR_DCH)) { rseq += nq; rd = wr; nq = 0; return; }   /* halted at LVI: all done */
    int civ = inb(nabm + PO_CIV) % NB;
    while (nq && rd != civ) { rd = (rd + 1) % NB; nq--; rseq++; }
}

/* sum up to `frames` frames from every stream with data into out; irqs must be off */
static void mix(int16_t *out, int frames) {
    static int32_t acc[MIXF * 2];
    memset(acc, 0, frames * 8);
    for (struct stream *s = streams; s; s = s->next) {
        if (!s->n) continue;
        int take = MIN(frames * 4, s->n), done = 0;
        while (done < take) {
            int c = MIN(take - done, SQ - s->head);   /* up to the wrap point */
            const int16_t *src = (const int16_t *)(s->q + s->head);
            int32_t *a = acc + done / 2;
            if (s->gain == 256) for (int i = 0; i < c / 2; i++) a[i] += src[i];
            else for (int i = 0; i < c / 2; i++) a[i] += src[i] * s->gain >> 8;
            s->head = (s->head + c) % SQ;
            done += c;
        }
        s->n -= take;
    }
    for (int i = 0; i < frames * 2; i++) out[i] = acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i];
}

/* keep LEAD descriptors in flight while any stream has data; irqs must be off */
static void refill(void) {
    reap();
    while (nq < LEAD) {
        int avail = 0;
        for (struct stream *s = streams; s; s = s->next) avail = MAX(avail, s->n);
        if (!avail) return;
        int frames = MIN(avail / 4, MIXF);
        mix((int16_t *)(buf + wr * MIXF * 4), frames);
        slot_len[wr] = frames * 4;
        bdl[wr].len = frames * 2;                   /* in 16-bit samples */
        int last = wr;
        wr = (wr + 1) % NB; nq++; wseq++;
        outb(nabm + PO_LVI, last);
        if (!started) { started = 1; outb(nabm + PO_CR, CR_RUN | CR_IOCE | CR_LVBIE); }
    }
}

static void ac97_irq(struct regs *r) {
    uint16_t sr = inw(nabm + PO_SR);
    if (!(sr & 0x1C)) return;                       /* LVBCI | BCIS | FIFOE */
    outw(nabm + PO_SR, sr & 0x1C);
    refill();
    wakeup(&chan);
}

static void set_vol(int v) {
    vol = v < 0 ? 0 : v > 100 ? 100 : v;
    int att = (100 - vol) * 31 / 100;               /* 1.5 dB steps, 0 = loudest */
    outw(nam + 0x02, vol ? att << 8 | att : 0x8000);
    outw(nam + 0x18, 0x0808);                       /* PCM out 0 dB */
}

int ac97_volume(int set) {
    if (!nam) return -1;
    if (set >= 0) set_vol(set);
    return vol;
}

/* bytes between a write() and the speaker for stream s: its own queue + what the DMA ring holds */
static int queued(struct stream *s) {
    uint32_t f = irq_save();
    reap();
    int b = s->n;
    for (int i = 0, k = rd; i < nq; i++, k = (k + 1) % NB) b += slot_len[k];
    irq_restore(f);
    return b;
}

static int dsp_write(struct vnode *v, uint32_t off, const void *src, uint32_t n) {
    struct stream *s = (struct stream *)v;
    n &= ~3u;                                       /* whole stereo frames */
    if (n && !s->q && !(s->q = kmalloc(SQ))) return -ENOMEM;
    uint32_t done = 0;
    while (done < n) {
        uint32_t f = irq_save();
        if (s->n == SQ) {
            refill();
            int r = sleep_on(&chan, 100);
            irq_restore(f);
            if (r < 0) return done ? (int)done : -EINTR;
            continue;
        }
        int c = MIN(MIN(n - done, (uint32_t)(SQ - s->n)), 4096u), t = (s->head + s->n) % SQ;   /* <= 4 KiB per irq-off stretch */
        int c1 = MIN(c, SQ - t);
        memcpy(s->q + t, (const uint8_t *)src + done, c1);
        memcpy(s->q, (const uint8_t *)src + done + c1, c - c1);
        s->n += c; done += c;
        refill();
        irq_restore(f);
    }
    return done;
}

static int dsp_ioctl(struct vnode *v, int req, void *arg) {
    struct stream *s = (struct stream *)v;
    int *p = arg;
    switch (req) {
    case AUDIO_GETQ: *p = queued(s); return 0;
    case AUDIO_SETVOL: set_vol(*p); return 0;
    case AUDIO_GETVOL: *p = vol; return 0;
    case AUDIO_SETSVOL: s->gain = (*p < 0 ? 0 : *p > 100 ? 100 : *p) * 256 / 100; return 0;
    case AUDIO_GETSVOL: *p = (s->gain * 100 + 128) / 256; return 0;
    case AUDIO_FLUSH: {
        uint32_t f = irq_save();
        s->head = s->n = 0;
        irq_restore(f);
        wakeup(&chan);                              /* a writer blocked on a full queue */
        return 0;
    }
    }
    return -ENOTTY;
}

static int dsp_poll(struct vnode *v, int ev) {
    struct stream *s = (struct stream *)v;
    return SQ - s->n >= 4096 ? ev & POLLOUT : 0;
}

/* drain: wait until this stream's queue is empty and the descriptors that carry its tail have played
 * (other streams keep going), max 5 s */
static void dsp_close(struct vnode *v, int flags) {
    struct stream *s = (struct stream *)v;
    if ((flags & O_ACCMODE) == O_RDONLY) return;
    uint32_t last = 0, until = ticks_ms + 5000;
    int have = 0;
    while ((int32_t)(ticks_ms - until) < 0) {
        uint32_t f = irq_save();
        refill();
        if (!s->n && !have) { last = wseq; have = 1; }
        if (have && (int32_t)(rseq - last) >= 0) { irq_restore(f); return; }
        int r = sleep_on(&chan, 50);
        irq_restore(f);
        if (r < 0) return;
    }
}

static void dsp_release(struct vnode *v) {
    struct stream *s = (struct stream *)v;
    uint32_t f = irq_save();
    for (struct stream **p = &streams; *p; p = &(*p)->next)
        if (*p == s) { *p = s->next; break; }
    nstreams--;
    irq_restore(f);
    kfree(s->q);
    kfree(s);
}

/* a lookup of /dev/dsp: a new stream */
static int dsp_clone(struct vnode *tmpl, struct vnode **out) {
    struct stream *s = kzalloc(sizeof *s);
    if (!s) return -ENOMEM;
    s->vn = *tmpl;
    s->vn.refs = 1; s->vn.ops = &ac97_stream_ops;
    s->gain = 256;
    uint32_t f = irq_save();
    if (nstreams >= MAXS) { irq_restore(f); kfree(s); return -EBUSY; }
    nstreams++;
    s->next = streams; streams = s;
    irq_restore(f);
    *out = &s->vn;
    return 0;
}

static int mixer_ioctl(struct vnode *v, int req, void *arg) {
    int *p = arg;
    switch (req) {
    case AUDIO_SETVOL: set_vol(*p); return 0;
    case AUDIO_GETVOL: *p = vol; return 0;
    }
    return -ENOTTY;
}

const struct vnode_ops ac97_ops = { .ioctl = mixer_ioctl };
static const struct vnode_ops ac97_stream_ops = { .write = dsp_write, .ioctl = dsp_ioctl, .poll = dsp_poll, .close = dsp_close, .release = dsp_release };
const struct vnode_ops ac97_dsp_ops = { .clone = dsp_clone };

int ac97_init(void) {
    struct pci_dev d;
    if (pci_find(0x8086, 0x2415, &d) < 0) return -ENODEV;
    pci_write(d.bus, d.dev, d.fn, 4, pci_read(d.bus, d.dev, d.fn, 4) | 5);    /* io + bus master */
    nam = d.bar[0] & 0xFFFC; nabm = d.bar[1] & 0xFFFC;
    uint32_t pg = pmm_alloc(), b = pmm_alloc_contig(NB * MIXF * 4 / PAGE_SIZE);
    if (!pg || !b) return -ENOMEM;
    bdl = (struct bde *)pg; buf = (uint8_t *)b;
    for (int i = 0; i < NB; i++) { bdl[i].addr = b + i * MIXF * 4; bdl[i].flags = 0x8000; }   /* IOC */

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
