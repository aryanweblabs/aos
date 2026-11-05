/* SPDX-License-Identifier: MIT */
// frame.hpp - register frame pushed by interrupts.S (identical for interrupts, exceptions and SYSCALL).
#pragma once
#include <stdint.h>

struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;     // pushed by the CPU (rsp/ss are always present in 64-bit mode)
};
static_assert(sizeof(InterruptFrame) == 22 * 8, "frame layout is shared with interrupts.S");
