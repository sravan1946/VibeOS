; interrupt stubs for all 256 vectors -> isr_dispatch(struct regs *)
BITS 32
section .text
extern isr_dispatch

%macro STUB 1
isr%1:
%if (%1 = 8) || (%1 >= 10 && %1 <= 14) || (%1 = 17) || (%1 = 21) || (%1 = 29) || (%1 = 30)
%else
    push dword 0
%endif
    push dword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 256
STUB i
%assign i i+1
%endrep

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call isr_dispatch
    add esp, 4
global isr_return
isr_return:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dd isr %+ i
%assign i i+1
%endrep

section .text
; void context_switch(uint32_t *save_esp, uint32_t new_esp)
global context_switch
context_switch:
    mov eax, [esp+4]
    mov edx, [esp+8]
    pushf
    push ebp
    push ebx
    push esi
    push edi
    mov [eax], esp
    mov esp, edx
    pop edi
    pop esi
    pop ebx
    pop ebp
    popf
    ret

global gdt_flush
gdt_flush:
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.reload
.reload:
    mov ax, 0x28
    ltr ax
    ret
