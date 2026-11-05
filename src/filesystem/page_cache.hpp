/* SPDX-License-Identifier: MIT */
// page_cache.hpp - block/page cache for filesystem data.
//
// Design goals (see docs/VFS.md):
//   - Cache disk clusters so repeated reads do not hit the device.
//   - Simple, deterministic eviction (CLOCK).
//   - Safe to call from the FS lock; not IRQ-safe (callers hold FS mutex).
//   - Invalidate on write / unmount so the cache never serves stale data.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::page_cache {

constexpr size_t kClusterSize = 512;
constexpr unsigned kDefaultSlots = 512;   // 512 * 512B = 256 KiB

struct Stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t inserts;
    uint64_t evictions;
    uint64_t invalidations;
    unsigned used;
    unsigned capacity;
};

// dev_id: stable non-zero id for the block device (pointer bits are fine).
// cluster: filesystem cluster / LBA index.
//
// Returns true and copies kClusterSize bytes into `out` on hit.
bool get(uint64_t dev_id, uint32_t cluster, void* out);

// Insert or overwrite a cache entry.  Copies kClusterSize bytes from `data`.
// May evict another entry.  Returns false only if the cache is not initialised.
bool put(uint64_t dev_id, uint32_t cluster, const void* data);

// Drop one entry (e.g. after a write to that cluster).
void invalidate(uint64_t dev_id, uint32_t cluster);

// Drop every entry for a device (unmount / format).
void invalidate_device(uint64_t dev_id);

// Drop everything.
void invalidate_all();

void stats(Stats* out);
void reset_stats();

// Called once at boot (or first use).  Safe to call multiple times.
void init();

}  // namespace aos::page_cache
