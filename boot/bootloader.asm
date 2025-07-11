; boot/bootloader.asm
; -------------------
; This is a simple bootloader that loads a C kernel from disk into memory and jumps to it in 32-bit protected mode.
; It is loaded by the BIOS at address 0x7C00 in 16-bit real mode.
; The bootloader loads 16 sectors (8 KiB) from disk (starting at sector 2) to address 0x1000:0x0000 (physical 0x10000).
; After loading, it sets up a minimal GDT, switches to protected mode, and far jumps to the kernel entry point.
; If disk loading fails, it prints an error message.
; The last two bytes are the boot signature (0xAA55), required for BIOS boot.

BITS 16
ORG 0x7C00

mov si, boot_msg
call print_string

; Set VGA to 80x25 mode
mov ah, 0x00
mov al, 0x03
int 0x10

mov [BOOT_DRIVE], dl    ; Store BIOS boot drive number for later disk reads

; Set ES:BX to 0x1000:0x0000 (destination address for kernel)
mov ax, 0x1000
mov es, ax
xor bx, bx        ; ES:BX = 0x1000:0x0000

; Set up disk read parameters
mov ah, 0x02      ; INT 13h: Read sectors
mov al, 128       ; Number of sectors to read (128 * 512 = 65536 bytes)
mov ch, 0         ; Cylinder 0
mov cl, 2         ; Sector 2 (sector 1 is the bootloader itself)
mov dh, 0         ; Head 0
mov dl, [BOOT_DRIVE] ; Boot drive (set by BIOS)
int 0x13          ; BIOS disk read
jc disk_error     ; Jump if carry flag set (error)

; Write 'B' to VGA to show we finished loading kernel
mov ax, 0x0F42
mov [0xB8000], ax

; Set up a minimal GDT for protected mode
cli
lgdt [gdt_descriptor]

; Set PE bit in CR0 to enable protected mode
mov eax, cr0
or eax, 1
mov cr0, eax

; Write 'P' to VGA to show we entered protected mode
mov ax, 0x0F50
mov [0xB8002], ax

; Far jump to 32-bit code segment at 0x10000 (kernel entry)
jmp 0x08:protected_mode_entry

; 32-bit code starts here
[BITS 32]
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x80000      ; Set up a stack (must be immediately after mov ss, ax)
    ; Write 'E' to VGA to show we are about to jump to kernel
    mov dword [0xB8004], 0x0F450F45 ; 'E' in two cells
    jmp 0x08:0x10000            ; Far jump to kernel entry point (physical 0x10000)

; 16-bit real mode error handler
[BITS 16]
disk_error:
    mov si, disk_error_msg
    call print_string
    jmp $

; Print a null-terminated string at DS:SI using BIOS teletype
print_string:
    mov ah, 0x0E
.next_char:
    lodsb
    cmp al, 0
    je .done
    int 0x10
    jmp .next_char
.done:
    ret

disk_error_msg db 'Disk read error!', 0

; The BIOS sets the boot drive in DL, so we store it for later use
BOOT_DRIVE db 0

align 8
; Minimal GDT: null, code, data
GDT:
    dq 0x0000000000000000     ; Null descriptor
    dq 0x00cf9a000000ffff     ; Code segment: base=0, limit=4GB, 32-bit, RX
    dq 0x00cf92000000ffff     ; Data segment: base=0, limit=4GB, 32-bit, RW

gdt_descriptor:
    dw (GDT_end - GDT - 1)
    dd GDT
GDT_end:

; Pad to 510 bytes, then add the boot signature (0xAA55)
times 510-($-$$) db 0
dw 0xAA55 

boot_msg db 'Bootloader start', 0 