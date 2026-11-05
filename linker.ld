/* AOS linker script.
 *
 *   ld --defsym=KOFF=0                    -> whole kernel at 1 MiB (identity mapped)         [HH=0, default]
 *   ld --defsym=KOFF=0xFFFFFFFF80000000   -> kernel proper in the higher half                [HH=1]
 *
 * .boot.* (entry code, boot page tables, boot stack) is ALWAYS linked at its physical address because it
 * runs before / while paging is being set up.  Everything after it gets VMA = LMA + KOFF.
 * The Multiboot2 header must be in the first 32 KiB of the file, so it comes first.
 */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .boot ALIGN(8) : {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
    }
    .boot.bss ALIGN(4K) (NOLOAD) : {
        *(.boot.bss)
    }

    /* AP start-up code: executed at physical 0x8000, stored here and copied at run time (SMP=1). */
    .ap_trampoline 0x8000 : AT(ALIGN(LOADADDR(.boot.bss) + SIZEOF(.boot.bss), 4K)) {
        KEEP(*(.ap_trampoline))
    }
    __ap_tramp_load = LOADADDR(.ap_trampoline);
    __ap_tramp_size = SIZEOF(.ap_trampoline);

    . = ALIGN(LOADADDR(.ap_trampoline) + SIZEOF(.ap_trampoline), 4K) + KOFF;

    .text ALIGN(4K) : AT(ADDR(.text) - KOFF) {
        *(.text*)
    }
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KOFF) {
        *(.rodata*)
    }
    .data ALIGN(4K) : AT(ADDR(.data) - KOFF) {
        *(.data*)
    }
    .bss ALIGN(4K) : AT(ADDR(.bss) - KOFF) {
        *(COMMON)
        *(.bss*)
    }

    _kernel_start      = ADDR(.text);
    _kernel_end        = .;
    _kernel_phys_start = 1M;
    _kernel_phys_end   = . - KOFF;

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
