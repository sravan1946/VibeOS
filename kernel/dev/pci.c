/* pci.c - config-space enumeration (mechanism #1, ports 0xCF8/0xCFC) */
#include "net.h"

#define MAX_PCI 32
static struct pci_dev devs[MAX_PCI];
static int ndevs;

uint32_t pci_read(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    outl(0xCF8, 1u << 31 | (uint32_t)b << 16 | (uint32_t)d << 11 | (uint32_t)f << 8 | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) {
    outl(0xCF8, 1u << 31 | (uint32_t)b << 16 | (uint32_t)d << 11 | (uint32_t)f << 8 | (off & 0xFC));
    outl(0xCFC, v);
}

void pci_init(void) {
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++) {
            uint32_t id = pci_read(b, d, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            int nf = pci_read(b, d, 0, 0x0C) & 0x800000 ? 8 : 1;
            for (int f = 0; f < nf; f++) {
                id = pci_read(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t cls = pci_read(b, d, f, 8) >> 8;
                kprintf("pci %02x:%02x.%x %04x:%04x class %06x\n", b, d, f, id & 0xFFFF, id >> 16, cls);
                if (ndevs == MAX_PCI) continue;
                struct pci_dev *p = &devs[ndevs++];
                p->bus = b; p->dev = d; p->fn = f; p->vendor = id & 0xFFFF; p->device = id >> 16;
                p->cls = cls;
                p->irq = pci_read(b, d, f, 0x3C) & 0xFF;
                for (int i = 0; i < 6; i++) p->bar[i] = pci_read(b, d, f, 0x10 + i * 4);
            }
        }
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device) { *out = devs[i]; return 0; }
    return -ENODEV;
}
