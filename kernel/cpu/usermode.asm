BITS 32

SECTION .text

GLOBAL enter_user_mode

; void enter_user_mode(
;     unsigned entry,
;     unsigned user_stack,
;     unsigned kernel_stack_top
; );

enter_user_mode:
    cli

    ; Arguments are on the caller's stack.
    mov eax, [esp + 4]       ; User entry point
    mov ebx, [esp + 8]       ; User stack
    mov ecx, [esp + 12]      ; Kernel stack top

    ; Switch to this process's dedicated kernel stack.
    mov esp, ecx

    ; Set user data segments.
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    ; Construct the Ring 3 iret frame.
    push dword 0x23
    push ebx

    pushfd
    pop edx
    or edx, 0x200
    push edx

    push dword 0x1B
    push eax

    iretd

.hang:
    cli
    hlt
    jmp .hang
