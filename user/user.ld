/* Linker script for AOS user programs: static ET_EXEC at the bottom of user space (PML4 slot 1).
   The kernel loader rejects anything below 0x8000000000 and any two segments that share a page. */
ENTRY(_start)
SECTIONS
{
    . = 0x8000000000;
    .text   : ALIGN(4096) { *(.text.start) *(.text*) }
    .rodata : ALIGN(4096) { *(.rodata*) }
    .data   : ALIGN(4096) { *(.data*) }
    .bss    : ALIGN(4096) { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
