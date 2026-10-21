#include "arch.h"

struct tss {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} PACKED;

static uint64_t gdt[6];
static struct tss tss;
static struct { uint16_t limit; uint32_t base; } PACKED gdtr;
extern void gdt_flush(void *gdtr);

static uint64_t seg(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    uint64_t d = (limit & 0xFFFF) | ((uint64_t)(base & 0xFFFFFF) << 16);
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)(flags & 0xF) << 52;
    d |= (uint64_t)(base >> 24) << 56;
    return d;
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = seg(0, 0xFFFFF, 0x9A, 0xC);   /* kernel code */
    gdt[2] = seg(0, 0xFFFFF, 0x92, 0xC);   /* kernel data */
    gdt[3] = seg(0, 0xFFFFF, 0xFA, 0xC);   /* user code */
    gdt[4] = seg(0, 0xFFFFF, 0xF2, 0xC);   /* user data */
    memset(&tss, 0, sizeof tss);
    tss.ss0 = KERNEL_DS;
    tss.iomap = sizeof tss;
    gdt[5] = seg((uint32_t)&tss, sizeof tss - 1, 0x89, 0x0);
    gdtr.limit = sizeof gdt - 1;
    gdtr.base = (uint32_t)gdt;
    gdt_flush(&gdtr);
}

void tss_set_kernel_stack(uint32_t esp0) { tss.esp0 = esp0; }
