#include "arch.h"

volatile uint64_t ticks_ms;
__attribute__((weak)) void sched_tick(void) {}

/* Sub-ms clock: the TSC is stamped on every tick and the time since the last tick is added to ticks_ms, so the
 * clock is anchored to the PIT (no drift) and the TSC rate only has to be good to ~1 ms. The rate is measured over
 * the first 64 ms of ticks; until then (and with a TSC that doesn't tick) the clock is just ticks_ms. */
static uint64_t tick_tsc, first_tsc;
static uint32_t tsc_khz, us_mult;          /* us_mult = 1000 * 2^32 / khz: cycles -> us as (cycles * us_mult) >> 32 */

static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

static void pit_irq(struct regs *r) {
    (void)r;
    tick_tsc = rdtsc();
    ticks_ms++;
    if (ticks_ms == 1) first_tsc = tick_tsc;
    else if (ticks_ms == 65) {
        uint64_t khz = (tick_tsc - first_tsc) / 64;        /* cycles over 64 ticks = per ms = kHz */
        if (khz >= 100000 && khz < 0xFFFFFFFFu / 4) { us_mult = (uint32_t)(((uint64_t)1000 << 32) / khz); tsc_khz = (uint32_t)khz; }
    }
    sched_tick();
}

uint64_t clock_us(void) {
    uint32_t f = irq_save();
    uint64_t ms = ticks_ms, d = rdtsc() - tick_tsc;
    irq_restore(f);
    uint32_t us = 0;
    if (tsc_khz) us = d >= tsc_khz ? 999 : (uint32_t)(((uint64_t)(uint32_t)d * us_mult) >> 32);   /* < 1 ms: never runs into the next tick */
    return ms * 1000 + us;
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
