/* ata.c - ATA PIO disk driver: polling, no IRQs (nIEN), LBA28 */
#include "vfs.h"

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80
#define TIMEOUT_MS 10000   /* a host fsync behind FLUSH CACHE can take a while */
#define SPIN   50000000     /* poll cap for when the timer isn't running */
#define MAXRUN 128          /* sectors per command */

struct ata { uint16_t io, ctl; uint8_t slave; bool unflushed; uint64_t last_flush; };
static struct { struct blockdev bd; struct ata a; } disks[4];
static int ndisks, inited;

static inline void insw(uint16_t p, void *d, uint32_t n) {
    __asm__ volatile("rep insw" : "+D"(d), "+c"(n) : "d"(p) : "memory");
}
static inline void outsw(uint16_t p, const void *s, uint32_t n) {
    __asm__ volatile("rep outsw" : "+S"(s), "+c"(n) : "d"(p) : "memory");
}

static void delay400(struct ata *a) { for (int i = 0; i < 4; i++) inb(a->ctl); }

/* wait for BSY to clear; returns status or -EIO on timeout */
static int wait_bsy(struct ata *a) {
    uint64_t until = ticks_ms + TIMEOUT_MS;
    for (int i = 0; i < SPIN && ticks_ms < until; i++) {
        uint8_t s = inb(a->ctl);
        if (!(s & ST_BSY)) return s;
    }
    kprintf("ata: timeout waiting for BSY\n");
    return -EIO;
}

/* 400ns after a command/select, then wait for ready (and DRQ if asked) */
static int ata_wait(struct ata *a, int drq) {
    delay400(a);
    uint64_t until = ticks_ms + TIMEOUT_MS;
    for (int i = 0; i < SPIN && ticks_ms < until; i++) {
        uint8_t s = inb(a->ctl);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) { kprintf("ata: error status %x (err %x)\n", s, inb(a->io + 1)); return -EIO; }
        if (!drq || (s & ST_DRQ)) return 0;
    }
    kprintf("ata: timeout (drq=%d)\n", drq);
    return -EIO;
}

static void select_drive(struct ata *a, uint32_t lba) {
    outb(a->io + 6, 0xE0 | (a->slave << 4) | ((lba >> 24) & 0xF));
    delay400(a);
}

static int flush(struct ata *a) {
    select_drive(a, 0);
    outb(a->io + 7, 0xE7);
    a->last_flush = ticks_ms;
    a->unflushed = false;
    return ata_wait(a, 0);
}

static int ata_rw(struct blockdev *d, int wr, uint32_t lba, uint32_t cnt, void *buf) {
    struct ata *a = d->priv;
    uint8_t *p = buf;
    if (lba >= d->sectors || cnt > d->sectors - lba) return cnt ? -EIO : 0;
    while (cnt) {
        uint32_t n = MIN(cnt, MAXRUN);
        select_drive(a, lba);
        if (wait_bsy(a) < 0) return -EIO;
        outb(a->io + 2, n);
        outb(a->io + 3, lba);
        outb(a->io + 4, lba >> 8);
        outb(a->io + 5, lba >> 16);
        outb(a->io + 7, wr ? 0x30 : 0x20);
        for (uint32_t i = 0; i < n; i++, p += 512) {
            if (ata_wait(a, 1) < 0) return -EIO;
            if (wr) outsw(a->io, p, 256); else insw(a->io, p, 256);
        }
        lba += n; cnt -= n;
        if (wr) {   /* let the drive finish; flush its cache at most once a second */
            if (ata_wait(a, 0) < 0) return -EIO;
            a->unflushed = true;
            if (ticks_ms - a->last_flush >= 1000 && flush(a) < 0) return -EIO;
        }
    }
    return 0;
}

/* before power-off: push every drive's write cache to the medium */
void ata_flush_all(void) {
    for (int i = 0; i < ndisks; i++) if (disks[i].a.unflushed) flush(&disks[i].a);
}

static int ata_read(struct blockdev *d, uint32_t lba, uint32_t cnt, void *buf) { return ata_rw(d, 0, lba, cnt, buf); }
static int ata_write(struct blockdev *d, uint32_t lba, uint32_t cnt, const void *buf) { return ata_rw(d, 1, lba, cnt, (void *)buf); }

static void probe(uint16_t io, uint16_t ctl, int slave, char letter) {
    struct ata a = { io, ctl, slave };
    uint16_t id[256];
    if (ndisks >= (int)ARRAY_LEN(disks)) return;
    select_drive(&a, 0);
    if (inb(ctl) == 0xFF) return;                       /* floating bus */
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, 0xEC);                                 /* IDENTIFY */
    delay400(&a);
    if (inb(ctl) == 0) return;
    if (wait_bsy(&a) < 0) return;
    if (inb(io + 4) || inb(io + 5)) return;             /* ATAPI / SATA signature */
    if (ata_wait(&a, 1) < 0) return;
    insw(io, id, 256);
    uint32_t sectors = id[60] | ((uint32_t)id[61] << 16);
    if (!(id[49] & 0x200) || !sectors) return;          /* need LBA */
    if (sectors > 0x0FFFFFFF) sectors = 0x0FFFFFFF;     /* LBA28 only */
    struct blockdev *bd = &disks[ndisks].bd;
    disks[ndisks].a = a;
    snprintf(bd->name, sizeof bd->name, "hd%c", letter);
    bd->sectors = sectors;
    bd->read = ata_read;
    bd->write = ata_write;
    bd->priv = &disks[ndisks].a;
    kprintf("ata: %s %u MiB\n", bd->name, sectors / 2048);
    ndisks++;
}

int ata_init(void) {
    if (inited) return ndisks;
    inited = 1;
    outb(0x3F6, 2);                                     /* nIEN: no IRQs */
    outb(0x376, 2);
    probe(0x1F0, 0x3F6, 0, 'a');
    probe(0x1F0, 0x3F6, 1, 'b');
    probe(0x170, 0x376, 0, 'c');
    probe(0x170, 0x376, 1, 'd');
    return ndisks;
}

struct blockdev *blockdev_get(int index) {
    ata_init();
    return index >= 0 && index < ndisks ? &disks[index].bd : NULL;
}

struct part { struct blockdev *disk; uint32_t start; };

static int part_read(struct blockdev *d, uint32_t lba, uint32_t cnt, void *buf) {
    struct part *p = d->priv;
    if (lba >= d->sectors || cnt > d->sectors - lba) return cnt ? -EIO : 0;
    return p->disk->read(p->disk, p->start + lba, cnt, buf);
}
static int part_write(struct blockdev *d, uint32_t lba, uint32_t cnt, const void *buf) {
    struct part *p = d->priv;
    if (lba >= d->sectors || cnt > d->sectors - lba) return cnt ? -EIO : 0;
    return p->disk->write(p->disk, p->start + lba, cnt, buf);
}

struct blockdev *blockdev_partition(struct blockdev *disk, uint32_t start, uint32_t len) {
    if (!disk || start > disk->sectors || len > disk->sectors - start) return NULL;
    struct blockdev *bd = kzalloc(sizeof *bd);
    struct part *p = kzalloc(sizeof *p);
    if (!bd || !p) { kfree(bd); kfree(p); return NULL; }
    p->disk = disk; p->start = start;
    snprintf(bd->name, sizeof bd->name, "%sp", disk->name);
    bd->sectors = len;
    bd->read = part_read;
    bd->write = part_write;
    bd->priv = p;
    return bd;
}

/* first type-0x83 MBR partition of disk 0 */
struct blockdev *blockdev_root_partition(void) {
    struct blockdev *d = blockdev_get(0);
    uint8_t mbr[512];
    if (!d || d->read(d, 0, 1, mbr) < 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) return NULL;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        if (e[4] != 0x83) continue;
        uint32_t start, len;
        memcpy(&start, e + 8, 4);
        memcpy(&len, e + 12, 4);
        return blockdev_partition(d, start, len);
    }
    return NULL;
}
