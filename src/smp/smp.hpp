// smp.hpp - application processor start-up (build with SMP=1).
#pragma once
#include <stdint.h>
namespace aos::smp {
void start(const uint8_t* multiboot_info);   // INIT-SIPI every enabled CPU from the MADT and wait for it
unsigned online();
}
