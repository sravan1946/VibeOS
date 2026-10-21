; VibeOS stage 2. Runs at 0x7E00 in real mode.
;  1. enable A20, init COM1
;  2. E820 memory map  -> 0x6000 (24-byte entries)
;  3. load the kernel (KERNEL_SECTORS from KERNEL_LBA) to 1 MiB through a
;     0x10000 bounce buffer, copying in unreal mode
;  4. pick a 32bpp linear-framebuffer VBE mode (PREF_W x PREF_H preferred)
;  5. enter protected mode and jump to 0x100000 with EBX = &boot_info
;
; boot_info at 0x5000 (all u32):
;  0 magic 'VIBE'  4 boot_drive  8 mmap_count  12 mmap_addr
; 16 fb_addr  20 fb_pitch  24 fb_width  28 fb_height  32 fb_bpp  36 kernel_size

BITS 16
ORG 0x7E00

BOOTINFO    equ 0x5000
MMAP        equ 0x6000
VBE_CTRL    equ 0x2000
VBE_MODE    equ 0x3000
BOUNCE_SEG  equ 0x1000
CHUNK       equ 64

%ifndef PREF_W
%define PREF_W 1280
%endif
%ifndef PREF_H
%define PREF_H 800
%endif

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [boot_drive], dl

    ; zero boot_info
    mov di, BOOTINFO
    mov cx, 64
    xor ax, ax
    rep stosw
    mov dword [BOOTINFO+0], 0x45424956
    movzx eax, byte [boot_drive]
    mov [BOOTINFO+4], eax
    mov dword [BOOTINFO+12], MMAP
    mov dword [BOOTINFO+36], KERNEL_SECTORS*512

    call serial_init
    mov si, msg_stage2
    call puts

    ; --- A20 ---
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    ; --- E820 ---
    mov di, MMAP
    xor ebx, ebx
    xor bp, bp
.e820:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [es:di+20], 1
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    jcxz .e820_skip
    inc bp
    add di, 24
.e820_skip:
    test ebx, ebx
    jz .e820_done
    cmp bp, 128
    jb .e820
.e820_done:
    movzx eax, bp
    mov [BOOTINFO+8], eax
    test bp, bp
    jnz .have_mmap
    mov si, msg_nommap
    jmp die
.have_mmap:

    ; --- kernel ---
    mov si, msg_kernel
    call puts
.load:
    mov eax, [remaining]
    test eax, eax
    jz .loaded
    cmp eax, CHUNK
    jbe .chunk_ok
    mov eax, CHUNK
.chunk_ok:
    mov [chunk], eax
    mov [dap_count], ax
    mov eax, [cur_lba]
    mov [dap_lba], eax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE_SEG
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    call unreal
    mov ecx, [chunk]
    shl ecx, 7                  ; dwords
    mov esi, BOUNCE_SEG*16
    mov edi, [dest]
    a32 rep movsd
    mov [dest], edi

    mov eax, [chunk]
    add [cur_lba], eax
    sub [remaining], eax
    mov al, '.'
    call putc
    jmp .load
.loaded:
    mov si, msg_ok
    call puts

    ; --- VBE ---
    mov dword [VBE_CTRL], 'VBE2'
    mov ax, 0x4F00
    mov di, VBE_CTRL
    int 0x10
    cmp ax, 0x004F
    jne no_vbe
    mov ax, [VBE_CTRL+14]
    mov [mode_ptr], ax
    mov ax, [VBE_CTRL+16]
    mov [mode_ptr+2], ax
.next_mode:
    push ds
    lds si, [mode_ptr]
    mov cx, [si]
    pop ds
    add word [mode_ptr], 2
    cmp cx, 0xFFFF
    je .scan_done
    mov [cur_mode], cx
    mov ax, 0x4F01
    mov di, VBE_MODE
    int 0x10
    cmp ax, 0x004F
    jne .next_mode
    mov ax, [VBE_MODE]          ; attributes: graphics + LFB
    and ax, 0x0090
    cmp ax, 0x0090
    jne .next_mode
    cmp byte [VBE_MODE+25], 32
    jne .next_mode
    mov ax, [VBE_MODE+18]       ; width
    mov bx, [VBE_MODE+20]       ; height
    cmp ax, PREF_W
    jne .not_pref
    cmp bx, PREF_H
    jne .not_pref
    mov cx, [cur_mode]
    mov [best_mode], cx
    jmp .scan_done
.not_pref:
    cmp ax, 1024                ; fallback: first 32bpp mode 1024..1280 wide
    jb .next_mode
    cmp ax, 1280
    ja .next_mode
    cmp word [best_mode], 0xFFFF
    jne .next_mode
    mov cx, [cur_mode]
    mov [best_mode], cx
    jmp .next_mode
.scan_done:
    mov cx, [best_mode]
    cmp cx, 0xFFFF
    je no_vbe
    mov ax, 0x4F01              ; reload info for the chosen mode
    mov di, VBE_MODE
    int 0x10
    mov eax, [VBE_MODE+40]
    mov [BOOTINFO+16], eax
    movzx eax, word [VBE_MODE+16]
    mov [BOOTINFO+20], eax
    movzx eax, word [VBE_MODE+18]
    mov [BOOTINFO+24], eax
    movzx eax, word [VBE_MODE+20]
    mov [BOOTINFO+28], eax
    movzx eax, byte [VBE_MODE+25]
    mov [BOOTINFO+32], eax
    mov si, msg_vbe
    call puts
    mov ax, 0x4F02
    mov bx, [best_mode]
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne no_vbe

    ; --- protected mode ---
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_entry

no_vbe:
    mov si, msg_novbe
    jmp die
disk_error:
    mov si, msg_disk
die:
    call puts
.hang:
    hlt
    jmp .hang

; enter unreal mode: DS/ES get 4 GiB limits, stay in real mode
unreal:
    cli
    push ds
    push es
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $+2
    mov bx, 0x10
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    pop es
    pop ds
    sti
    ret

serial_init:
    mov dx, 0x3F9
    xor al, al
    out dx, al                  ; no interrupts
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al                  ; DLAB
    mov dx, 0x3F8
    mov al, 1
    out dx, al                  ; 115200
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al                  ; 8N1
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    ret

; putc AL to screen (BIOS) and COM1
putc:
    push ax
    push bx
    push dx
    mov dx, 0x3F8
    out dx, al
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    pop dx
    pop bx
    pop ax
    ret

puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
    ret

msg_stage2: db 13, 10, "[stage2] loading", 13, 10, 0
msg_kernel: db "[stage2] kernel ", 0
msg_ok:     db " ok", 13, 10, 0
msg_vbe:    db "[stage2] vbe mode set, entering kernel", 13, 10, 0
msg_novbe:  db "[stage2] no usable 32bpp VBE mode", 13, 10, 0
msg_disk:   db "[stage2] disk read error", 13, 10, 0
msg_nommap: db "[stage2] no E820 memory map", 13, 10, 0

boot_drive: db 0
align 4
cur_lba:    dd KERNEL_LBA
remaining:  dd KERNEL_SECTORS
dest:       dd 0x100000
chunk:      dd 0
mode_ptr:   dd 0
cur_mode:   dw 0
best_mode:  dw 0xFFFF

align 4
dap:
            db 0x10, 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dq 0

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08 code, flat 32-bit
    dq 0x00CF92000000FFFF       ; 0x10 data, flat
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov ebx, BOOTINFO
    mov eax, 0x100000
    jmp eax
