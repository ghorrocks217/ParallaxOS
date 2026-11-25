ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text   : { *(.text*) }          /* entry.o is first on the command line, so _start is at 0x10000 */
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    __bss_start = .;
    .bss    : { *(.bss*) *(COMMON) }
    _end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
