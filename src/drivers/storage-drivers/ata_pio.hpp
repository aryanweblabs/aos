/* SPDX-License-Identifier: MIT */
#pragma once
#include "block_device.hpp"
#include <stdint.h>

namespace aos::storage::ata_pio {

struct DriveInfo {
    bool        present;
    bool        lba48;
    uint64_t    sectors;          // total 512-byte sectors
    char        model[41];        // ATA model string (NUL-terminated)
    char        serial[21];
    uint16_t    cylinders;        // legacy CHS (informational)
    uint16_t    heads;
    uint16_t    sectors_per_track;
};

bool initialize_primary_master();
const BlockDevice* primary_master();
const DriveInfo*   primary_master_info();

}  // namespace aos::storage::ata_pio
