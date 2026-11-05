/* SPDX-License-Identifier: MIT */
#include "aspace.hpp"
#include "vmm.hpp"
#include "../physical/pmm.h"
#include "../../arch/x86_64/sync.h"

namespace aos::vmm {
namespace {

constexpr unsigned kMaxSpaces = 64;
AddressSpace*  registry[kMaxSpaces];
aos_spinlock_t reg_lock = AOS_SPINLOCK_INIT;

// Global zero page used for demand-zero reads (COW on first write).
uint64_t g_zero_page = 0;

inline uint64_t* tbl(uint64_t phys) {
    return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(phys & 0x000ffffffffff000ULL));
}

uint64_t new_table() {
    const uint64_t f = pmm_alloc_frame();
    if (!f) return 0;
    uint64_t* t = tbl(f);
    for (unsigned i = 0; i < 512; ++i) t[i] = 0;
    return f;
}

// Returns the next-level table for entry `idx`, creating it (user-accessible) when asked.
uint64_t* next_level(uint64_t* parent, unsigned idx, bool create) {
    if (!(parent[idx] & PF_PRESENT)) {
        if (!create) return nullptr;
        const uint64_t f = new_table();
        if (!f) return nullptr;
        parent[idx] = f | PF_PRESENT | PF_WRITE | PF_USER;
    }
    if (parent[idx] & 0x80) return nullptr;       // huge page: not a table
    return tbl(parent[idx]);
}

void free_subtree(uint64_t* table, int level) {   // level 3 = PDPT, 2 = PD, 1 = PT
    for (unsigned i = 0; i < 512; ++i) {
        const uint64_t e = table[i];
        if (!(e & PF_PRESENT)) {
            // demand PTE – nothing to free
            continue;
        }
        if (level == 1) {
            pmm_dec_ref(e & 0x000ffffffffff000ULL);
            continue;
        }
        if (e & 0x80) continue;
        free_subtree(tbl(e), level - 1);
        pmm_free_frame(e & 0x000ffffffffff000ULL);
    }
}

// Find VMA covering `va`. Returns index or -1.
int find_vma(const AddressSpace* as, uint64_t va) {
    for (unsigned i = 0; i < as->nvmas; ++i) {
        const Vma& v = as->vmas[i];
        if (va >= v.start && va < v.end) return static_cast<int>(i);
    }
    return -1;
}

// Insert a new VMA; keeps the array sorted by start. Returns false if full or overlap.
bool insert_vma(AddressSpace* as, uint64_t start, uint64_t end, uint64_t prot, uint32_t flags) {
    if (as->nvmas >= kMaxVmas || start >= end || (start & 4095) || (end & 4095)) return false;
    // Reject any overlap.
    for (unsigned i = 0; i < as->nvmas; ++i) {
        const Vma& v = as->vmas[i];
        if (!(end <= v.start || start >= v.end)) return false;
    }
    // Find insertion point.
    unsigned pos = as->nvmas;
    for (unsigned i = 0; i < as->nvmas; ++i) {
        if (as->vmas[i].start > start) { pos = i; break; }
    }
    for (unsigned i = as->nvmas; i > pos; --i) as->vmas[i] = as->vmas[i - 1];
    as->vmas[pos] = {start, end, prot, flags};
    ++as->nvmas;
    return true;
}

// Remove VMA that exactly matches [start, end). Used by unmap of whole regions later.
void remove_vma_range(AddressSpace* as, uint64_t start, uint64_t end) {
    unsigned w = 0;
    for (unsigned i = 0; i < as->nvmas; ++i) {
        if (as->vmas[i].start == start && as->vmas[i].end == end) continue;
        as->vmas[w++] = as->vmas[i];
    }
    as->nvmas = w;
}

// Install a demand PTE (Present=0, PF_DEMAND set). Creates intermediate tables if needed.
bool install_demand_pte(AddressSpace* as, uint64_t va) {
    uint64_t* pml4 = tbl(as->pml4);
    uint64_t* pdpt = next_level(pml4, (va >> 39) & 511, true);
    if (!pdpt) return false;
    uint64_t* pd = next_level(pdpt, (va >> 30) & 511, true);
    if (!pd) return false;
    uint64_t* pt = next_level(pd, (va >> 21) & 511, true);
    if (!pt) return false;
    uint64_t& e = pt[(va >> 12) & 511];
    if (e & PF_PRESENT) return false;          // already backed
    e = PF_DEMAND;                             // Present = 0
    return true;
}

// Get a pointer to the PTE for `va` (must already exist or be creatable).
uint64_t* get_pte(AddressSpace* as, uint64_t va, bool create) {
    uint64_t* pml4 = tbl(as->pml4);
    uint64_t* pdpt = next_level(pml4, (va >> 39) & 511, create);
    if (!pdpt) return nullptr;
    uint64_t* pd = next_level(pdpt, (va >> 30) & 511, create);
    if (!pd) return nullptr;
    uint64_t* pt = next_level(pd, (va >> 21) & 511, create);
    if (!pt) return nullptr;
    return &pt[(va >> 12) & 511];
}

void ensure_zero_page() {
    if (g_zero_page) return;
    g_zero_page = pmm_alloc_frame();
    if (!g_zero_page) return;
    uint8_t* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(g_zero_page));
    for (unsigned i = 0; i < 4096; ++i) p[i] = 0;
    // Keep an extra permanent reference so it is never freed.
    pmm_inc_ref(g_zero_page);
}

}  // namespace

void sync_kernel_slot(unsigned idx) {
    if (idx != 0 && idx < 256) return;            // user slots are private
    uint64_t* master = tbl(kernel_cr3());
    const uint64_t f = aos_spin_lock_irqsave(&reg_lock);
    for (auto* as : registry)
        if (as && as->pml4) tbl(as->pml4)[idx] = master[idx];
    aos_spin_unlock_irqrestore(&reg_lock, f);
}

bool user_range_ok(uint64_t va, uint64_t len) {
    return va >= kUserBase && va < kUserEnd && len <= kUserEnd - va;
}

bool as_create(AddressSpace* as) {
    const uint64_t f = new_table();
    if (!f) return false;
    uint64_t* pml4 = tbl(f);
    uint64_t* master = tbl(kernel_cr3());
    pml4[0] = master[0];
    for (unsigned i = 256; i < 512; ++i) pml4[i] = master[i];
    as->pml4 = f;
    as->pages = 0;
    as->nvmas = 0;
    for (unsigned i = 0; i < kMaxVmas; ++i) as->vmas[i] = {};
    const uint64_t fl = aos_spin_lock_irqsave(&reg_lock);
    bool placed = false;
    for (auto& slot : registry) if (!slot) { slot = as; placed = true; break; }
    aos_spin_unlock_irqrestore(&reg_lock, fl);
    if (!placed) { pmm_free_frame(f); as->pml4 = 0; return false; }
    ensure_zero_page();
    return true;
}

bool as_clone(const AddressSpace* src, AddressSpace* dst) {
    if (!src || !src->pml4 || !dst) return false;
    if (!as_create(dst)) return false;

    // Copy VMA list.
    dst->nvmas = src->nvmas;
    for (unsigned i = 0; i < src->nvmas; ++i) dst->vmas[i] = src->vmas[i];

    // Walk every user page and share it under COW, or copy demand markers.
    const uint64_t* pml4 = tbl(src->pml4);
    for (unsigned pml4i = 1; pml4i < 256; ++pml4i) {
        if (!(pml4[pml4i] & PF_PRESENT)) continue;
        if (pml4[pml4i] & 0x80) continue;
        const uint64_t* pdpt = tbl(pml4[pml4i]);
        for (unsigned pdpti = 0; pdpti < 512; ++pdpti) {
            if (!(pdpt[pdpti] & PF_PRESENT) || (pdpt[pdpti] & 0x80)) continue;
            const uint64_t* pd = tbl(pdpt[pdpti]);
            for (unsigned pdi = 0; pdi < 512; ++pdi) {
                if (!(pd[pdi] & PF_PRESENT) || (pd[pdi] & 0x80)) continue;
                const uint64_t* pt = tbl(pd[pdi]);
                for (unsigned pti = 0; pti < 512; ++pti) {
                    const uint64_t e = pt[pti];
                    const uint64_t va = (static_cast<uint64_t>(pml4i) << 39) |
                                       (static_cast<uint64_t>(pdpti) << 30) |
                                       (static_cast<uint64_t>(pdi) << 21) |
                                       (static_cast<uint64_t>(pti) << 12);
                    if (!user_range_ok(va, kPageSize)) continue;

                    if (e & PF_DEMAND) {
                        // Still demand – just install the same marker in the child.
                        if (!install_demand_pte(dst, va)) {
                            as_destroy(dst);
                            return false;
                        }
                        continue;
                    }
                    if (!(e & PF_PRESENT)) continue;

                    // Present page → share under COW.
                    const uint64_t phys = e & 0x000ffffffffff000ULL;
                    pmm_inc_ref(phys);

                    // Both sides become read-only + COW.  The original `e` does not
                    // yet carry PF_COW, so the bit must be set explicitly on both PTEs.
                    const uint64_t shared = (phys | (e & (PF_USER | PF_NX)) |
                                            PF_PRESENT | PF_COW) & ~PF_WRITE;

                    uint64_t* src_pte = const_cast<uint64_t*>(&pt[pti]);
                    *src_pte = shared;
                    asm volatile("invlpg (%0)" :: "r"(va) : "memory");

                    uint64_t* dst_pte = get_pte(dst, va, true);
                    if (!dst_pte) {
                        pmm_dec_ref(phys);
                        as_destroy(dst);
                        return false;
                    }
                    *dst_pte = shared;
                    ++dst->pages;
                }
            }
        }
    }
    return true;
}

void as_destroy(AddressSpace* as) {
    if (!as || !as->pml4) return;
    const uint64_t fl = aos_spin_lock_irqsave(&reg_lock);
    for (auto& slot : registry) if (slot == as) slot = nullptr;
    aos_spin_unlock_irqrestore(&reg_lock, fl);
    uint64_t* pml4 = tbl(as->pml4);
    for (unsigned i = 1; i < 256; ++i) {
        if (!(pml4[i] & PF_PRESENT)) continue;
        free_subtree(tbl(pml4[i]), 3);
        pmm_free_frame(pml4[i] & 0x000ffffffffff000ULL);
        pml4[i] = 0;
    }
    pmm_free_frame(as->pml4);
    as->pml4 = 0;
    as->pages = 0;
    as->nvmas = 0;
}

bool as_map(AddressSpace* as, uint64_t va, uint64_t phys, uint64_t flags) {
    if (!as || !as->pml4 || (va & 4095) || (phys & 4095) || !user_range_ok(va, kPageSize)) return false;
    uint64_t* pte = get_pte(as, va, true);
    if (!pte) return false;
    if (*pte & PF_PRESENT) return false;
    // Clear any previous demand marker.
    *pte = phys | (flags & (PF_WRITE | PF_NX)) | PF_USER | PF_PRESENT;
    ++as->pages;
    return true;
}

bool as_unmap(AddressSpace* as, uint64_t va) {
    if (!as || !as->pml4 || (va & 4095) || !user_range_ok(va, kPageSize)) return false;
    uint64_t* pte = get_pte(as, va, false);
    if (!pte) return false;
    const uint64_t e = *pte;
    if (e & PF_DEMAND) {
        *pte = 0;
        return true;
    }
    if (!(e & PF_PRESENT)) return false;
    const uint64_t phys = e & 0x000ffffffffff000ULL;
    *pte = 0;
    if (as->pages) --as->pages;
    pmm_dec_ref(phys);
    // Note: we do not remove the VMA here; callers that unmap whole regions
    // should call a higher-level helper in the future.
    return true;
}

bool as_alloc(AddressSpace* as, uint64_t va, uint64_t pages, uint64_t flags) {
    if (!as || !as->pml4 || !pages || (va & 4095) || !user_range_ok(va, pages * kPageSize))
        return false;
    const uint64_t end = va + pages * kPageSize;
    const uint64_t prot = flags & (PF_WRITE | PF_NX);

    if (!insert_vma(as, va, end, prot, VMA_ANON)) return false;

    // Install demand PTEs for the whole range. On any failure roll back.
    for (uint64_t i = 0; i < pages; ++i) {
        if (!install_demand_pte(as, va + i * kPageSize)) {
            // Roll back demand PTEs and the VMA.
            for (uint64_t j = 0; j < i; ++j) {
                uint64_t* pte = get_pte(as, va + j * kPageSize, false);
                if (pte) *pte = 0;
            }
            remove_vma_range(as, va, end);
            return false;
        }
    }
    return true;
}

// Unmap every leaf PTE in [va, va+len) and release frames.
static void unmap_range_pages(AddressSpace* as, uint64_t va, uint64_t len) {
    for (uint64_t off = 0; off < len; off += kPageSize) {
        uint64_t* pte = get_pte(as, va + off, false);
        if (!pte) continue;
        const uint64_t e = *pte;
        if (e & PF_DEMAND) {
            *pte = 0;
            continue;
        }
        if (!(e & PF_PRESENT)) {
            *pte = 0;
            continue;
        }
        const uint64_t phys = e & 0x000ffffffffff000ULL;
        *pte = 0;
        if (as->pages) --as->pages;
        pmm_dec_ref(phys);
        asm volatile("invlpg (%0)" :: "r"(va + off) : "memory");
    }
}

// Rewrite the VMA list so that [va, va+len) is no longer covered.
// May split VMAs at the boundaries.  Returns false only on VMA-table overflow.
static bool punch_vmas(AddressSpace* as, uint64_t va, uint64_t end) {
    Vma out[kMaxVmas];
    unsigned n = 0;
    for (unsigned i = 0; i < as->nvmas; ++i) {
        Vma v = as->vmas[i];
        if (v.end <= va || v.start >= end) {
            // Completely outside the hole.
            if (n >= kMaxVmas) return false;
            out[n++] = v;
            continue;
        }
        // Left remnant.
        if (v.start < va) {
            if (n >= kMaxVmas) return false;
            out[n++] = {v.start, va, v.prot, v.flags};
        }
        // Right remnant.
        if (v.end > end) {
            if (n >= kMaxVmas) return false;
            out[n++] = {end, v.end, v.prot, v.flags};
        }
    }
    as->nvmas = n;
    for (unsigned i = 0; i < n; ++i) as->vmas[i] = out[i];
    return true;
}

bool as_munmap(AddressSpace* as, uint64_t va, uint64_t len) {
    if (!as || !as->pml4 || !len) return false;
    if ((va & 4095) || (len & 4095)) return false;
    if (!user_range_ok(va, len)) return false;
    const uint64_t end = va + len;

    unmap_range_pages(as, va, len);
    if (!punch_vmas(as, va, end)) return false;
    return true;
}

bool as_mprotect(AddressSpace* as, uint64_t va, uint64_t len, uint64_t prot) {
    if (!as || !as->pml4 || !len) return false;
    if ((va & 4095) || (len & 4095)) return false;
    if (!user_range_ok(va, len)) return false;
    const uint64_t end = va + len;
    prot &= (PF_WRITE | PF_NX);

    // Every page in the range must belong to some VMA.
    for (uint64_t a = va; a < end; ) {
        const int idx = find_vma(as, a);
        if (idx < 0) return false;
        a = as->vmas[idx].end;
    }

    // Update VMA protections (split at boundaries when needed).
    Vma out[kMaxVmas];
    unsigned n = 0;
    for (unsigned i = 0; i < as->nvmas; ++i) {
        Vma v = as->vmas[i];
        if (v.end <= va || v.start >= end) {
            if (n >= kMaxVmas) return false;
            out[n++] = v;
            continue;
        }
        // Left remnant keeps old prot.
        if (v.start < va) {
            if (n >= kMaxVmas) return false;
            out[n++] = {v.start, va, v.prot, v.flags};
        }
        // Middle slice gets new prot.
        const uint64_t ms = v.start < va ? va : v.start;
        const uint64_t me = v.end > end ? end : v.end;
        if (n >= kMaxVmas) return false;
        out[n++] = {ms, me, prot, v.flags};
        // Right remnant keeps old prot.
        if (v.end > end) {
            if (n >= kMaxVmas) return false;
            out[n++] = {end, v.end, v.prot, v.flags};
        }
    }
    as->nvmas = n;
    for (unsigned i = 0; i < n; ++i) as->vmas[i] = out[i];

    // Update existing PTEs.  Demand pages only need the VMA prot (applied on fault).
    // Present pages: adjust Write/NX; keep COW if set (write will break it later).
    for (uint64_t off = 0; off < len; off += kPageSize) {
        uint64_t* pte = get_pte(as, va + off, false);
        if (!pte) continue;
        uint64_t e = *pte;
        if (e & PF_DEMAND) continue;
        if (!(e & PF_PRESENT)) continue;
        e = (e & ~(PF_WRITE | PF_NX)) | prot;
        // If the page is still COW, do not grant Write until the break.
        if (e & PF_COW) e &= ~PF_WRITE;
        *pte = e;
        asm volatile("invlpg (%0)" :: "r"(va + off) : "memory");
    }
    return true;
}

bool as_translate(const AddressSpace* as, uint64_t va, uint64_t* phys, uint64_t* flags) {
    if (!as || !as->pml4 || !user_range_ok(va, 1)) return false;
    const uint64_t* pml4 = tbl(as->pml4);
    uint64_t e = pml4[(va >> 39) & 511];
    if (!(e & PF_PRESENT)) return false;
    e = tbl(e)[(va >> 30) & 511];
    if (!(e & PF_PRESENT) || (e & 0x80)) return false;
    e = tbl(e)[(va >> 21) & 511];
    if (!(e & PF_PRESENT) || (e & 0x80)) return false;
    e = tbl(e)[(va >> 12) & 511];
    if (e & PF_DEMAND) return false;          // not yet backed
    if (!(e & PF_PRESENT)) return false;
    if (phys) *phys = (e & 0x000ffffffffff000ULL) | (va & 4095);
    if (flags) *flags = e & (PF_WRITE | PF_USER | PF_NX | PF_COW);
    return true;
}

bool as_copy_to(const AddressSpace* as, uint64_t va, const void* src, size_t n, bool force) {
    if (!user_range_ok(va, n)) return false;
    const uint8_t* s = static_cast<const uint8_t*>(src);
    AddressSpace* mut = const_cast<AddressSpace*>(as);

    while (n) {
        uint64_t pa = 0, fl = 0;
        bool backed = as_translate(as, va, &pa, &fl);

        if (!backed) {
            if (!force) return false;
            // Loader force path: allocate a private frame even for read-only VMAs
            // so we can write the initial file contents, then lock the page down.
            const int vidx = find_vma(as, va);
            if (vidx < 0) return false;
            const Vma& vma = as->vmas[vidx];

            uint64_t* pte = get_pte(mut, va, true);
            if (!pte) return false;
            if (!(*pte & PF_PRESENT)) {
                const uint64_t phys = pmm_alloc_frame();
                if (!phys) return false;
                uint8_t* page = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(phys));
                for (unsigned i = 0; i < 4096; ++i) page[i] = 0;
                // Temporarily writable for the copy.
                *pte = phys | PF_PRESENT | PF_USER | PF_WRITE | (vma.prot & PF_NX);
                ++mut->pages;
            }
            if (!as_translate(as, va, &pa, &fl)) return false;
        }

        if (!(fl & PF_USER) || (!force && !(fl & PF_WRITE))) return false;

        size_t chunk = 4096 - (va & 4095);
        if (chunk > n) chunk = n;
        uint8_t* d = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(pa));
        for (size_t i = 0; i < chunk; ++i) d[i] = s[i];

        // After loading into a read-only VMA, remove the write bit.
        if (force && !backed) {
            const int vidx = find_vma(as, va);
            if (vidx >= 0 && !(as->vmas[vidx].prot & PF_WRITE)) {
                uint64_t* pte = get_pte(mut, va, false);
                if (pte && (*pte & PF_PRESENT))
                    *pte = (*pte & ~PF_WRITE);
            }
        }

        va += chunk; s += chunk; n -= chunk;
    }
    return true;
}

bool as_copy_from(const AddressSpace* as, void* dst, uint64_t va, size_t n) {
    if (!user_range_ok(va, n)) return false;
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (n) {
        uint64_t pa, fl;
        if (!as_translate(as, va, &pa, &fl) || !(fl & PF_USER)) return false;
        size_t chunk = 4096 - (va & 4095);
        if (chunk > n) chunk = n;
        const uint8_t* s = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pa));
        for (size_t i = 0; i < chunk; ++i) d[i] = s[i];
        va += chunk; d += chunk; n -= chunk;
    }
    return true;
}

bool as_copy_string(const AddressSpace* as, char* dst, uint64_t va, size_t max) {
    if (!max || !user_range_ok(va, 1)) return false;
    size_t i = 0;
    while (i < max - 1) {
        char c;
        if (!as_copy_from(as, &c, va + i, 1)) return false;
        dst[i++] = c;
        if (c == 0) return true;
    }
    dst[max - 1] = 0;
    return false;   // truncated
}

bool handle_user_page_fault(AddressSpace* as, uint64_t fault_addr, uint64_t error_code) {
    if (!as || !as->pml4) return false;
    if (!user_range_ok(fault_addr, 1)) return false;

    const bool present = (error_code & 1) != 0;
    const bool write   = (error_code & 2) != 0;
    // const bool user = (error_code & 4) != 0;  // already checked by caller

    const int vidx = find_vma(as, fault_addr);
    if (vidx < 0) return false;                 // no VMA → hard fault
    const Vma& vma = as->vmas[vidx];

    uint64_t* pte = get_pte(as, fault_addr, true);
    if (!pte) return false;
    uint64_t e = *pte;

    // ----- Demand-zero path -----
    if (!present || (e & PF_DEMAND)) {
        if (write && !(vma.prot & PF_WRITE)) return false;   // write to read-only VMA

        uint64_t phys;
        if (!write && g_zero_page) {
            // Read fault → map the global zero page as COW.
            phys = g_zero_page;
            pmm_inc_ref(phys);
            *pte = phys | PF_PRESENT | PF_USER | PF_COW | (vma.prot & PF_NX);
        } else {
            // Write (or no zero page) → allocate a private zeroed frame.
            phys = pmm_alloc_frame();
            if (!phys) return false;
            uint8_t* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(phys));
            for (unsigned i = 0; i < 4096; ++i) p[i] = 0;
            uint64_t fl = PF_PRESENT | PF_USER | (vma.prot & (PF_WRITE | PF_NX));
            *pte = phys | fl;
        }
        ++as->pages;
        asm volatile("invlpg (%0)" :: "r"(fault_addr) : "memory");
        return true;
    }

    // ----- COW break path -----
    if (present && write && (e & PF_COW)) {
        const uint64_t old_phys = e & 0x000ffffffffff000ULL;
        const uint16_t refs = pmm_get_ref(old_phys);

        if (refs <= 1) {
            // Last reference – just make it writable.
            *pte = (e & ~PF_COW) | PF_WRITE;
        } else {
            // Need a private copy.
            const uint64_t new_phys = pmm_alloc_frame();
            if (!new_phys) return false;
            uint8_t* d = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(new_phys));
            const uint8_t* s = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(old_phys));
            for (unsigned i = 0; i < 4096; ++i) d[i] = s[i];
            pmm_dec_ref(old_phys);
            *pte = new_phys | PF_PRESENT | PF_USER | PF_WRITE | (e & PF_NX);
        }
        asm volatile("invlpg (%0)" :: "r"(fault_addr) : "memory");
        return true;
    }

    // Any other case (read of present page, write of non-COW read-only, …) is a real fault.
    return false;
}

}  // namespace aos::vmm
