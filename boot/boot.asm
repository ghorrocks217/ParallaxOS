[org 0x7c00]
[bits 16]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00          ; stack grows down from just below the bootloader
    mov [boot_drive], dl    ; BIOS puts the boot drive number in DL

    mov si, msg_loading
    call print

    mov ah, 0x02            ; BIOS read sectors
    mov al, 4               ; 4 sectors = 2048 bytes (stage 2)
    mov ch, 0               ; cylinder 0
    mov cl, 2               ; start at sector 2 (sector 1 is this bootloader)
    mov dh, 0               ; head 0
    mov dl, [boot_drive]
    xor bx, bx              ; ES is already 0, BX=0 → ES:BX = 0x0000:0x7E00
    mov es, bx              ; make ES explicit (some BIOSes are picky)
    mov bx, 0x7e00
    int 0x13
    jc disk_error

    mov dl, [boot_drive]    ; pass the boot drive to stage 2
    jmp 0x0000:0x7e00

disk_error:
    mov si, msg_err
    call print
    jmp $

print:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp print
.done:
    ret

boot_drive  db 0
msg_loading db "Loading stage 2...", 0
msg_err     db "Disk read error!", 0

times 510-($-$$) db 0
dw 0xAA55