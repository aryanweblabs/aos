/* SPDX-License-Identifier: MIT */
#include "vmm.hpp"
#include "../physical/pmm.h"

namespace aos::vmm {
bool is_canonical(uint64_t address) {
    const uint64_t top = address >> 48;
    return ((address >> 47) & 1) ? top == 0xffff : top == 0;
}

// The boot loader currently installs writable 2 MiB identity pages for 0..4 GiB.
bool is_boot_identity_mapped(uint64_t physical_address) {
    return physical_address < 0x100000000ULL;
}

namespace {
constexpr uint64_t present = 1, writable = 2, huge = 0x80;
uint64_t* table_at(uint64_t entry) { return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(entry & 0x000ffffffffff000ULL)); }
uint64_t g_kernel_cr3 = 0;   // the boot PML4: the master copy every address space inherits from
uint64_t* current_pml4() {
    if (g_kernel_cr3) return table_at(g_kernel_cr3);
    uint64_t root; asm volatile("mov %%cr3, %0" : "=r"(root)); return table_at(root);
}
uint64_t* child_table(uint64_t* parent, unsigned index, bool create) {
    if (!(parent[index] & present)) {
        if (!create) return nullptr;
        const uint64_t frame = pmm_alloc_frame();
        if (!frame) return nullptr;
        auto* table = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(frame));
        for (unsigned i = 0; i < 512; ++i) table[i] = 0;
        parent[index] = frame | present | writable;
    }
    if (parent[index] & huge) return nullptr;
    return table_at(parent[index]);
}
bool split_huge_page(uint64_t* directory, unsigned index) {
    const uint64_t old = directory[index];
    if (!(old & present) || !(old & huge)) return true;
    const uint64_t frame = pmm_alloc_frame();
    if (!frame) return false;
    auto* table = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(frame));
    const uint64_t base = old & 0x000fffffffe00000ULL;
    const uint64_t flags = old & 0xfffULL & ~huge;   // bit 12 of a 2 MiB entry is PAT, not a 4 KiB flag
    for (unsigned i = 0; i < 512; ++i) table[i] = (base + static_cast<uint64_t>(i) * page_size) | flags;
    directory[index] = frame | (old & 0xfffULL & ~huge) | present | writable;
    return true;
}
}

void init() {
    uint64_t root;
    asm volatile("mov %%cr3, %0" : "=r"(root));
    g_kernel_cr3 = root & 0x000ffffffffff000ULL;
}
uint64_t kernel_cr3() {
    if (!g_kernel_cr3) init();
    return g_kernel_cr3;
}

bool map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (!is_canonical(virtual_address) || (virtual_address & (page_size - 1)) ||
        (physical_address & (page_size - 1)) || physical_address >= 0x100000000ULL) return false;
    auto* pml4 = current_pml4();
    auto* pdpt = child_table(pml4, (virtual_address >> 39) & 511, true);
    if (!pdpt) return false;
    sync_kernel_slot((virtual_address >> 39) & 511);
    auto* pd = child_table(pdpt, (virtual_address >> 30) & 511, true);
    if (!pd) return false;
    const unsigned pdi = (virtual_address >> 21) & 511;
    if (!split_huge_page(pd, pdi)) return false;
    auto* pt = child_table(pd, pdi, true);
    if (!pt) return false;
    const unsigned pti = (virtual_address >> 12) & 511;
    pt[pti] = physical_address | (flags & 0xfffULL) | present;
    invalidate(virtual_address);
    return true;
}

bool unmap_page(uint64_t virtual_address) {
    if (!is_canonical(virtual_address) || (virtual_address & (page_size - 1))) return false;
    auto* pdpt = child_table(current_pml4(), (virtual_address >> 39) & 511, false);
    if (!pdpt) return false;
    auto* pd = child_table(pdpt, (virtual_address >> 30) & 511, false);
    if (!pd) return false;
    const unsigned pdi = (virtual_address >> 21) & 511;
    if (!split_huge_page(pd, pdi)) return false;
    auto* pt = child_table(pd, pdi, false);
    if (!pt) return false;
    const unsigned pti = (virtual_address >> 12) & 511;
    if (!(pt[pti] & present)) return false;
    pt[pti] = 0;
    invalidate(virtual_address);
    return true;
}

bool translate(uint64_t virtual_address, uint64_t* physical_address) {
    if (!physical_address || !is_canonical(virtual_address)) return false;
    auto* pdpt = child_table(current_pml4(), (virtual_address >> 39) & 511, false);
    if (!pdpt || !(pdpt[(virtual_address >> 30) & 511] & present)) return false;
    uint64_t entry = pdpt[(virtual_address >> 30) & 511];
    if (entry & huge) { *physical_address = (entry & 0x000fffffc0000000ULL) | (virtual_address & 0x3fffffffULL); return true; }
    auto* pd = table_at(entry);
    entry = pd[(virtual_address >> 21) & 511];
    if (!(entry & present)) return false;
    if (entry & huge) { *physical_address = (entry & 0x000fffffffe00000ULL) | (virtual_address & 0x1fffffULL); return true; }
    auto* pt = table_at(entry);
    entry = pt[(virtual_address >> 12) & 511];
    if (!(entry & present)) return false;
    *physical_address = (entry & 0x000ffffffffff000ULL) | (virtual_address & 0xfffULL);
    return true;
}

void invalidate(uint64_t virtual_address) {
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

namespace {
constexpr uint64_t pwt = 1ULL << 3, pcd = 1ULL << 4;

inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}
inline void wrmsr(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
bool enable_write_combining() {
    uint32_t a = 1, b, c, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 16))) return false;                    // no PAT
    uint64_t pat = rdmsr(0x277);
    pat = (pat & ~0xff00ULL) | 0x0100ULL;                   // PAT entry 1 (PWT only) -> WC
    wrmsr(0x277, pat);
    return true;
}
}

bool set_cache_mode(uint64_t phys, uint64_t length, CacheMode mode) {
    if (!length || phys + length > 0x100000000ULL) return false;
    uint64_t bits = pcd | pwt;                              // UC
    if (mode == CacheMode::WriteCombining) {
        if (!enable_write_combining()) return false;
        bits = pwt;
    }
    uint64_t* pdpt = child_table(current_pml4(), 0, false);
    if (!pdpt) return false;
    const uint64_t big = 1ULL << 21;
    for (uint64_t a = phys & ~(big - 1); a < phys + length; a += big) {
        uint64_t* pd = child_table(pdpt, (a >> 30) & 511, false);
        if (!pd) return false;
        const unsigned i = (a >> 21) & 511;
        if (!(pd[i] & present)) return false;
        if (pd[i] & huge) {
            pd[i] |= bits;
        } else {
            uint64_t* pt = table_at(pd[i]);
            for (unsigned k = 0; k < 512; ++k) {
                const uint64_t pa = a + k * page_size;
                if (pa + page_size > phys && pa < phys + length) pt[k] |= bits;
            }
        }
    }
    asm volatile("wbinvd; mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax", "memory");
    return true;
}
}

namespace aos::vmm {
bool map_mmio(uint64_t phys, uint64_t length, CacheMode mode) {
    if (!length || phys + length < phys || phys + length > (1ULL << 47)) return false;
    uint64_t bits = pcd | pwt;                              // UC
    if (mode == CacheMode::WriteCombining) {
        if (!enable_write_combining()) return false;
        bits = pwt;                                         // PAT index 1 = WC
    }
    const uint64_t big = 1ULL << 21;
    uint64_t* pml4 = current_pml4();
    for (uint64_t a = phys & ~(big - 1); a < phys + length; a += big) {
        uint64_t* pdpt = child_table(pml4, (a >> 39) & 511, true);
        if (!pdpt) return false;
        sync_kernel_slot((a >> 39) & 511);
        uint64_t* pd = child_table(pdpt, (a >> 30) & 511, true);
        if (!pd) return false;
        const unsigned i = (a >> 21) & 511;
        if (pd[i] & present) {
            if (pd[i] & huge) pd[i] = (pd[i] & ~(pcd | pwt)) | bits;
            else {
                uint64_t* pt = table_at(pd[i]);
                for (unsigned k = 0; k < 512; ++k) pt[k] = (pt[k] & ~(pcd | pwt)) | bits;
            }
        } else {
            pd[i] = a | present | writable | huge | bits;
        }
    }
    asm volatile("wbinvd; mov %%cr3, %%rax; mov %%rax, %%cr3" : : : "rax", "memory");
    return true;
}
}

extern "C" bool aos_vmm_map_mmio_uc(uint64_t physical_address, uint64_t length) {
    return aos::vmm::set_cache_mode(physical_address, length, aos::vmm::CacheMode::Uncached);
}
