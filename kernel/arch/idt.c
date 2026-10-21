#include "arch.h"
#include "console.h"

struct idt_entry { uint16_t lo, sel; uint8_t zero, flags; uint16_t hi; } PACKED;
static struct idt_entry idt[256];
static struct { uint16_t limit; uint32_t base; } PACKED idtr;
static isr_t handlers[256];
extern const uint32_t isr_table[256];

static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack fault", "General protection fault", "Page fault", "Reserved",
    "x87 FPU error", "Alignment check", "Machine check", "SIMD FP error", "Virtualization",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security", "Reserved",
};

static void pic_init(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();   /* vectors 32..47 */
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFB);                                           /* all masked but cascade */
    outb(0xA1, 0xFF);
}

void irq_mask(int irq, bool masked) {
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    uint8_t bit = 1 << (irq & 7);
    uint8_t v = inb(port);
    outb(port, masked ? (v | bit) : (v & ~bit));
}

void isr_register(int vec, isr_t h) { handlers[vec] = h; }
void irq_register(int irq, isr_t h) { handlers[32 + irq] = h; irq_mask(irq, false); }

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t a = isr_table[i];
        /* 0x8E: ring-0 interrupt gate; 0x80 syscall: ring-3 callable trap gate */
        uint8_t flags = i == 0x80 ? 0xEF : 0x8E;
        idt[i] = (struct idt_entry){ a & 0xFFFF, KERNEL_CS, 0, flags, a >> 16 };
    }
    idtr.limit = sizeof idt - 1;
    idtr.base = (uint32_t)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    pic_init();
}

/* weak hooks the process layer overrides */
__attribute__((weak)) bool proc_handle_fault(struct regs *r) { (void)r; return false; }
__attribute__((weak)) void sched_irq_exit(struct regs *r) { (void)r; }

void isr_dispatch(struct regs *r) {
    uint32_t n = r->int_no;
    if (n >= 32 && n < 48) {
        if (n == 39) {                           /* spurious IRQ7: no EOI */
            outb(0x20, 0x0B);
            if (!(inb(0x20) & 0x80)) return;
        }
        if (n >= 40) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        if (handlers[n]) handlers[n](r);
        sched_irq_exit(r);
        return;
    }
    if (handlers[n]) { handlers[n](r); if (n == 0x80) sched_irq_exit(r); return; }
    if (n < 32) {
        if (proc_handle_fault(r)) { sched_irq_exit(r); return; }
        char msg[96];
        snprintf(msg, sizeof msg, "%s (#%u, err=%x)%s", exc_names[n], n, r->err,
                 n == 14 ? " - see CR2" : "");
        cli();
        bootcon_panic(msg, r);
        for (;;) hlt();
    }
}
