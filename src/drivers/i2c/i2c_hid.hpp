/* SPDX-License-Identifier: MIT */
// i2c_hid.hpp - HID-over-I2C (PNP0C50) touchpads / touchscreens: ACPI discovery + polling reader.
#pragma once
#include <stdint.h>

namespace aos::i2c {
bool init(const uint8_t* multiboot_info);   // find devices via ACPI, bring them up
void poll();                                // call from the main loop
int  device_count();
}
