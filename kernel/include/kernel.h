/* kernel.h - things every kernel file needs */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B
#define USER_DS   0x23
#define TSS_SEL   0x28

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) > (b) ? (a) : (b))
#define UNUSED           __attribute__((unused))
#define PACKED           __attribute__((packed))
#define NORETURN         __attribute__((noreturn))

/* ---- port I/O ---- */
static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

/* ---- interrupt flag ---- */
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline uint32_t irq_save(void) {
    uint32_t f; __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void irq_restore(uint32_t f) { if (f & 0x200) sti(); }

/* ---- boot info (filled by boot/stage2.asm) ---- */
struct boot_info {
    uint32_t magic, boot_drive, mmap_count, mmap_addr;
    uint32_t fb_addr, fb_pitch, fb_width, fb_height, fb_bpp;
    uint32_t kernel_size;
};
struct e820_entry { uint64_t base, len; uint32_t type, acpi; } PACKED;
extern struct boot_info *boot_info;

/* ---- lib/string.c ---- */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
size_t strlcpy(char *d, const char *s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strdup(const char *s);
static inline void memset32(uint32_t *d, uint32_t v, size_t n) {
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

/* ---- lib/printf.c ---- */
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define assert(c) do { if (!(c)) panic("assert failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

/* ---- dev/serial.c ---- */
void serial_init(void);
void serial_putc(char c);
int  serial_getc(void);   /* -1 if nothing waiting */

/* ---- mm ---- */
#define PAGE_SIZE 4096
void      pmm_init(void);
uint32_t  pmm_alloc(void);                 /* one zeroed frame, 0 on OOM */
uint32_t  pmm_alloc_contig(uint32_t n);    /* n contiguous frames, not zeroed */
void      pmm_free(uint32_t addr);
void      pmm_free_contig(uint32_t addr, uint32_t n);
uint32_t  pmm_free_count(void);
uint32_t  pmm_total_count(void);
void     *kmalloc(size_t n);
void     *kzalloc(size_t n);
void     *krealloc(void *p, size_t n);
void      kfree(void *p);
void      heap_init(uint32_t start, uint32_t size);
size_t    heap_used(void);
size_t    heap_size(void);

/* ---- time (dev/pit.c) ---- */
extern volatile uint64_t ticks_ms;
void pit_init(void);
