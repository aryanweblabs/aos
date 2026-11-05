/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

namespace aos::storage {
struct BlockDevice {
    void* context;
    uint64_t block_count;
    uint32_t block_size;
    bool (*read)(void* context, uint64_t lba, uint32_t count, void* buffer);
    bool (*write)(void* context, uint64_t lba, uint32_t count, const void* buffer);
    bool (*flush)(void* context) = nullptr;     // optional: make written data durable (NVMe volatile write cache)
};
bool valid(const BlockDevice* device);
bool read_blocks(const BlockDevice* device, uint64_t lba, uint32_t count, void* buffer);
bool write_blocks(const BlockDevice* device, uint64_t lba, uint32_t count, const void* buffer);
bool flush_blocks(const BlockDevice* device);
}
