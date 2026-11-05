/* SPDX-License-Identifier: MIT */
// ahci.hpp - Universal AHCI (SATA) class driver.
// One driver for AHCI host controllers; every implemented port with a device becomes a disk.
#pragma once
#include "block_device.hpp"
#include <stdint.h>

namespace aos::storage::ahci {

struct Info {
    char     model[41];
    char     serial[21];
    uint64_t bytes;
    uint32_t native_block;
    uint8_t  port;
};

bool init();
int  device_count();
const BlockDevice* device(int i);
const Info*        info(int i);

}  // namespace aos::storage::ahci
