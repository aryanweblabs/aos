/* SPDX-License-Identifier: MIT */
// aspace.hpp - per-process address spaces with demand paging and COW.
//
// Layout (all 4-level, identical kernel half in every space):
//   PML4[0]      identity map of 0..4 GiB, supervisor only (drivers, DMA, the kernel image with HH=0)
//   PML4[1..255] user space, 0x0000_0080_0000_0000 .. 0x0000_7FFF_FFFF_FFFF, private per process
//   PML4[256..]  kernel (higher-half image with HH=1, MMIO windows above 512 GiB)
// User programs are therefore linked at/above kUserBase (see user/user.ld).
//
// Virtual memory model (see docs/MEMORY.md):
//   - Anonymous regions are installed as demand-zero (no physical frame until first touch).
//   - fork() shares pages and marks them copy-on-write; a write fault creates a private copy.
//   - Every valid user mapping belongs to a VMA; faults outside any VMA kill the process.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::vmm {

constexpr uint64_t kUserBase = 0x0000008000000000ULL;
constexpr uint64_t kUserEnd  = 0x0000800000000000ULL;
constexpr uint64_t PF_PRESENT = 1, PF_WRITE = 2, PF_USER = 4, PF_NX = 1ULL << 63;
// Software bits (x86 available bits 9-11)
constexpr uint64_t PF_COW    = 1ULL << 9;   // shared; must copy on write
constexpr uint64_t PF_DEMAND = 1ULL << 10;  // not present yet; allocate on fault
constexpr uint64_t kPageSize = 4096;

// VMA flags
constexpr uint32_t VMA_ANON  = 1;           // anonymous (demand-zero)

struct Vma {
    uint64_t start = 0;     // inclusive, page-aligned
    uint64_t end   = 0;     // exclusive, page-aligned
    uint64_t prot  = 0;     // PF_WRITE | PF_NX (read always allowed for user)
    uint32_t flags = 0;     // VMA_ANON | …
};

constexpr unsigned kMaxVmas = 32;

struct AddressSpace {
    uint64_t pml4 = 0;          // physical address of the PML4 (what goes into CR3)
    uint64_t pages = 0;         // user leaf pages currently backed by a physical frame
    Vma      vmas[kMaxVmas]{};  // sorted by start; empty slots have start == end == 0
    unsigned nvmas = 0;
};

bool as_create(AddressSpace* as);
bool as_clone(const AddressSpace* src, AddressSpace* dst);           // COW fork
void as_destroy(AddressSpace* as);                                  // frees every user page + table

// Eager map of an already-allocated physical frame (used by ELF loader for file data).
// Takes ownership of `phys` (refcount is assumed 1).
bool as_map(AddressSpace* as, uint64_t va, uint64_t phys, uint64_t flags);

// Unmap one page; decrements refcount (frees when last).
bool as_unmap(AddressSpace* as, uint64_t va);

// Lazy anonymous allocation: installs a VMA + demand PTEs. No physical memory is
// consumed until the process first touches the pages.
bool as_alloc(AddressSpace* as, uint64_t va, uint64_t pages, uint64_t flags);

// Unmap a page-aligned range.  Splits/removes VMAs as needed and releases
// any physical frames (via refcount).  Partial coverage of a VMA is allowed.
bool as_munmap(AddressSpace* as, uint64_t va, uint64_t len);

// Change protection on a page-aligned range that must be fully covered by
// existing VMAs.  `prot` uses the same flags as as_alloc (PF_WRITE | PF_NX).
// COW pages that become writable are left COW until the next write fault.
bool as_mprotect(AddressSpace* as, uint64_t va, uint64_t len, uint64_t prot);

bool as_translate(const AddressSpace* as, uint64_t va, uint64_t* phys, uint64_t* flags);
bool as_copy_to(const AddressSpace* as, uint64_t va, const void* src, size_t n, bool force = false);
bool as_copy_from(const AddressSpace* as, void* dst, uint64_t va, size_t n);
bool as_copy_string(const AddressSpace* as, char* dst, uint64_t va, size_t max);
bool user_range_ok(uint64_t va, uint64_t len);

// Called from the #PF path for user-mode faults. Returns true if the fault was
// handled (demand page or COW break); false means a real fault → kill the process.
bool handle_user_page_fault(AddressSpace* as, uint64_t fault_addr, uint64_t error_code);

}  // namespace aos::vmm
