; kernel entry, jumped to by stage2 at 0x100000 in 32-bit protected mode
; EBX = boot_info
BITS 32
section .text.entry
global _start
extern kmain, __bss_start, __bss_end

_start:
    cli
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    mov esp, boot_stack_top
    xor ebp, ebp
    push ebx
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
global boot_stack_top
boot_stack: resb 16384
boot_stack_top:
