/* SPDX-License-Identifier: MIT */
// xhci.hpp - USB 3.x / 2.0 host controller (xHCI) driver with HID mouse / touchpad / tablet support.
#pragma once
#include <stdint.h>

namespace aos::usb {
bool init();            // find xHCI controllers, enumerate root-port devices, start HID pointer devices
void poll();            // call from the main loop: events, hot-plug, gesture timers
int  controller_count();
int  pointer_count();   // number of active USB pointer devices
}
