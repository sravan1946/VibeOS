; crt0.asm - process entry. Stack on entry: argc, argv[0..argc-1], NULL, envp[...], NULL
bits 32
global _start
extern __libc_init, main, exit

section .text
_start:
    xor ebp, ebp
    mov esi, [esp]              ; argc
    lea edi, [esp + 4]          ; argv
    lea ebx, [edi + esi*4 + 4]  ; envp
    and esp, -16
    sub esp, 4                  ; keep the 3-arg frame 16-byte aligned
    push ebx
    push edi
    push esi
    call __libc_init            ; (argc, argv, envp): sets environ
    call main                   ; same three args still on the stack
    sub esp, 12
    push eax
    call exit
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
