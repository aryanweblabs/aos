/* SPDX-License-Identifier: MIT */
// percpu.hpp - per-CPU control block, GDT and TSS.
//
// GS.base points at the PerCpu of the executing CPU while in ring 0 (user GS lives in KERNEL_GS_BASE and is
// swapped in with swapgs).  The first fields are accessed from assembly by fixed offset - keep them in sync
// with interrupts.S (8 = kernel_rsp, 16 = user_rsp).
#pragma once
#include "frame.hpp"
#include <stddef.h>
#include <stdint.h>

namespace aos::arch {

struct __attribute__((packed)) Tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
static_assert(sizeof(Tss) == 104, "TSS must be 104 bytes");

// Segment selectors (the order is dictated by SYSCALL/SYSRET: STAR[63:48] = 0x18).
constexpr uint16_t SEL_KCODE = 0x08;
constexpr uint16_t SEL_KDATA = 0x10;
constexpr uint16_t SEL_UCODE32 = 0x18;
constexpr uint16_t SEL_UDATA = 0x20 | 3;
constexpr uint16_t SEL_UCODE = 0x28 | 3;
constexpr uint16_t SEL_TSS = 0x30;

struct alignas(64) PerCpu {
    PerCpu*  self;           //  0
    uint64_t kernel_rsp;     //  8  top of the running task's kernel stack (SYSCALL entry loads it)
    uint64_t user_rsp;       // 16  scratch used by the SYSCALL entry stub
    uint32_t cpu_id;         // 24  0 = bootstrap processor
    uint32_t lapic_id;       // 28
    void*    current;        // 32  scheduler::Task*
    void*    idle;           // 40  scheduler::Task* (this CPU's idle thread)
    void*    switch_prev;    // 48  task we just switched away from (for finish_switch)
    uint32_t preempt_count;  // 56
    uint32_t need_resched;   // 60
    volatile uint32_t online;// 64
    uint32_t timer_count;    // 68  LAPIC timer initial count for the scheduler tick
    uint64_t irq_count;      // 72
    Tss      tss;
    alignas(8) uint64_t gdt[8];
};
static_assert(offsetof(PerCpu, kernel_rsp) == 8 && offsetof(PerCpu, user_rsp) == 16, "asm offsets");

inline PerCpu* percpu() {
    PerCpu* p;
    asm volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

// Allocate-free setup of the boot CPU (uses a static PerCpu).  Needs the PMM (IST stacks).
bool init_bsp();
// Application processors call this with a PerCpu allocated by the BSP.
void init_cpu(PerCpu* cpu);
// Enable SYSCALL/SYSRET, NX and SMEP on the calling CPU.
void init_cpu_features();
// Kernel stack used when this CPU next enters ring 0 from ring 3.
inline void set_kernel_stack(uint64_t top) {
    PerCpu* c = percpu();
    c->kernel_rsp = top;
    c->tss.rsp[0] = top;
}
PerCpu* cpu_by_id(uint32_t id);
uint32_t cpu_count();
PerCpu* alloc_percpu(uint32_t cpu_id, uint32_t lapic_id);   // for AP bring-up

}  // namespace aos::arch

extern "C" void syscall_entry();
extern "C" [[noreturn]] void aos_enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1);
extern "C" [[noreturn]] void aos_fork_return(InterruptFrame* frame);
