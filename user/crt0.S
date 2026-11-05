/* SPDX-License-Identifier: MIT */
/* _start: kernel passes argc in rdi and argv in rsi, rsp is 16-byte aligned. */
.intel_syntax noprefix
.section .text.start, "ax"
.global _start
_start:
    xor ebp, ebp
    and rsp, -16
    call main                 /* int main(int argc, char** argv) */
    mov edi, eax
    xor eax, eax              /* SYS_EXIT */
    syscall
1:  hlt
    jmp 1b
