[org 0x7e00]
[bits 16]

KERNEL_SEG     equ 0x1000       ; loaded at 0x1000:0000 = 0x10000
KERNEL_LBA     equ 5            ; LBA 0 = stage1, 1-4 = stage2, 5.. = kernel
KERNEL_SECTORS equ 128          ; 32 KB: raise if kernel.bin grows

stage2_start:
    cli
    xor ax, ax                  ; don't trust stage1's segments
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl        ; dl still intact

    mov si, msg_stage2
    call print16

    ; ---- Enable A20 ----
    call check_a20
    test ax, ax
    jnz .a20_ok

    mov ax, 0x2401              ; method 1: BIOS
    int 0x15
    call check_a20
    test ax, ax
    jnz .a20_ok

    in al, 0x92                 ; method 2: fast A20
    or al, 2
    and al, 0xFE                ; bit 0 = reset, never set it
    out 0x92, al
    call check_a20
    test ax, ax
    jnz .a20_ok

    call a20_kbc                ; method 3: keyboard controller
    call check_a20
    test ax, ax
    jnz .a20_ok

    mov si, msg_a20_fail
    call print16
    jmp $

.a20_ok:
    mov si, msg_a20_ok
    call print16

    call load_kernel            ; jumps to disk_fail on error
    call detect_memory          ; needs the BIOS, so before PM

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword CODE_SEG:pm_start

; ---------- 16-bit helpers ----------
print16:
    push bx
    mov bx, 0x0007              ; page 0, light grey
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp .loop
.done:
    pop bx
    ret

disk_fail:
    mov si, msg_disk_fail
    call print16
    jmp $

; Reads KERNEL_SECTORS sectors one at a time to KERNEL_SEG:0.
; One sector per call = no track / 64 KB boundary problems.
load_kernel:
    mov word [left], KERNEL_SECTORS

    mov ah, 0x41                ; do we have INT 13h extensions?
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .chs
    cmp bx, 0xAA55
    jne .chs
    test cx, 1
    jz .chs

    ; ---- LBA path ----
    mov word [dap_seg], KERNEL_SEG
    mov dword [dap_lba], KERNEL_LBA
.lba_loop:
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_fail
    add word [dap_seg], 0x20    ; 512 bytes = 0x20 paragraphs
    inc dword [dap_lba]
    dec word [left]
    jnz .lba_loop
    jmp .done

    ; ---- CHS fallback (old BIOS / floppy) ----
.chs:
    mov ah, 0x08                ; drive geometry
    mov dl, [boot_drive]
    xor di, di
    int 0x13
    jc disk_fail
    mov al, cl
    and al, 0x3F
    xor ah, ah
    mov [spt], ax               ; sectors per track
    xor ax, ax
    mov al, dh
    inc ax
    mov [heads], ax
    mov word [cur_lba], KERNEL_LBA
    mov word [dest_seg], KERNEL_SEG
.chs_loop:
    mov ax, [cur_lba]
    xor dx, dx
    div word [spt]              ; ax = lba / spt, dx = lba % spt
    mov cl, dl
    inc cl                      ; 1-based sector
    xor dx, dx
    div word [heads]            ; ax = cylinder, dx = head
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah                   ; cylinder bits 8-9
    mov dl, [boot_drive]
    mov bx, [dest_seg]
    mov es, bx
    xor bx, bx
    mov ax, 0x0201
    int 0x13
    jc disk_fail
    add word [dest_seg], 0x20
    inc word [cur_lba]
    dec word [left]
    jnz .chs_loop
.done:
    xor ax, ax
    mov es, ax
    ret

; Stores the BIOS memory map: count at 0x1000, 24-byte entries from 0x1010
detect_memory:
    push es
    push di
    xor ax, ax
    mov es, ax                  ; explicit, so we don't depend on DS
    mov di, 0x1010
    xor ebx, ebx
    mov dword [es:0x1000], 0
.next:
    mov eax, 0xE820
    mov edx, 0x534D4150         ; 'SMAP'
    mov ecx, 24
    mov dword [es:di+20], 1     ; ACPI field defaults to "valid"
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    inc dword [es:0x1000]
    add di, 24
    cmp dword [es:0x1000], 32
    jae .done
    test ebx, ebx
    jnz .next
.done:
    pop di
    pop es
    ret

; Enable A20 through the keyboard controller
a20_kbc:
    call .wait_in
    mov al, 0xAD                ; disable keyboard
    out 0x64, al
    call .wait_in
    mov al, 0xD0                ; read output port
    out 0x64, al
    call .wait_out
    in al, 0x60
    push ax
    call .wait_in
    mov al, 0xD1                ; write output port
    out 0x64, al
    call .wait_in
    pop ax
    or al, 2                    ; A20 bit
    out 0x60, al
    call .wait_in
    mov al, 0xAE                ; re-enable keyboard
    out 0x64, al
    call .wait_in
    ret
.wait_in:
    in al, 0x64
    test al, 2
    jnz .wait_in
    ret
.wait_out:
    in al, 0x64
    test al, 1
    jz .wait_out
    ret

; Returns AX=1 if A20 is enabled, 0 if not.
check_a20:
    pushf
    push ds
    push es
    push di
    push si

    xor ax, ax
    mov es, ax
    not ax
    mov ds, ax
    mov di, 0x0500
    mov si, 0x0510

    mov al, [es:di]
    push ax
    mov al, [ds:si]
    push ax

    mov byte [es:di], 0x00
    mov byte [ds:si], 0xFF
    cmp byte [es:di], 0xFF      ; changed = memory wrapped = A20 OFF

    pop ax
    mov [ds:si], al
    pop ax
    mov [es:di], al

    mov ax, 0
    je .done
    mov ax, 1
.done:
    pop si
    pop di
    pop es
    pop ds
    popf
    ret

; ---------- GDT ----------
gdt_start:
    dq 0x0
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ---------- 32-bit code ----------
[bits 32]
pm_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, 0x10000
    jmp eax
.halt:
    cli
    hlt
    jmp .halt

; ---------- data ----------
boot_drive    db 0
left          dw 0
cur_lba       dw 0
dest_seg      dw 0
spt           dw 0
heads         dw 0

dap:                            ; INT 13h AH=42h disk address packet
    db 0x10, 0
    dw 1                        ; one sector per call
dap_off dw 0
dap_seg dw 0
dap_lba dd 0, 0

msg_stage2    db 13, 10, "Stage 2 loaded.", 13, 10, 0
msg_a20_ok    db "A20 enabled.", 13, 10, 0
msg_a20_fail  db "A20 failed!", 13, 10, 0
msg_disk_fail db "Kernel load failed!", 13, 10, 0

times 2048-($-$$) db 0
