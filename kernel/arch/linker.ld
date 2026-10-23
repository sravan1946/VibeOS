ENTRY(_start)
SECTIONS {
    . = 0x100000;
    __kernel_start = .;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) }
    .bss : ALIGN(16) { __bss_start = .; *(COMMON) *(.bss .bss.*) __bss_end = .; }
    __kernel_end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
