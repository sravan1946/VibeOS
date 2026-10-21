/* arch.h - x86 descriptor tables, interrupts, paging */
#pragma once
#include "kernel.h"

/* trap frame pushed by isr.asm (+ CPU) */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp0, ebx, edx, ecx, eax;   /* pusha */
    uint32_t int_no, err;
    uint32_t eip, cs, eflags, useresp, ss;              /* useresp/ss only from ring 3 */
};

typedef void (*isr_t)(struct regs *r);

void gdt_init(void);
void tss_set_kernel_stack(uint32_t esp0);
void idt_init(void);
void isr_register(int vec, isr_t h);
void irq_register(int irq, isr_t h);   /* irq 0..15, unmasks it */
void irq_mask(int irq, bool masked);

static inline uint32_t read_cr0(void) { uint32_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint32_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void invlpg(uint32_t a) { __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory"); }

/* ---- paging (mm/vmm.c) ----
 * 0x00000000-0x3FFFFFFF  kernel: identity-mapped RAM, 4 MiB pages, supervisor
 * 0x40000000-0xBFFFFFFF  user space, 4 KiB pages, per process
 * 0xC0000000-0xFFFFFFFF  kernel: MMIO (framebuffer...) identity-mapped */
#define USER_BASE   0x40000000u
#define USER_TOP    0xC0000000u
#define USER_STACK_TOP 0xBFFF0000u
#define PTE_P 0x001
#define PTE_W 0x002
#define PTE_U 0x004
#define PTE_PS 0x080
#define PTE_NOFREE 0x200   /* available bit: frame not owned by this mapping */

void      vmm_init(void);
uint32_t  vmm_kernel_dir(void);
uint32_t  vmm_new_dir(void);                    /* fresh address space (kernel half shared) */
void      vmm_free_dir(uint32_t dir);           /* frees user pages + tables */
int       vmm_map(uint32_t dir, uint32_t va, uint32_t pa, uint32_t flags);
uint32_t  vmm_translate(uint32_t dir, uint32_t va);   /* 0 if unmapped */
int       vmm_alloc_range(uint32_t dir, uint32_t va, uint32_t len, uint32_t flags);  /* zeroed pages */
void      vmm_unmap_range(uint32_t dir, uint32_t va, uint32_t len, bool free_frames);
void      vmm_map_mmio(uint32_t pa, uint32_t len);   /* identity, kernel only */
bool      vmm_user_ok(uint32_t dir, uint32_t va, uint32_t len, bool write);
