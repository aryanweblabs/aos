/* SPDX-License-Identifier: MIT */
/* entry.S - Multiboot2 entry, 32->64 bit transition (GNU as, Intel syntax; no nasm needed).
 *
 * Everything in .boot.* is linked at its physical address.  The rest of the kernel is linked either at
 * 1 MiB (HH=0) or in the higher half at 0xFFFFFFFF80100000 (HH=1); in both cases we jump to it through
 * an absolute 64-bit address after paging is on.
 */
.intel_syntax noprefix

.section .multiboot, "a"
.balign 8
header_start:
    .long 0xe85250d6
    .long 0
    .long header_end - header_start
    .long (0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))) & 0xffffffff
    /* information request: memory map (6), modules (3) */
    .balign 8
    .short 1
    .short 0
    .long 16
    .long 6
    .long 0
    .balign 8
    .short 1
    .short 0
    .long 16
    .long 3
    .long 0
    /* framebuffer request (optional): 1920x1080x32. GRUB falls back to its "auto" mode if the firmware does not offer it.
       Change width/height here for another size (0,0 = no preference). */
    .balign 8
    .short 5
    .short 1
    .long 20
    .long 1920
    .long 1080
    .long 32
    /* end tag */
    .balign 8
    .short 0
    .short 0
    .long 8
header_end:

.section .boot.text, "ax"
.code32
.balign 16
.global _start
_start:
    cli
    mov [boot_magic], eax
    mov [boot_info], ebx
    mov esp, offset stack_top

    /* zero the 7 boot page tables (pml4, pdpt, pd0..3, pdpt_high) */
    mov edi, offset pml4_table
    xor eax, eax
    mov ecx, (4096 * 7) / 4
    rep stosd

    mov eax, offset pdpt_table
    or eax, 3
    mov [pml4_table], eax                    /* PML4[0]   -> identity PDPT      */
    mov eax, offset pdpt_high
    or eax, 3
    mov [pml4_table + 511 * 8], eax          /* PML4[511] -> higher-half PDPT   */

    mov edi, offset pdpt_table
    mov eax, offset page_directory_0
    or eax, 3
    mov [edi], eax
    mov eax, offset page_directory_1
    or eax, 3
    mov [edi + 8], eax
    mov eax, offset page_directory_2
    or eax, 3
    mov [edi + 16], eax
    mov eax, offset page_directory_3
    or eax, 3
    mov [edi + 24], eax

    /* 0xFFFFFFFF80000000 + x  ->  physical x   (first GiB, shares PD0 with the identity map) */
    mov eax, offset page_directory_0
    or eax, 3
    mov [pdpt_high + 510 * 8], eax

    /* 2 MiB pages covering 0..4 GiB */
    mov edi, offset page_directory_0
    mov eax, 0x83
    mov ecx, 2048
1:  mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop 1b

    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)  /* PAE, OSFXSR, OSXMMEXCPT */
    mov cr4, eax

    mov eax, offset pml4_table
    mov cr3, eax

    mov ecx, 0xC0000080                      /* EFER.LME */
    rdmsr
    or eax, (1 << 8)
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)                        /* paging on */
    mov cr0, eax

    lgdt [gdt64_pointer]
    jmp 0x08:long_mode_start

.code64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov rax, cr0                             /* enable SSE */
    and rax, ~(1 << 2)
    or rax, (1 << 1)
    mov cr0, rax
    fninit

    mov esp, offset stack_top
    and rsp, -16

    mov dx, 0x3F8                            /* early serial marker */
    mov al, 'A'
    out dx, al

    mov edi, DWORD PTR [boot_magic]          /* low addresses: absolute, still on the identity map */
    mov esi, DWORD PTR [boot_info]
    movabs rax, offset hh_entry
    jmp rax

.section .text
.code64
hh_entry:
    call kernel_main
1:  hlt
    jmp 1b

.section .boot.data, "aw"
.balign 8
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt64_pointer:
    .short gdt64_pointer - gdt64 - 1
    .long gdt64
.global boot_magic
boot_magic: .long 0
.global boot_info
boot_info:  .long 0

.section .boot.bss, "aw", @nobits
.balign 4096
.global boot_pml4_table
pml4_table:
boot_pml4_table:   .skip 4096
pdpt_table:        .skip 4096
page_directory_0:  .skip 4096
page_directory_1:  .skip 4096
page_directory_2:  .skip 4096
page_directory_3:  .skip 4096
pdpt_high:         .skip 4096
.balign 16
stack_bottom:      .skip 65536
.global stack_top
stack_top:
