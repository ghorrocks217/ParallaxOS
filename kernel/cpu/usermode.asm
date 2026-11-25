BITS 32

SECTION .text

GLOBAL enter_user_mode
GLOBAL leave_user_mode

EXTERN user_mode_saved_esp
EXTERN user_mode_saved_eflags

; void enter_user_mode(unsigned entry,
;                      unsigned user_stack,
;                      unsigned kernel_stack_top);

enter_user_mode:
    cli

    ; Preserve the callee-saved registers from the original kernel caller.
    push ebp
    push ebx
    push esi
    push edi

    ; Save the kernel stack so SYS_EXIT can return here.
    mov [user_mode_saved_esp], esp

    pushfd
    pop dword [user_mode_saved_eflags]

    mov eax, [esp + 20]       ; entry
    mov ebx, [esp + 24]       ; user stack
    mov ecx, [esp + 28]       ; kernel stack top

    mov esp, ecx

    ; Set user data segments.
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    ; Build the Ring 3 iret frame.
    push dword 0x23
    push ebx

    pushfd
    pop edx
    or edx, 0x200
    push edx

    push dword 0x1B
    push eax

    iretd


; Called by the kernel when a user process exits.
; Restores the original kernel caller of enter_user_mode.

leave_user_mode:
    cli

    ; Restore kernel data segments.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Restore the original kernel stack and callee-saved registers.
    mov esp, [user_mode_saved_esp]

    pop edi
    pop esi
    pop ebx
    pop ebp

    ; Restore the original interrupt state.
    push dword [user_mode_saved_eflags]
    popfd

    ret
