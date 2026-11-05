/* SPDX-License-Identifier: MIT */
#include "power.hpp"
#include "i2c/acpi.hpp"
#include "../filesystem/ramfs.hpp"
#include <stdint.h>

namespace aos::power {
namespace { inline void outb_(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); } }

[[noreturn]] void shutdown() {
    fs::sync();
    asm volatile("cli");
    acpi::poweroff();
    for (;;) asm volatile("hlt");          // firmware refused: halted, safe to remove power
}

[[noreturn]] void restart() {
    fs::sync();
    asm volatile("cli");
    for (int i = 0; i < 100000; ++i) { if (!(0x02 & ({ uint8_t s; asm volatile("inb %1, %0" : "=a"(s) : "Nd"((uint16_t)0x64)); s; }))) break; }
    outb_(0x64, 0xFE);                     // keyboard-controller reset
    outb_(0xCF9, 0x06);                    // PCI reset as fallback
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) idt = {0, 0};
    asm volatile("lidt %0; int3" : : "m"(idt));   // triple fault
    for (;;) asm volatile("hlt");
}
}  // namespace aos::power
