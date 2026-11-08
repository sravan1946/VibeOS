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
    ; no reload of ds/es/fs/gs: the user data selector is a flat 4 GiB segment too, so ring 0 can use it as is,
    ; and the pops at isr_return put back whatever the frame says (a new task's frame carries the user ones)
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

; sysenter fast path (libc's __syscall0..3). The user stub passes its return eip in EBP and its esp in ESI (so
; these calls take at most 3 args; 4-5 arg and mmap calls stay on int 0x80). Entry has IF=0 and ESP=SYSENTER_ESP,
; which is unused: the kernel stack comes from tss.esp0 (offset 4), so a switch needs no wrmsr. The frame built is
; the one an int 0x80 from ring 3 would push, so the C side is shared; the exit uses sysexit instead of iret.
extern tss
global sysenter_entry
sysenter_entry:
    mov esp, [tss + 4]
    push dword 0x23             ; ss
    push esi                    ; useresp
    pushfd
    or dword [esp], 0x200       ; IF on in the saved flags
    push dword 0x1b             ; cs
    push ebp                    ; eip
    push dword 0
    push dword 0x80
    pusha
    push ds
    push es
    push fs
    push gs
    cld
    push esp
    call isr_dispatch
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    pop edi
    pop esi
    pop ebp
    add esp, 4                  ; pusha's esp
    pop ebx
    add esp, 8                  ; edx, ecx: sysexit takes them
    pop eax
    add esp, 8                  ; int_no, err
    pop edx                     ; eip
    add esp, 4                  ; cs
    popfd                       ; saved flags, IF on
    pop ecx                     ; useresp
    sysexit

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

; first switch into a kernel thread: EBX = fn, ESI = arg
global kthread_entry
extern kthread_exit
kthread_entry:
    sti
    push esi
    call ebx
    push eax
    call kthread_exit
.hang:
    jmp .hang
