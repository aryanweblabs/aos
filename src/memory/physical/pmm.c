/* SPDX-License-Identifier: MIT */
#include "pmm.h"
#include "../../arch/x86_64/sync.h"

static aos_spinlock_t g_pmm_lock = AOS_SPINLOCK_INIT;

#define PAGE_SIZE 4096ULL
#define MAX_FRAMES (1ULL << 20) /* Manage the 4 GiB identity-mapped physical range. */
#define BITMAP_WORDS (MAX_FRAMES / 64)

struct __attribute__((packed)) tag_header { uint32_t type, size; };
struct __attribute__((packed)) mmap_tag { uint32_t type, size, entry_size, entry_version; };
struct __attribute__((packed)) mmap_entry { uint64_t address, length; uint32_t type, reserved; };

static uint64_t used[BITMAP_WORDS];
static uint64_t g_total_frames;   /* usable RAM (frames) according to the firmware memory map */
static uint64_t allocated[BITMAP_WORDS];
static uint64_t free_count;
/* Per-frame reference counts for COW / shared pages.  Only frames that have
 * been handed out by pmm_alloc_* ever have a non-zero count. */
static uint16_t refcount[MAX_FRAMES];

static void set_used(uint64_t frame) { used[frame >> 6] |= 1ULL << (frame & 63); }
static void set_free(uint64_t frame) { used[frame >> 6] &= ~(1ULL << (frame & 63)); }
static int is_used(uint64_t frame) { return (used[frame >> 6] >> (frame & 63)) & 1; }
static void reserve_range(uint64_t begin, uint64_t end) {
    if (end <= begin) return;
    uint64_t first = begin / PAGE_SIZE;
    uint64_t last = end / PAGE_SIZE + ((end % PAGE_SIZE) != 0);
    if (last > MAX_FRAMES) last = MAX_FRAMES;
    for (uint64_t frame = first; frame < last; ++frame) {
        if (!is_used(frame)) { set_used(frame); --free_count; }
    }
}

int pmm_init(const void* multiboot_info, uint64_t kernel_start, uint64_t kernel_end) {
    for (uint64_t i = 0; i < BITMAP_WORDS; ++i) used[i] = UINT64_MAX;
    for (uint64_t i = 0; i < BITMAP_WORDS; ++i) allocated[i] = 0;
    for (uint64_t i = 0; i < MAX_FRAMES; ++i) refcount[i] = 0;
    free_count = 0;
    if (!multiboot_info) return 0;
    const uint8_t* base = (const uint8_t*)multiboot_info;
    const uint32_t total_size = *(const uint32_t*)base;
    if (total_size < 16 || total_size > 0x1000000U) return 0;
    int found_map = 0;
    for (uint32_t offset = 8; offset + sizeof(struct tag_header) <= total_size;) {
        const struct tag_header* header = (const struct tag_header*)(base + offset);
        if (header->type == 0 || header->size < 8 || offset + header->size > total_size) break;
        if (header->type == 6 && header->size >= sizeof(struct mmap_tag)) {
            const struct mmap_tag* tag = (const struct mmap_tag*)header;
            if (tag->entry_size < sizeof(struct mmap_entry)) return 0;
            uint32_t entry_offset = sizeof(struct mmap_tag);
            while (entry_offset + tag->entry_size <= tag->size) {
                const struct mmap_entry* entry = (const struct mmap_entry*)((const uint8_t*)tag + entry_offset);
                if (entry->type == 1 && entry->address < MAX_FRAMES * PAGE_SIZE) {
                    uint64_t end = entry->address + entry->length;
                    if (end < entry->address || end > MAX_FRAMES * PAGE_SIZE) end = MAX_FRAMES * PAGE_SIZE;
                    uint64_t first = (entry->address + PAGE_SIZE - 1) / PAGE_SIZE;
                    uint64_t last = end / PAGE_SIZE;
                    for (uint64_t frame = first; frame < last; ++frame) {
                        if (is_used(frame)) { set_free(frame); ++free_count; }
                    }
                }
                entry_offset += tag->entry_size;
            }
            found_map = 1;
        }
        offset = (offset + header->size + 7) & ~7U;
    }
    if (!found_map) return 0;
    g_total_frames = free_count;
    reserve_range(0, 0x100000);
    reserve_range(kernel_start, kernel_end);
    reserve_range((uint64_t)(uintptr_t)multiboot_info,
                  (uint64_t)(uintptr_t)multiboot_info + total_size);
    for (uint32_t offset = 8; offset + sizeof(struct tag_header) <= total_size;) {
        const struct tag_header* header = (const struct tag_header*)(base + offset);
        if (header->type == 0 || header->size < 8 || offset + header->size > total_size) break;
        if (header->type == 3 && header->size >= 16) {
            const uint32_t* module = (const uint32_t*)(const void*)(base + offset);
            if (module[3] >= module[2]) reserve_range(module[2], module[3]);
        }
        offset = (offset + header->size + 7) & ~7U;
    }
    return 1;
}

static uint64_t pmm_alloc_frame_locked(void) {
    if (!free_count) return 0;
    for (uint64_t word = 0; word < BITMAP_WORDS; ++word) {
        const uint64_t available = ~used[word];
        if (!available) continue;
        const unsigned bit = (unsigned)__builtin_ctzll(available);
        const uint64_t frame = word * 64 + bit;
        if (frame >= MAX_FRAMES) return 0;
        const uint64_t mask = 1ULL << bit;
        used[word] |= mask;
        allocated[word] |= mask;
        refcount[frame] = 1;
        --free_count;
        return frame * PAGE_SIZE;
    }
    return 0;
}

uint64_t pmm_alloc_frame(void) {
    const uint64_t f = aos_spin_lock_irqsave(&g_pmm_lock);
    const uint64_t r = pmm_alloc_frame_locked();
    aos_spin_unlock_irqrestore(&g_pmm_lock, f);
    return r;
}

static uint64_t pmm_alloc_contiguous_locked(uint64_t count) {
    if (!count || count > free_count) return 0;
    uint64_t run = 0, start = 0;
    for (uint64_t frame = 0; frame < MAX_FRAMES; ++frame) {
        if ((frame & 63) == 0 && used[frame >> 6] == UINT64_MAX) { frame += 63; run = 0; continue; }
        if (is_used(frame)) { run = 0; continue; }
        if (run == 0) start = frame;
        if (++run == count) {
            for (uint64_t f = start; f < start + count; ++f) {
                used[f >> 6] |= 1ULL << (f & 63);
                allocated[f >> 6] |= 1ULL << (f & 63);
                refcount[f] = 1;
            }
            free_count -= count;
            return start * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    const uint64_t f = aos_spin_lock_irqsave(&g_pmm_lock);
    const uint64_t r = pmm_alloc_contiguous_locked(count);
    aos_spin_unlock_irqrestore(&g_pmm_lock, f);
    return r;
}

static void pmm_free_frame_locked(uint64_t address) {
    if ((address & (PAGE_SIZE - 1)) || address >= MAX_FRAMES * PAGE_SIZE) return;
    const uint64_t frame = address / PAGE_SIZE;
    const uint64_t mask = 1ULL << (frame & 63);
    if (allocated[frame >> 6] & mask) {
        allocated[frame >> 6] &= ~mask;
        refcount[frame] = 0;
        set_free(frame);
        ++free_count;
    }
}

void pmm_free_frame(uint64_t address) {
    const uint64_t f = aos_spin_lock_irqsave(&g_pmm_lock);
    pmm_free_frame_locked(address);
    aos_spin_unlock_irqrestore(&g_pmm_lock, f);
}

void pmm_inc_ref(uint64_t physical_address) {
    if ((physical_address & (PAGE_SIZE - 1)) || physical_address >= MAX_FRAMES * PAGE_SIZE) return;
    const uint64_t frame = physical_address / PAGE_SIZE;
    const uint64_t f = aos_spin_lock_irqsave(&g_pmm_lock);
    if (allocated[frame >> 6] & (1ULL << (frame & 63))) {
        if (refcount[frame] < 0xffff) ++refcount[frame];
    }
    aos_spin_unlock_irqrestore(&g_pmm_lock, f);
}

void pmm_dec_ref(uint64_t physical_address) {
    if ((physical_address & (PAGE_SIZE - 1)) || physical_address >= MAX_FRAMES * PAGE_SIZE) return;
    const uint64_t frame = physical_address / PAGE_SIZE;
    const uint64_t f = aos_spin_lock_irqsave(&g_pmm_lock);
    if (allocated[frame >> 6] & (1ULL << (frame & 63))) {
        if (refcount[frame] > 1) {
            --refcount[frame];
        } else {
            /* last reference – free the frame */
            pmm_free_frame_locked(physical_address);
        }
    }
    aos_spin_unlock_irqrestore(&g_pmm_lock, f);
}

uint16_t pmm_get_ref(uint64_t physical_address) {
    if ((physical_address & (PAGE_SIZE - 1)) || physical_address >= MAX_FRAMES * PAGE_SIZE) return 0;
    const uint64_t frame = physical_address / PAGE_SIZE;
    const uint64_t f = aos_spin_lock_irqsave(&g_pmm_lock);
    uint16_t r = 0;
    if (allocated[frame >> 6] & (1ULL << (frame & 63))) r = refcount[frame];
    aos_spin_unlock_irqrestore(&g_pmm_lock, f);
    return r;
}

uint64_t pmm_free_frames(void) { return free_count; }
uint64_t pmm_total_frames(void) { return g_total_frames; }
