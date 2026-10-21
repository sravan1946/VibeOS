/* user.ld - static ELF32 at 0x40000000: RX segment (text + rodata), then RW segment (data + bss) */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);      /* R X */
    data PT_LOAD FLAGS(6);      /* R W */
}

SECTIONS {
    . = 0x40001000;
    .text : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text

    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
