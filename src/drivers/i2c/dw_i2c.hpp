/* SPDX-License-Identifier: MIT */
// dw_i2c.hpp - Synopsys DesignWare I2C master (AMD AMDI0010/AMDI0019, Intel LPSS, ...), polling mode.
#pragma once
#include <stdint.h>

namespace aos::i2c {

struct Bus {
    uint64_t base = 0;
    uint32_t rx_depth = 8;
    bool init(uint64_t mmio_base);                                    // false if no DesignWare IP at base
    // write wl bytes, then (repeated START) read rl bytes. Either may be 0.
    bool xfer(uint8_t addr, const uint8_t* w, uint32_t wl, uint8_t* r, uint32_t rl);
};

}  // namespace aos::i2c
