/* context.S - kernel thread context switch.
 *
 * void aos_context_switch(Context* old, Context* new)
 *   Context = { uint64 rsp; uint64 pad; uint8 fx[512] }   (fx is 16-byte aligned)
 * Saves x87/SSE, RFLAGS and the callee-saved registers of the old thread on its own stack, then restores
 * those of the new one.  RFLAGS is part of the context so a thread preempted inside an interrupt resumes
 * with interrupts masked and one that yielded voluntarily resumes with them enabled.
 * A fresh thread's stack is laid out so the final `ret` lands in aos_task_bootstrap.
 */
.intel_syntax noprefix
.section .text
.code64
.global aos_context_switch
aos_context_switch:
    fxsave64 [rdi + 16]
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, [rsi]
    fxrstor64 [rsi + 16]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret
