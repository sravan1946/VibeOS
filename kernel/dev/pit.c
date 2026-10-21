#include "arch.h"

volatile uint64_t ticks_ms;
__attribute__((weak)) void sched_tick(void) {}

static void pit_irq(struct regs *r) {
    (void)r;
    ticks_ms++;
    sched_tick();
}

void pit_init(void) {
    uint32_t div = 1193182 / 1000;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_register(0, pit_irq);
}

/* PC speaker on PIT channel 2; hz == 0 silences it */
void speaker_tone(uint32_t hz) {
    if (!hz) { outb(0x61, inb(0x61) & ~3); return; }
    uint32_t div = 1193182 / hz;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);
}
