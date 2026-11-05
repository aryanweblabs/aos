/* SPDX-License-Identifier: MIT */
/* interrupts.S - ISR stubs, common interrupt path, SYSCALL entry and the ring-3 launcher.
 *
 * Frame layout built on the kernel stack (lowest address first), mirrored by InterruptFrame in C++:
 *   r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rdx rcx rbx rax | vector error | rip cs rflags rsp ss
 *
 * Invariant: while the CPU runs ring-0 code, GS.base == this CPU's PerCpu block (see percpu.hpp).
 * Entering from ring 3 therefore does swapgs, and so does every return to ring 3.
 * PerCpu offsets used here: 8 = kernel_rsp, 16 = user_rsp.
 */
.intel_syntax noprefix
.altmacro

.section .text
.code64
.extern interrupt_dispatch

.macro ISR_NOERR n
.global isr\n
isr\n:
    push 0
    push \n
    jmp interrupt_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    push \n
    jmp interrupt_common
.endm

.set vec, 0
.rept 256
    .if (vec == 8) || (vec >= 10 && vec <= 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
        ISR_ERR %vec
    .else
        ISR_NOERR %vec
    .endif
    .set vec, vec + 1
.endr

.global interrupt_common
interrupt_common:
    test BYTE PTR [rsp + 24], 3          /* saved CS: coming from ring 3? */
    jz 1f
    swapgs
1:
.global interrupt_common_noswap
interrupt_common_noswap:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                         /* InterruptFrame* */
    mov r12, rsp                         /* callee-saved: survives context switches inside dispatch */
    sub rsp, 512                         /* FPU/SSE state of the interrupted code, so C++ may use SSE freely */
    and rsp, -16
    fxsave64 [rsp]
    call interrupt_dispatch
    fxrstor64 [rsp]
    mov rsp, r12

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                          /* vector + error code */
    test BYTE PTR [rsp + 8], 3           /* returning to ring 3? */
    jz 2f
    swapgs
2:  iretq

/* SYSCALL entry (IA32_LSTAR).  Builds the same frame as an interrupt so the C++ side is shared and the
 * return path is always iretq (immune to the non-canonical-RCX SYSRET hazard).  IF is masked by SFMASK. */
.global syscall_entry
syscall_entry:
    swapgs
    mov gs:[16], rsp                     /* stash user rsp */
    mov rsp, gs:[8]                      /* kernel stack of the current task */
    push 0x23                            /* ss  = user data  | 3 */
    push QWORD PTR gs:[16]               /* user rsp */
    push r11                             /* rflags (saved by SYSCALL) */
    push 0x2B                            /* cs  = user code64 | 3 */
    push rcx                             /* rip (saved by SYSCALL) */
    push 0                               /* error code */
    push 0x80                            /* vector */
    jmp interrupt_common_noswap

/* void aos_enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1) - never returns. */
.global aos_enter_user
aos_enter_user:
    cli
    push 0x23
    push rsi
    push 0x202
    push 0x2B
    push rdi
    mov rdi, rdx
    mov rsi, rcx
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    swapgs
    iretq

/* void aos_fork_return(InterruptFrame* f) - restore a full user frame and iretq (for fork child). */
.global aos_fork_return
aos_fork_return:
    cli
    mov rsp, rdi                         /* frame pointer becomes our stack */
    /* stack layout matches InterruptFrame: r15..rax, vector, error, rip, cs, rflags, rsp, ss */
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                          /* skip vector + error */
    /* now: rip cs rflags rsp ss */
    test BYTE PTR [rsp + 8], 3           /* returning to ring 3? */
    jz 1f
    swapgs
1:  iretq


.section .rodata
.balign 8
.global isr_stub_table
.macro ISR_ENTRY n
    .quad isr\n
.endm
isr_stub_table:
.set vec, 0
.rept 256
    ISR_ENTRY %vec
    .set vec, vec + 1
.endr
