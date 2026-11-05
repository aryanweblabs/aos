/* SPDX-License-Identifier: MIT */
// gdt.cpp - per-CPU GDT/TSS, GS base, SYSCALL MSRs.
#include "percpu.hpp"
#include "../../memory/physical/pmm.h"

extern "C" void aos_serial_write(const char* text);

namespace aos::arch {
namespace {
constexpr uint32_t MSR_EFER = 0xC0000080, MSR_STAR = 0xC0000081, MSR_LSTAR = 0xC0000082, MSR_SFMASK = 0xC0000084;
constexpr uint32_t MSR_GS_BASE = 0xC0000101, MSR_KERNEL_GS_BASE = 0xC0000102;
constexpr uint32_t kMaxCpus = 32;

PerCpu  g_bsp;
PerCpu* g_cpus[kMaxCpus];
uint32_t g_count = 0;

inline void wrmsr(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" : : "c"(msr), "a"(static_cast<uint32_t>(v)), "d"(static_cast<uint32_t>(v >> 32)) : "memory");
}
inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

uint64_t alloc_ist_stack() {
    const uint64_t p = pmm_alloc_contiguous(2);
    return p ? p + 2 * 4096 - 16 : 0;
}

void build_gdt(PerCpu* c) {
    c->gdt[0] = 0;
    c->gdt[1] = 0x00AF9A000000FFFFULL;   // 0x08 kernel code (64-bit)
    c->gdt[2] = 0x00CF92000000FFFFULL;   // 0x10 kernel data
    c->gdt[3] = 0x00CFFA000000FFFFULL;   // 0x18 user code (32-bit, only the SYSRET base)
    c->gdt[4] = 0x00CFF2000000FFFFULL;   // 0x20 user data
    c->gdt[5] = 0x00AFFA000000FFFFULL;   // 0x28 user code (64-bit)
    const uint64_t base = reinterpret_cast<uint64_t>(&c->tss);
    const uint64_t limit = sizeof(Tss) - 1;
    c->gdt[6] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
                (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    c->gdt[7] = base >> 32;
}

struct __attribute__((packed)) DescPtr { uint16_t limit; uint64_t base; };

void load_cpu(PerCpu* c) {
    build_gdt(c);
    DescPtr gp{static_cast<uint16_t>(sizeof(c->gdt) - 1), reinterpret_cast<uint64_t>(c->gdt)};
    asm volatile("lgdt %0" : : "m"(gp) : "memory");
    // Reload CS through a far return, then the data segments.
    asm volatile(
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        : : [cs] "i"(static_cast<uint64_t>(SEL_KCODE)), [ds] "i"(SEL_KDATA) : "rax", "memory");
    asm volatile("ltr %0" : : "r"(static_cast<uint16_t>(SEL_TSS)));
    wrmsr(MSR_GS_BASE, reinterpret_cast<uint64_t>(c));   // after the gs selector load, which clears the base
    wrmsr(MSR_KERNEL_GS_BASE, 0);                        // user GS
}

void setup_tss(PerCpu* c) {
    c->tss = {};
    c->tss.iomap_base = sizeof(Tss);                      // no I/O bitmap: ring 3 gets #GP on in/out
    c->tss.ist[0] = alloc_ist_stack();                    // IST1: double fault
    c->tss.ist[1] = alloc_ist_stack();                    // IST2: NMI
    c->tss.ist[2] = alloc_ist_stack();                    // IST3: machine check
}
}  // namespace

PerCpu* cpu_by_id(uint32_t id) { return id < g_count ? g_cpus[id] : nullptr; }
uint32_t cpu_count() { return g_count; }

bool init_bsp() {
    g_bsp = {};
    g_bsp.self = &g_bsp;
    g_bsp.cpu_id = 0;
    g_bsp.online = 1;
    setup_tss(&g_bsp);
    g_cpus[0] = &g_bsp;
    g_count = 1;
    load_cpu(&g_bsp);
    init_cpu_features();
    return g_bsp.tss.ist[0] != 0;
}

PerCpu* alloc_percpu(uint32_t cpu_id, uint32_t lapic_id) {
    if (g_count >= kMaxCpus) return nullptr;
    const uint64_t frames = (sizeof(PerCpu) + 4095) / 4096;
    const uint64_t p = pmm_alloc_contiguous(frames);
    if (!p) return nullptr;
    auto* c = reinterpret_cast<PerCpu*>(p);
    *c = {};
    c->self = c;
    c->cpu_id = cpu_id;
    c->lapic_id = lapic_id;
    setup_tss(c);
    g_cpus[g_count++] = c;     // the id must equal the slot, APs are brought up in order
    return c;
}

void init_cpu(PerCpu* c) {
    load_cpu(c);
    init_cpu_features();
    c->online = 1;
}

void init_cpu_features() {
    uint32_t a, b, c, d;
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1;                                  // SCE: SYSCALL/SYSRET
    if (d & (1u << 20)) efer |= (1ULL << 11);   // NXE
    wrmsr(MSR_EFER, efer);

    wrmsr(MSR_STAR, (static_cast<uint64_t>(0x18) << 48) | (static_cast<uint64_t>(SEL_KCODE) << 32));
    wrmsr(MSR_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x100 | 0x40000 | 0x4000);   // IF DF TF AC NT cleared on entry

    cpuid(7, 0, &a, &b, &c, &d);
    if (b & (1u << 7)) {                        // SMEP: the kernel may never execute user pages
        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 20);
        asm volatile("mov %0, %%cr4" : : "r"(cr4));
    }
}

}  // namespace aos::arch
