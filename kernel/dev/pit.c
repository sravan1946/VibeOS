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
