; kernel/entry.asm
; Entry point for the kernel: set up stack and call kernel_main (NASM syntax)

BITS 32
section .text.entry
    global _start
    extern kernel_main

_start:
    ; Write 'A' to VGA to show kernel entry
    mov ax, 0x0F41
    mov [0xB8000], ax

    ; Set up our own GDT in the kernel
    lgdt [gdt_descriptor]

    ; Reload segment registers with our GDT selectors
    mov ax, 0x10        ; Data segment selector (2nd entry)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x20000

    ; Far jump to flush the instruction pipeline and load CS
    jmp 0x08:kernel_main

; GDT definition (must be 8-byte aligned)
section .data
align 8
GDT:
    dq 0x0000000000000000     ; Null descriptor
    dq 0x00cf9a000000ffff     ; Code segment: base=0, limit=4GB, 32-bit, RX
    dq 0x00cf92000000ffff     ; Data segment: base=0, limit=4GB, 32-bit, RW

gdt_descriptor:
    dw (GDT_end - GDT - 1)
    dd GDT
GDT_end:

.hang:
    jmp .hang           ; Never return 