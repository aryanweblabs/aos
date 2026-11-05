/* SPDX-License-Identifier: MIT */
#include "kheap.hpp"
#include "../physical/pmm.h"
#include "../../arch/x86_64/sync.h"

namespace aos::heap {
namespace {
constexpr uint64_t page_size  = 4096;
constexpr uint64_t live_magic = 0x414F534845415031ULL;  // "AOSHEAP1"
constexpr uint64_t dead_magic = 0x414F534845415000ULL;  // "AOSHEAP\0"
constexpr uint16_t large_page = 1;
constexpr size_t   class_count = 8;
constexpr uint16_t class_sizes[class_count] = {16, 32, 64, 128, 256, 512, 1024, 2048};

struct alignas(16) PageHeader {
    uint64_t    magic;
    PageHeader* next;
    void*       free_list;
    uint16_t    block_size;
    uint16_t    capacity;
    uint16_t    free_count;
    uint16_t    flags;
    uint64_t    allocated[4];   // bitmap, up to 256 slots (enough for 16-byte class)
};
static_assert(sizeof(PageHeader) == 64, "heap page header must stay 64 bytes");

PageHeader* classes[class_count];
uint64_t    managed_pages;
bool        ready;
aos_spinlock_t heap_lock = AOS_SPINLOCK_INIT;

size_t class_for(size_t size) {
    for (size_t i = 0; i < class_count; ++i)
        if (size <= class_sizes[i]) return i;
    return class_count;
}

PageHeader* new_page(uint16_t block_size, uint16_t flags) {
    const uint64_t frame = pmm_alloc_frame();
    if (!frame) return nullptr;

    auto* page = reinterpret_cast<PageHeader*>(static_cast<uintptr_t>(frame));
    page->magic     = live_magic;
    page->next      = nullptr;
    page->free_list = nullptr;
    page->block_size = block_size;
    page->flags      = flags;
    for (unsigned i = 0; i < 4; ++i) page->allocated[i] = 0;

    if (flags == large_page) {
        page->capacity   = 1;
        page->free_count = 0;
        ++managed_pages;
        return page;
    }

    page->capacity   = static_cast<uint16_t>((page_size - sizeof(PageHeader)) / block_size);
    page->free_count = page->capacity;
    auto* first = reinterpret_cast<uint8_t*>(page) + sizeof(PageHeader);
    for (uint16_t i = 0; i < page->capacity; ++i) {
        void* block = first + static_cast<size_t>(i) * block_size;
        *reinterpret_cast<void**>(block) = page->free_list;
        page->free_list = block;
    }
    ++managed_pages;
    return page;
}

// Unlink `page` from the singly-linked class list headed by classes[index].
void unlink_page(size_t index, PageHeader* page) {
    PageHeader** walk = &classes[index];
    while (*walk) {
        if (*walk == page) {
            *walk = page->next;
            page->next = nullptr;
            return;
        }
        walk = &(*walk)->next;
    }
}
}  // namespace

bool initialize() {
    for (size_t i = 0; i < class_count; ++i) classes[i] = nullptr;
    managed_pages = 0;
    ready = true;
    return true;
}

static void* allocate_unlocked(size_t size) {
    if (!ready || !size || size > page_size - sizeof(PageHeader)) return nullptr;

    const size_t index = class_for(size);

    // Large object: one dedicated page, returned to PMM on release.
    if (index == class_count) {
        PageHeader* page = new_page(0, large_page);
        if (!page) return nullptr;
        page->free_list = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(page) + sizeof(PageHeader));
        // capacity stores the requested size for debugging only (fits in uint16_t
        // because of the size check above).
        page->capacity = static_cast<uint16_t>(size);
        return page->free_list;
    }

    // Find a slab page that still has free blocks.
    PageHeader* page = classes[index];
    while (page && page->free_count == 0) page = page->next;

    if (!page) {
        page = new_page(class_sizes[index], 0);
        if (!page) return nullptr;
        page->next = classes[index];
        classes[index] = page;
    }

    void* block = page->free_list;
    page->free_list = *reinterpret_cast<void**>(block);

    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(block) - reinterpret_cast<uintptr_t>(page) - sizeof(PageHeader);
    const uint16_t slot = static_cast<uint16_t>(offset / page->block_size);
    if (slot >= page->capacity) return nullptr;  // corrupt free list

    page->allocated[slot >> 6] |= 1ULL << (slot & 63);
    --page->free_count;
    return block;
}

void* allocate(size_t size) {
    const uint64_t f = aos_spin_lock_irqsave(&heap_lock);
    void* r = allocate_unlocked(size);
    aos_spin_unlock_irqrestore(&heap_lock, f);
    return r;
}

static void release_unlocked(void* pointer) {
    if (!pointer) return;

    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (address >= 0x100000000ULL) return;

    auto* page = reinterpret_cast<PageHeader*>(address & ~(page_size - 1));
    if (page->magic != live_magic) return;

    // ---- large page path ----
    if (page->flags == large_page) {
        if (pointer != reinterpret_cast<uint8_t*>(page) + sizeof(PageHeader)) return;
        page->magic     = dead_magic;
        page->free_list = nullptr;
        if (managed_pages) --managed_pages;
        pmm_free_frame(reinterpret_cast<uintptr_t>(page));
        return;
    }

    // ---- slab path ----
    const uintptr_t first = reinterpret_cast<uintptr_t>(page) + sizeof(PageHeader);
    if (address < first ||
        address >= reinterpret_cast<uintptr_t>(page) + page_size ||
        !page->block_size) return;

    const uintptr_t offset = address - first;
    if (offset % page->block_size) return;

    const uint16_t slot = static_cast<uint16_t>(offset / page->block_size);
    if (slot >= page->capacity) return;

    const uint64_t mask = 1ULL << (slot & 63);
    if (!(page->allocated[slot >> 6] & mask)) return;  // double-free

    page->allocated[slot >> 6] &= ~mask;
    ++page->free_count;
    *reinterpret_cast<void**>(pointer) = page->free_list;
    page->free_list = pointer;

    // When every block on this page is free, return the whole page to the PMM
    // so long-running systems do not accumulate empty slab pages.
    if (page->free_count == page->capacity) {
        const size_t index = class_for(page->block_size);
        if (index < class_count) unlink_page(index, page);
        page->magic     = dead_magic;
        page->free_list = nullptr;
        if (managed_pages) --managed_pages;
        pmm_free_frame(reinterpret_cast<uintptr_t>(page));
    }
}

void release(void* pointer) {
    const uint64_t f = aos_spin_lock_irqsave(&heap_lock);
    release_unlocked(pointer);
    aos_spin_unlock_irqrestore(&heap_lock, f);
}

uint64_t pages_in_use() { return managed_pages; }
}  // namespace aos::heap
