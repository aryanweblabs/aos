/* SPDX-License-Identifier: MIT */
// backlight.cpp - Intel iGPU backlight PWM (PCH_BLC_PWM_CTL2 @ 0xC8254 on Ivy Bridge..Alder Lake PCHs, BLC_PWM_CTL @ 0x61254 on older).
// Layout of both: bits 31:16 = PWM period, bits 15:0 = duty. We only ever change duty and only when the BIOS already enabled and
// programmed the PWM (non-zero period), so we can never turn on a backlight that is not wired.
#include "backlight.hpp"
#include "pciutil.hpp"
#include "../memory/virtual/vmm.hpp"

extern "C" void aos_serial_write(const char* text);

namespace aos::backlight {
namespace {
volatile uint32_t* g_reg = nullptr;
uint32_t g_period = 0; bool g_ok = false; int g_pct = 100;
}

bool init() {
    g_ok = false;
    pcix::scan([](const pcix::Dev& d) {
        if (g_ok || d.vendor != 0x8086 || (d.cls >> 16) != 0x03) return;
        uint64_t bar;
        if (!pcix::bar_mem(d, 0, &bar)) return;
        pcix::enable(d);
        if (!vmm::map_mmio(bar, 0x100000, vmm::CacheMode::Uncached)) return;
        static const uint32_t offs[2] = {0xC8254, 0x61254};
        for (int i = 0; i < 2 && !g_ok; ++i) {
            volatile uint32_t* r = reinterpret_cast<volatile uint32_t*>(bar + offs[i]);
            const uint32_t v = *r, period = v >> 16, duty = v & 0xFFFF;
            if (period < 16 || period == 0xFFFF || duty > period) continue;     // not programmed by the BIOS
            g_reg = r; g_period = period; g_ok = true;
            g_pct = static_cast<int>(duty * 100 / period);
        }
    });
    if (g_ok) aos_serial_write("[backlight] Intel PWM backlight found\r\n");
    return g_ok;
}
bool available() { return g_ok; }
int get() { return g_pct; }
void set(int pct) {
    if (!g_ok) return;
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    g_pct = pct;
    const uint32_t duty = g_period * static_cast<uint32_t>(pct) / 100;
    *g_reg = (g_period << 16) | duty;
}
}  // namespace aos::backlight
