/* SPDX-License-Identifier: MIT */
// pciutil.hpp - tiny C++ helpers on top of network/pci.c (config space access).
#pragma once
#include <stdint.h>

extern "C" {
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
}

namespace aos::pcix {

struct Dev {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint32_t cls;                 // class<<16 | subclass<<8 | prog-if
};

inline uint32_t rd(const Dev& d, uint8_t off) { return pci_config_read(d.bus, d.slot, d.func, off); }
inline void     wr(const Dev& d, uint8_t off, uint32_t v) { pci_config_write(d.bus, d.slot, d.func, off, v); }

template <class F>
inline void scan(F f) {
    for (unsigned bus = 0; bus < 256; ++bus) {
        for (unsigned slot = 0; slot < 32; ++slot) {
            const uint32_t id0 = pci_config_read(bus, slot, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            const uint32_t hdr = pci_config_read(bus, slot, 0, 0x0C);
            const unsigned nf = ((hdr >> 16) & 0x80) ? 8 : 1;
            for (unsigned fn = 0; fn < nf; ++fn) {
                const uint32_t id = pci_config_read(bus, slot, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                Dev d{static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(fn),
                      static_cast<uint16_t>(id & 0xFFFF), static_cast<uint16_t>(id >> 16),
                      pci_config_read(bus, slot, fn, 8) >> 8};
                f(d);
            }
        }
    }
}

// Memory BAR -> physical base (handles 64-bit BARs). False for I/O BARs or unassigned (0) BARs.
inline bool bar_mem(const Dev& d, int idx, uint64_t* base) {
    const uint32_t lo = rd(d, static_cast<uint8_t>(0x10 + 4 * idx));
    if (lo & 1) return false;
    uint64_t b = lo & ~0xFULL;
    if (((lo >> 1) & 3) == 2) b |= static_cast<uint64_t>(rd(d, static_cast<uint8_t>(0x14 + 4 * idx))) << 32;
    if (!b) return false;
    *base = b;
    return true;
}

inline void enable(const Dev& d) {   // memory space + bus mastering (status bits left untouched)
    const uint32_t cmd = rd(d, 4) & 0xFFFF;
    wr(d, 4, cmd | 0x6);
}

}  // namespace aos::pcix
