/* trampoline.S - real-mode -> long-mode start-up code for application processors.
 *
 * Linked at physical 0x8000 (see linker.ld: .ap_trampoline) and copied there by smp.cpp; a STARTUP IPI with
 * vector 0x08 makes the AP start executing at 0x8000 in real mode.  The BSP fills the parameter block at the
 * end (cr3, stack, entry point, per-CPU pointer) before sending the IPI.  Only assembled into a *used* path
 * when SMP=1; with SMP=0 the section is copied nowhere and never executed.
 */
.intel_syntax noprefix
.section .ap_trampoline, "ax"
.global ap_tramp_start
.global ap_tramp_end
.global ap_params

.code16
ap_tramp_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    lgdt [ap_gdt_ptr]                     /* absolute: we are linked at 0x8000 */
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    ljmp 0x08, offset ap_pm32             /* 32-bit code selector */

.code32
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)   /* PAE, OSFXSR, OSXMMEXCPT */
    mov cr4, eax
    mov eax, [ap_cr3]
    mov cr3, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)          /* LME + NXE (the page tables the BSP built may use NX) */
    wrmsr
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax
    ljmp 0x18, offset ap_lm64             /* 64-bit code selector */

.code64
ap_lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, [ap_stack]
    mov rdi, [ap_percpu]
    mov rax, [ap_entry]
    call rax                              /* ap_main(PerCpu*), never returns */
1:  hlt
    jmp 1b

.balign 8
ap_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF              /* 0x08: 32-bit code */
    .quad 0x00CF92000000FFFF              /* 0x10: data */
    .quad 0x00AF9A000000FFFF              /* 0x18: 64-bit code */
ap_gdt_end:
ap_gdt_ptr:
    .short ap_gdt_end - ap_gdt - 1
    .long ap_gdt
.balign 8
ap_params:
ap_cr3:     .quad 0
ap_stack:   .quad 0
ap_entry:   .quad 0
ap_percpu:  .quad 0
ap_tramp_end:
