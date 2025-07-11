/* kernel/linker.ld
 * -----------------
 * This linker script places the kernel at physical address 0x10000 (0x1000:0x0000).
 * The entry point is _start (assembly stub), which sets up the stack and calls kernel_main.
 *
 * The sections are mapped as follows:
 *   .text - code (placed at the start)
 *   .data - initialized data
 *   .bss  - uninitialized data
 *
 * Unnecessary sections are discarded to ensure the entry point is at the start.
 */
ENTRY(_start)
SECTIONS {
    . = 0x10000;
    .text : {
        *(.text.entry)   /* entry.asm stub */
        *(.text.start)   /* kernel_main */
        *(.text*)        /* rest of code */
    }
    .data : { *(.data .data.*) }
    .bss  : { *(.bss .bss.*) }
    /DISCARD/ : { *(.eh_frame) *(.comment) }
} 