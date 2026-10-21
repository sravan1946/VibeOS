/* pmm.c - bitmap physical frame allocator for the first 1 GiB of RAM
 * (that's all the kernel identity-maps; ponytail: more RAM is ignored). */
#include "kernel.h"

#define MAX_FRAMES (0x40000000u / PAGE_SIZE)
static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total, nfree, hint;
extern char __kernel_end[];

static inline bool used(uint32_t f) { return bitmap[f >> 5] & (1u << (f & 31)); }
static inline void set(uint32_t f)   { bitmap[f >> 5] |= 1u << (f & 31); }
static inline void clr(uint32_t f)   { bitmap[f >> 5] &= ~(1u << (f & 31)); }

void pmm_init(void) {
    memset(bitmap, 0xFF, sizeof bitmap);
    struct e820_entry *e = (struct e820_entry *)boot_info->mmap_addr;
    uint32_t reserve = ALIGN_UP((uint32_t)__kernel_end, PAGE_SIZE);
    for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
        if (e[i].type != 1) continue;
        uint64_t b = ALIGN_UP(e[i].base, PAGE_SIZE), end = ALIGN_DOWN(e[i].base + e[i].len, PAGE_SIZE);
        if (end > 0x40000000ull) end = 0x40000000ull;
        for (uint64_t a = b; a < end; a += PAGE_SIZE) {
            if (a < reserve) continue;                  /* low memory + kernel image */
            uint32_t f = a / PAGE_SIZE;
            if (used(f)) { clr(f); nfree++; total++; }
        }
    }
    hint = reserve / PAGE_SIZE;
}

uint32_t pmm_alloc(void) {
    for (uint32_t n = 0; n < MAX_FRAMES; n++) {
        uint32_t f = (hint + n) % MAX_FRAMES;
        if (bitmap[f >> 5] == 0xFFFFFFFF) { n += 31 - (f & 31); continue; }
        if (!used(f)) {
            set(f); nfree--; hint = f + 1;
            memset((void *)(f * PAGE_SIZE), 0, PAGE_SIZE);
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint32_t pmm_alloc_contig(uint32_t n) {
    uint32_t run = 0;
    for (uint32_t f = 256; f < MAX_FRAMES; f++) {
        run = used(f) ? 0 : run + 1;
        if (run == n) {
            uint32_t start = f + 1 - n;
            for (uint32_t i = start; i <= f; i++) set(i);
            nfree -= n;
            return start * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint32_t a) {
    uint32_t f = a / PAGE_SIZE;
    if (f < MAX_FRAMES && used(f)) { clr(f); nfree++; if (f < hint) hint = f; }
}

void pmm_free_contig(uint32_t a, uint32_t n) { for (uint32_t i = 0; i < n; i++) pmm_free(a + i * PAGE_SIZE); }
uint32_t pmm_free_count(void) { return nfree; }
uint32_t pmm_total_count(void) { return total; }
