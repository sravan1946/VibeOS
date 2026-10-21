; VibeOS stage 1 (MBR). Loaded by the BIOS at 0x7C00.
; Loads stage 2 (STAGE2_SECTORS sectors from LBA 1) to 0x7E00 with INT 13h
; extensions and jumps to it with the boot drive in DL.
; Bytes 446..509 hold the partition table: entry 0 is the ext2 root.

BITS 16
ORG 0x7C00

start:
    jmp 0x0000:.norm            ; normalise CS:IP
.norm:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    mov si, msg_hello
    call puts

    mov ah, 0x41                ; extensions present?
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc fail
    cmp bx, 0xAA55
    jne fail

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc fail

    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

fail:
    mov si, msg_fail
    call puts
.hang:
    hlt
    jmp .hang

puts:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp puts
.done:
    ret

boot_drive: db 0
msg_hello:  db "VibeOS", 0
msg_fail:   db " - disk error", 0

align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0x7E00, 0x0000
    dq 1

times 446-($-$$) db 0
; partition 0: Linux (ext2) root
    db 0x80, 0xFE, 0xFF, 0xFF, 0x83, 0xFE, 0xFF, 0xFF
    dd FS_LBA
    dd FS_SECTORS
times 510-($-$$) db 0
dw 0xAA55
