/* SPDX-License-Identifier: MIT */
#include "block_device.hpp"

namespace aos::storage {
bool valid(const BlockDevice* device) {
    return device && device->block_size && device->block_count && device->read;
}
bool read_blocks(const BlockDevice* device, uint64_t lba, uint32_t count, void* buffer) {
    if (!valid(device) || !buffer || !count || lba >= device->block_count || count > device->block_count - lba) return false;
    return device->read(device->context, lba, count, buffer);
}
bool write_blocks(const BlockDevice* device, uint64_t lba, uint32_t count, const void* buffer) {
    if (!device || !device->write || !valid(device) || !buffer || !count || lba >= device->block_count || count > device->block_count - lba) return false;
    return device->write(device->context, lba, count, buffer);
}
bool flush_blocks(const BlockDevice* device) {
    if (!device || !device->flush) return true;
    return device->flush(device->context);
}
}
