[bits 32]
[extern kmain]
[extern isr_handler]
[extern __bss_start]            ; provided by ld's default script
[extern _end]
global _start
global idt_flush
global isr_table
global switch_context

section .text

_start:
    cld                         ; C ABI: DF must be 0
    mov edi, __bss_start        ; the flat binary doesn't contain .bss: zero it
    mov ecx, _end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call kmain
    cli
.hang:
    hlt
    jmp .hang

idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; void switch_context(unsigned *save_old_esp, unsigned new_esp)
switch_context:
    mov eax, [esp+4]
    mov edx, [esp+8]
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
    ret

%macro ISR_NOERR 1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

isr_common:
    pusha
    cld
    push esp
    call isr_handler
    add esp, 4
    popa
    add esp, 8
    iret

section .rodata
isr_table:
%assign i 0
%rep 48
    dd isr %+ i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits