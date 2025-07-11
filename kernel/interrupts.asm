; kernel/interrupts.asm
; Assembly stubs for timer and keyboard interrupt handlers and IDT loader

[BITS 32]

extern timer_handler
extern keyboard_handler

global isr_timer
isr_timer:
    pushad
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call timer_handler
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iretd

global isr_keyboard
isr_keyboard:
    pushad
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    call keyboard_handler
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iretd

global idt_load
idt_load:
    mov eax, [esp+4]
    lidt [eax]
    ret 