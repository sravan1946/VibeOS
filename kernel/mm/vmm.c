/* vmm.c - two-level x86 paging, see the layout in arch.h */
#include "arch.h"

static uint32_t *kdir;

uint32_t vmm_kernel_dir(void) { return (uint32_t)kdir; }

void vmm_init(void) {
    kdir = (uint32_t *)pmm_alloc();
    for (uint32_t i = 0; i < 256; i++) kdir[i] = (i << 22) | PTE_P | PTE_W | PTE_PS;
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10;                                           /* PSE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    write_cr3((uint32_t)kdir);
    __asm__ volatile("mov %%cr0, %%eax; or $0x80000000, %%eax; mov %%eax, %%cr0" ::: "eax");
}

void vmm_map_mmio(uint32_t pa, uint32_t len) {
    for (uint32_t a = ALIGN_DOWN(pa, 0x400000); a < pa + len && a >= ALIGN_DOWN(pa, 0x400000); a += 0x400000) {
        kdir[a >> 22] = a | PTE_P | PTE_W | PTE_PS | 0x10;   /* PCD */
        invlpg(a);
    }
}

uint32_t vmm_new_dir(void) {
    uint32_t *d = (uint32_t *)pmm_alloc();
    if (!d) return 0;
    for (int i = 0; i < 1024; i++)
        if (i < 256 || i >= 768) d[i] = kdir[i];
    return (uint32_t)d;
}

static uint32_t *pte_for(uint32_t dir, uint32_t va, bool create) {
    uint32_t *d = (uint32_t *)dir;
    uint32_t *pde = &d[va >> 22];
    if (!(*pde & PTE_P)) {
        if (!create) return 0;
        uint32_t t = pmm_alloc();
        if (!t) return 0;
        *pde = t | PTE_P | PTE_W | PTE_U;
    }
    if (*pde & PTE_PS) return 0;
    return &((uint32_t *)(*pde & ~0xFFFu))[(va >> 12) & 0x3FF];
}

int vmm_map(uint32_t dir, uint32_t va, uint32_t pa, uint32_t flags) {
    uint32_t *pte = pte_for(dir, va, true);
    if (!pte) return -1;
    *pte = (pa & ~0xFFFu) | flags | PTE_P;
    if (dir == read_cr3()) invlpg(va);
    return 0;
}

uint32_t vmm_translate(uint32_t dir, uint32_t va) {
    uint32_t *pte = pte_for(dir, va, false);
    if (!pte || !(*pte & PTE_P)) return 0;
    return (*pte & ~0xFFFu) | (va & 0xFFF);
}

int vmm_alloc_range(uint32_t dir, uint32_t va, uint32_t len, uint32_t flags) {
    for (uint32_t a = ALIGN_DOWN(va, PAGE_SIZE); a < va + len; a += PAGE_SIZE) {
        if (vmm_translate(dir, a)) continue;
        uint32_t f = pmm_alloc();
        if (!f || vmm_map(dir, a, f, flags) < 0) return -1;
    }
    return 0;
}

void vmm_unmap_range(uint32_t dir, uint32_t va, uint32_t len, bool free_frames) {
    for (uint32_t a = ALIGN_DOWN(va, PAGE_SIZE); a < va + len; a += PAGE_SIZE) {
        uint32_t *pte = pte_for(dir, a, false);
        if (!pte || !(*pte & PTE_P)) continue;
        if (free_frames && !(*pte & PTE_NOFREE)) pmm_free(*pte & ~0xFFFu);
        *pte = 0;
        if (dir == read_cr3()) invlpg(a);
    }
}

void vmm_free_dir(uint32_t dir) {
    uint32_t *d = (uint32_t *)dir;
    for (int i = USER_BASE >> 22; i < (int)(USER_TOP >> 22); i++) {
        if (!(d[i] & PTE_P)) continue;
        uint32_t *t = (uint32_t *)(d[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++)
            if ((t[j] & PTE_P) && !(t[j] & PTE_NOFREE)) pmm_free(t[j] & ~0xFFFu);
        pmm_free((uint32_t)t);
    }
    pmm_free(dir);
}

bool vmm_user_ok(uint32_t dir, uint32_t va, uint32_t len, bool write) {
    if (va < USER_BASE || va + len < va || va + len > USER_TOP) return false;
    for (uint32_t a = ALIGN_DOWN(va, PAGE_SIZE); a < va + len; a += PAGE_SIZE) {
        uint32_t *pte = pte_for(dir, a, false);
        if (!pte || !(*pte & PTE_P) || !(*pte & PTE_U)) return false;
        if (write && !(*pte & PTE_W)) return false;
    }
    return true;
}
