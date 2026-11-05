/* SPDX-License-Identifier: MIT */
// nvme.hpp - NVM Express (PCIe SSD) driver: polling, one admin + one I/O queue, 512-byte block view.
#pragma once
#include "block_device.hpp"
#include <stdint.h>

namespace aos::storage::nvme {

struct Info {
    char     model[41];
    char     serial[21];
    uint32_t nsid;
    uint64_t bytes;
    uint32_t native_block;     // 512 or 4096 (the BlockDevice always exposes 512-byte blocks)
};

bool init();                               // find controllers, create queues, identify namespaces
int  namespace_count();
const BlockDevice* device(int i);          // whole namespace, 512-byte blocks
const Info*        info(int i);

}  // namespace aos::storage::nvme
