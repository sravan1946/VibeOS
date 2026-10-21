#include "arch.h"
#include "console.h"

struct boot_info *boot_info;

void kmain(struct boot_info *bi) {
    boot_info = bi;
    serial_init();
    if (bi->magic != 0x45424956) { kprintf("bad boot_info magic\n"); for (;;) hlt(); }
    fb_init();
    kprintf("\x1b[35mVibeOS\x1b[0m v2 booting  (%ux%ux%u fb at %08x)\n",
            bi->fb_width, bi->fb_height, bi->fb_bpp, bi->fb_addr);

    gdt_init();
    idt_init();
    pmm_init();
    uint32_t ram_mb = pmm_total_count() / 256;
    uint32_t heap_bytes = (ram_mb >= 200 ? 32 : 16) << 20;
    uint32_t heap = pmm_alloc_contig(heap_bytes / PAGE_SIZE);
    if (!heap) panic("no memory for kernel heap");
    heap_init(heap, heap_bytes);
    vmm_init();
    vmm_map_mmio(bi->fb_addr, bi->fb_pitch * bi->fb_height);
    kprintf("memory: %u MiB usable, heap %u MiB at %08x\n", ram_mb, heap_bytes >> 20, heap);

    pit_init();
    sti();
    kprintf("\x1b[32mok\x1b[0m interrupts on\n");

    uint64_t t = ticks_ms;
    while (ticks_ms < t + 100) hlt();
    kprintf("timer: 100 ms elapsed\n");
    for (;;) hlt();
}
