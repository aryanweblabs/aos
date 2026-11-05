/* SPDX-License-Identifier: MIT */
// acpi.hpp - just enough ACPI to find I2C-HID devices without a full AML interpreter.
#pragma once
#include <stdint.h>

namespace aos::acpi {

struct I2cHidInfo {
    uint16_t addr;            // 7-bit slave address
    char     bus_name[5];     // ACPI name of the I2C controller, e.g. "I2C1"
    uint64_t bus_mmio;        // controller MMIO base (0 = not found in ACPI, try PCI)
};

bool init(const uint8_t* multiboot_info);                 // locate RSDP/XSDT, remember DSDT + SSDTs
int  find_i2c_hid(I2cHidInfo* out, int max);              // PNP0C50 / ACPI0C50 devices

void poweroff();                                          // best-effort ACPI S5 (+ QEMU/Bochs/VBox ports); returns if it failed

}  // namespace aos::acpi
