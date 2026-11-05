/* SPDX-License-Identifier: MIT */
// virtio_blk.hpp - Universal Virtio block class driver (PCI modern + legacy).
// One driver for all virtio-blk devices (QEMU/KVM and any compliant hypervisor).
#pragma once
#include "block_device.hpp"
#include <stdint.h>

namespace aos::storage::virtio_blk {

struct Info {
    char     model[41];
    uint64_t bytes;
    uint32_t native_block;   // always exposed as 512-byte BlockDevice blocks
};

// Probe every virtio-blk PCI function, negotiate features, set up one request queue each.
bool init();
int  device_count();
const BlockDevice* device(int i);
const Info*        info(int i);

}  // namespace aos::storage::virtio_blk
