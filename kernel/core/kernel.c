#include "../drivers/console.h"
#include "../drivers/keyboard.h"
#include "shell.h"
#include "../arch/exceptions.h"
#include "../fs/filesystem.h"

volatile unsigned int key_irq_count = 0;
volatile unsigned int timer_irq_count = 0;

// IDT entry structure (32-bit)
struct idt_entry {
    unsigned short base_lo;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short base_hi;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

#define IDT_ENTRIES 256
struct idt_entry idt[IDT_ENTRIES];
struct idt_ptr idtp;

void idt_set_gate(int n, unsigned int handler) {
    idt[n].base_lo = handler & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].always0 = 0;
    idt[n].flags = 0x8E;
    idt[n].base_hi = (handler >> 16) & 0xFFFF;
}

extern void idt_load(unsigned int);
extern void isr_timer(void);
extern void isr_keyboard(void);

void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFC & ~((1 << 0) | (1 << 1)));
    outb(0xA1, 0xFF);
}

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idtp.base = (unsigned int)&idt;
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0);
    }
    extern void isr_exception0(void);
    extern void isr_exception1(void);
    extern void isr_exception2(void);
    extern void isr_exception3(void);
    extern void isr_exception4(void);
    extern void isr_exception5(void);
    extern void isr_exception6(void);
    extern void isr_exception7(void);
    extern void isr_exception8(void);
    extern void isr_exception9(void);
    extern void isr_exception10(void);
    extern void isr_exception11(void);
    extern void isr_exception12(void);
    extern void isr_exception13(void);
    extern void isr_exception14(void);
    extern void isr_exception15(void);
    extern void isr_exception16(void);
    extern void isr_exception17(void);
    extern void isr_exception18(void);
    extern void isr_exception19(void);
    extern void isr_exception20(void);
    extern void isr_exception21(void);
    extern void isr_exception22(void);
    extern void isr_exception23(void);
    extern void isr_exception24(void);
    extern void isr_exception25(void);
    extern void isr_exception26(void);
    extern void isr_exception27(void);
    extern void isr_exception28(void);
    extern void isr_exception29(void);
    extern void isr_exception30(void);
    extern void isr_exception31(void);
    idt_set_gate(0,  (unsigned int)isr_exception0);
    idt_set_gate(1,  (unsigned int)isr_exception1);
    idt_set_gate(2,  (unsigned int)isr_exception2);
    idt_set_gate(3,  (unsigned int)isr_exception3);
    idt_set_gate(4,  (unsigned int)isr_exception4);
    idt_set_gate(5,  (unsigned int)isr_exception5);
    idt_set_gate(6,  (unsigned int)isr_exception6);
    idt_set_gate(7,  (unsigned int)isr_exception7);
    idt_set_gate(8,  (unsigned int)isr_exception8);
    idt_set_gate(9,  (unsigned int)isr_exception9);
    idt_set_gate(10, (unsigned int)isr_exception10);
    idt_set_gate(11, (unsigned int)isr_exception11);
    idt_set_gate(12, (unsigned int)isr_exception12);
    idt_set_gate(13, (unsigned int)isr_exception13);
    idt_set_gate(14, (unsigned int)isr_exception14);
    idt_set_gate(15, (unsigned int)isr_exception15);
    idt_set_gate(16, (unsigned int)isr_exception16);
    idt_set_gate(17, (unsigned int)isr_exception17);
    idt_set_gate(18, (unsigned int)isr_exception18);
    idt_set_gate(19, (unsigned int)isr_exception19);
    idt_set_gate(20, (unsigned int)isr_exception20);
    idt_set_gate(21, (unsigned int)isr_exception21);
    idt_set_gate(22, (unsigned int)isr_exception22);
    idt_set_gate(23, (unsigned int)isr_exception23);
    idt_set_gate(24, (unsigned int)isr_exception24);
    idt_set_gate(25, (unsigned int)isr_exception25);
    idt_set_gate(26, (unsigned int)isr_exception26);
    idt_set_gate(27, (unsigned int)isr_exception27);
    idt_set_gate(28, (unsigned int)isr_exception28);
    idt_set_gate(29, (unsigned int)isr_exception29);
    idt_set_gate(30, (unsigned int)isr_exception30);
    idt_set_gate(31, (unsigned int)isr_exception31);
    idt_set_gate(32, (unsigned int)isr_timer);
    idt_set_gate(33, (unsigned int)isr_keyboard);
    idt_load((unsigned int)&idtp);
}

static inline void sti(void) { __asm__ volatile ("sti"); }

void kernel_main(void) __attribute__((noreturn, section(".text.start")));
void kernel_main(void) {
    fs_init();
    clear_screen();
    pic_remap();
    idt_init();
    unsigned int divisor = 1193182 / 10;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    sti();
    // Enable keyboard controller (PS/2) IRQs and scanning
    outb(0x64, 0xAE); // Enable keyboard interface
    outb(0x60, 0xF4); // Enable scanning
    print("VibeOS Shell\n\n");
    print_prompt();
    while (1) {}
} 