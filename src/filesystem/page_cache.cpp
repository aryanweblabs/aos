/* SPDX-License-Identifier: MIT */
#include "page_cache.hpp"
#include "../arch/x86_64/sync.h"
#include <stdint.h>

namespace aos::page_cache {
namespace {

struct Slot {
    uint64_t dev_id;
    uint32_t cluster;
    uint16_t ref;          // CLOCK reference bit
    uint8_t  used;
    uint8_t  data[kClusterSize];
};

// Static storage so the cache works even before the kernel heap is ready.
alignas(64) Slot g_slot_storage[kDefaultSlots];

Slot*          g_slots = nullptr;
unsigned       g_cap = 0;
unsigned       g_used = 0;
unsigned       g_hand = 0;
Stats          g_stats{};
aos_spinlock_t g_lock = AOS_SPINLOCK_INIT;
bool           g_ready = false;

int find_slot(uint64_t dev_id, uint32_t cluster) {
    for (unsigned i = 0; i < g_cap; ++i) {
        if (g_slots[i].used && g_slots[i].dev_id == dev_id && g_slots[i].cluster == cluster)
            return static_cast<int>(i);
    }
    return -1;
}

// CLOCK: advance hand until a slot with ref==0 is found (or we wrap twice).
unsigned evict_one() {
    for (unsigned pass = 0; pass < g_cap * 2; ++pass) {
        Slot& s = g_slots[g_hand];
        const unsigned idx = g_hand;
        g_hand = (g_hand + 1) % g_cap;
        if (!s.used) return idx;
        if (s.ref) {
            s.ref = 0;
            continue;
        }
        s.used = 0;
        if (g_used) --g_used;
        ++g_stats.evictions;
        return idx;
    }
    // All referenced — force the current hand.
    unsigned idx = g_hand;
    g_hand = (g_hand + 1) % g_cap;
    if (g_slots[idx].used && g_used) --g_used;
    g_slots[idx].used = 0;
    ++g_stats.evictions;
    return idx;
}

}  // namespace

void init() {
    if (g_ready) return;
    g_cap = kDefaultSlots;
    g_slots = g_slot_storage;
    for (unsigned i = 0; i < g_cap; ++i) {
        g_slots[i].used = 0;
        g_slots[i].ref = 0;
        g_slots[i].dev_id = 0;
        g_slots[i].cluster = 0;
    }
    g_used = 0;
    g_hand = 0;
    g_stats = {};
    g_stats.capacity = g_cap;
    g_ready = true;
}

bool get(uint64_t dev_id, uint32_t cluster, void* out) {
    if (!g_ready) init();
    if (!g_slots || !out || !dev_id) return false;
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    const int idx = find_slot(dev_id, cluster);
    if (idx < 0) {
        ++g_stats.misses;
        aos_spin_unlock_irqrestore(&g_lock, f);
        return false;
    }
    Slot& s = g_slots[idx];
    s.ref = 1;
    for (size_t i = 0; i < kClusterSize; ++i)
        static_cast<uint8_t*>(out)[i] = s.data[i];
    ++g_stats.hits;
    aos_spin_unlock_irqrestore(&g_lock, f);
    return true;
}

bool put(uint64_t dev_id, uint32_t cluster, const void* data) {
    if (!g_ready) init();
    if (!g_slots || !data || !dev_id) return false;
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    int idx = find_slot(dev_id, cluster);
    if (idx < 0) {
        if (g_used < g_cap) {
            // Free slot.
            for (unsigned i = 0; i < g_cap; ++i) {
                if (!g_slots[i].used) { idx = static_cast<int>(i); break; }
            }
        }
        if (idx < 0) idx = static_cast<int>(evict_one());
        ++g_used;
        ++g_stats.inserts;
    }
    Slot& s = g_slots[idx];
    s.dev_id = dev_id;
    s.cluster = cluster;
    s.ref = 1;
    s.used = 1;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < kClusterSize; ++i) s.data[i] = src[i];
    aos_spin_unlock_irqrestore(&g_lock, f);
    return true;
}

void invalidate(uint64_t dev_id, uint32_t cluster) {
    if (!g_ready || !g_slots) return;
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    const int idx = find_slot(dev_id, cluster);
    if (idx >= 0) {
        g_slots[idx].used = 0;
        if (g_used) --g_used;
        ++g_stats.invalidations;
    }
    aos_spin_unlock_irqrestore(&g_lock, f);
}

void invalidate_device(uint64_t dev_id) {
    if (!g_ready || !g_slots) return;
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    for (unsigned i = 0; i < g_cap; ++i) {
        if (g_slots[i].used && g_slots[i].dev_id == dev_id) {
            g_slots[i].used = 0;
            if (g_used) --g_used;
            ++g_stats.invalidations;
        }
    }
    aos_spin_unlock_irqrestore(&g_lock, f);
}

void invalidate_all() {
    if (!g_ready || !g_slots) return;
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    for (unsigned i = 0; i < g_cap; ++i) g_slots[i].used = 0;
    g_used = 0;
    ++g_stats.invalidations;
    aos_spin_unlock_irqrestore(&g_lock, f);
}

void stats(Stats* out) {
    if (!out) return;
    if (!g_ready) init();
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    *out = g_stats;
    out->used = g_used;
    out->capacity = g_cap;
    aos_spin_unlock_irqrestore(&g_lock, f);
}

void reset_stats() {
    const uint64_t f = aos_spin_lock_irqsave(&g_lock);
    g_stats.hits = g_stats.misses = g_stats.inserts = g_stats.evictions = g_stats.invalidations = 0;
    aos_spin_unlock_irqrestore(&g_lock, f);
}

}  // namespace aos::page_cache
