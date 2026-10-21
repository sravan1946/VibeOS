; setjmp/longjmp: jmp_buf = ebx esi edi ebp esp eip
bits 32
global setjmp, longjmp

section .text
setjmp:
    mov eax, [esp + 4]
    mov [eax], ebx
    mov [eax + 4], esi
    mov [eax + 8], edi
    mov [eax + 12], ebp
    lea ecx, [esp + 4]          ; esp after return
    mov [eax + 16], ecx
    mov ecx, [esp]              ; return address
    mov [eax + 20], ecx
    xor eax, eax
    ret

longjmp:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    test eax, eax
    jnz .nz
    inc eax
.nz:
    mov ebx, [edx]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    jmp dword [edx + 20]

section .note.GNU-stack noalloc noexec nowrite progbits
