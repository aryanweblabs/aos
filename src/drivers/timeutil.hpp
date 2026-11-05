/* SPDX-License-Identifier: MIT */
// timeutil.hpp - TSC based millisecond clock + busy delays (calibrated against the ~90 Hz PIT tick).
#pragma once
#include <stdint.h>
#include "../arch/x86_64/interrupts.hpp"

namespace aos::tm {

inline uint64_t g_tsc_per_ms = 2000000;   // fallback: 2 GHz

inline uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Calibrate the TSC against PIT channel 2 (works with interrupts OFF, takes ~20 ms).
// Falls back to the IRQ0 tick if the PIT gate does not respond, then to a 2 GHz guess.
inline void outb_(uint16_t p, uint8_t v) { asm volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
inline uint8_t inb_(uint16_t p) { uint8_t v; asm volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

inline void calibrate() {
#ifdef AOS_HOST_TEST
    g_tsc_per_ms = 2000000;                                      // host unit tests: no port I/O
    return;
#endif
    uint8_t gate = inb_(0x61);
    outb_(0x61, static_cast<uint8_t>((gate & 0xFD) | 1));        // gate on, speaker off
    outb_(0x43, 0xB0);                                           // ch2, lo/hi, mode 0 (one-shot)
    outb_(0x42, 0x38); outb_(0x42, 0x5D);                        // 0x5D38 = 23864 ticks = 20 ms
    const uint8_t p = inb_(0x61) & 0xFE;
    outb_(0x61, p); outb_(0x61, static_cast<uint8_t>(p | 1));    // restart the counter
    const uint64_t a = rdtsc();
    uint64_t guard = 0;
    while (!(inb_(0x61) & 0x20) && ++guard < 50000000ULL) {}
    const uint64_t b = rdtsc();
    outb_(0x61, gate);
    if (guard < 50000000ULL) {
        const uint64_t v = (b - a) / 20;
        if (v >= 100000 && v < 20000000) { g_tsc_per_ms = v; return; }
    }
    // fallback: measure against the 90 Hz IRQ0 tick (needs interrupts enabled)
    const uint64_t t = timer_ticks();
    guard = 0;
    while (timer_ticks() == t && ++guard < 300000000ULL) asm volatile("pause");
    if (timer_ticks() == t) return;
    const uint64_t a2 = rdtsc(), t0 = timer_ticks();
    guard = 0;
    while (timer_ticks() < t0 + 9 && ++guard < 600000000ULL) asm volatile("pause");
    const uint64_t b2 = rdtsc(), dt = timer_ticks() - t0;
    if (!dt) return;
    const uint64_t v = (b2 - a2) * 90 / (dt * 1000);
    if (v >= 100000) g_tsc_per_ms = v;
}

inline uint32_t ms() { return static_cast<uint32_t>(rdtsc() / g_tsc_per_ms); }

inline void delay_us(uint32_t us) {
    const uint64_t end = rdtsc() + g_tsc_per_ms * us / 1000;
    while (rdtsc() < end) asm volatile("pause");
}
inline void delay_ms(uint32_t m) { delay_us(m * 1000); }

}  // namespace aos::tm
