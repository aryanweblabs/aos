/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

namespace aos::vmm {
constexpr uint64_t page_size = 4096;
void init();                    // remember the boot PML4 as the kernel's master page table
uint64_t kernel_cr3();          // physical address of that PML4
void sync_kernel_slot(unsigned pml4_index);   // copy a newly created top-level kernel entry into live address spaces
bool is_canonical(uint64_t address);
bool is_boot_identity_mapped(uint64_t physical_address);
bool map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags = 3);
bool unmap_page(uint64_t virtual_address);
bool translate(uint64_t virtual_address, uint64_t* physical_address);
void invalidate(uint64_t virtual_address);

// Change the caching of an identity-mapped physical range (below 4 GiB).
//  WriteCombining : framebuffers (fast streaming writes, no stale cache lines)
//  Uncached       : device registers (MMIO)
enum class CacheMode { WriteCombining, Uncached };
bool set_cache_mode(uint64_t physical_address, uint64_t length, CacheMode mode);
// Identity-map [phys, phys+len) with 2 MiB pages and the given cache mode. Works above 4 GiB
// (GPU framebuffers on AMD/Intel laptops usually live there).
bool map_mmio(uint64_t physical_address, uint64_t length, CacheMode mode);
}

extern "C" bool aos_vmm_map_mmio_uc(uint64_t physical_address, uint64_t length);   // for the C network drivers
