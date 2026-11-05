/* SPDX-License-Identifier: MIT */
// i2c_hid.cpp - HID over I2C (Microsoft spec) on top of the DesignWare master.
// BEST EFFORT: needs real hardware to validate. The touchpad's GPIO interrupt is not used - the device
// is read by polling (a zero length answer means "no report pending").
#include "i2c_hid.hpp"
#include "acpi.hpp"
#include "dw_i2c.hpp"
#include "../input/hid.hpp"
#include "../pciutil.hpp"
#include "../timeutil.hpp"
#include "../../memory/virtual/vmm.hpp"

extern "C" {
void aos_serial_write(const char* text);
void aos_serial_write_hex64(uint64_t value);
}

namespace aos::i2c {
namespace {

void log(const char* s) { aos_serial_write("[i2c] "); aos_serial_write(s); aos_serial_write("\r\n"); }
void logv(const char* s, uint64_t v) { aos_serial_write("[i2c] "); aos_serial_write(s); aos_serial_write(" "); aos_serial_write_hex64(v); aos_serial_write("\r\n"); }

constexpr int kMaxDev = 3, kMaxBus = 6;
constexpr uint8_t kSrcBase = 6;                         // pointer sources 6,7 (USB uses 1..5)

struct Dev {
    bool ok = false;
    Bus* bus = nullptr;
    uint8_t addr = 0;
    uint16_t cmd_reg = 0, data_reg = 0, in_reg = 0, max_in = 0;
    uint32_t last_poll = 0;
    hid::PointerDevice pd;
    uint8_t buf[130];
};

Dev g_dev[kMaxDev];
int g_ndev = 0;
Bus g_bus[kMaxBus];
int g_nbus = 0;
bool g_calibrated = false;

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool reg_read(Dev& d, uint16_t reg, uint8_t* out, uint32_t len) {
    const uint8_t w[2] = {static_cast<uint8_t>(reg), static_cast<uint8_t>(reg >> 8)};
    return d.bus->xfer(d.addr, w, 2, out, len);
}

bool command(Dev& d, uint8_t opcode, uint8_t arg) {
    const uint8_t w[4] = {static_cast<uint8_t>(d.cmd_reg), static_cast<uint8_t>(d.cmd_reg >> 8), arg, opcode};
    return d.bus->xfer(d.addr, w, 4, nullptr, 0);
}

bool set_feature(Dev& d, uint8_t rid, const uint8_t* data, uint32_t len) {      // data already includes the id byte
    uint8_t w[40];
    uint32_t n = 0;
    w[n++] = static_cast<uint8_t>(d.cmd_reg); w[n++] = static_cast<uint8_t>(d.cmd_reg >> 8);
    w[n++] = static_cast<uint8_t>((3u << 4) | (rid >= 0xF ? 0xF : rid));          // report type: feature
    w[n++] = 0x03;                                                                 // SET_REPORT
    if (rid >= 0xF) w[n++] = rid;
    w[n++] = static_cast<uint8_t>(d.data_reg); w[n++] = static_cast<uint8_t>(d.data_reg >> 8);
    const uint32_t size = 2 + len;
    w[n++] = static_cast<uint8_t>(size); w[n++] = static_cast<uint8_t>(size >> 8);
    if (n + len > sizeof w) return false;
    for (uint32_t i = 0; i < len; ++i) w[n++] = data[i];
    return d.bus->xfer(d.addr, w, n, nullptr, 0);
}

bool bring_up(Dev& d, int idx) {
    // 1. HID descriptor: its register comes from an ACPI _DSM we don't evaluate -> probe the usual suspects
    static const uint16_t cand[] = {0x0001, 0x0020, 0x0000, 0x0010, 0x0021, 0x0030, 0x0002, 0x0100};
    uint8_t hd[30];
    bool found = false;
    for (uint16_t reg : cand) {
        if (!reg_read(d, reg, hd, 30)) continue;
        if (le16(hd) == 30 && le16(hd + 2) == 0x0100) { found = true; break; }
    }
    if (!found) { log("no HID descriptor answered"); return false; }
    const uint16_t rdesc_len = le16(hd + 4), rdesc_reg = le16(hd + 6);
    d.in_reg = le16(hd + 8); d.max_in = le16(hd + 10);
    d.cmd_reg = le16(hd + 16); d.data_reg = le16(hd + 18);
    logv("HID-I2C device, vid:pid", (static_cast<uint64_t>(le16(hd + 20)) << 16) | le16(hd + 22));
    if (d.max_in < 2 || d.max_in > sizeof d.buf || rdesc_len == 0 || rdesc_len > 4096) { log("bad descriptor sizes"); return false; }

    command(d, 0x08, 0x00);                                    // SET_POWER(on)
    tm::delay_ms(10);
    command(d, 0x01, 0x00);                                    // RESET
    tm::delay_ms(100);
    for (int i = 0; i < 20; ++i) {                             // swallow the reset-complete (zero length) report
        uint8_t t[2];
        if (d.bus->xfer(d.addr, nullptr, 0, t, 2) && le16(t) == 0) break;
        tm::delay_ms(10);
    }

    static uint8_t rd[4096];
    if (!reg_read(d, rdesc_reg, rd, rdesc_len)) { log("report descriptor read failed"); return false; }
    d.pd.set_source(static_cast<uint8_t>(kSrcBase + idx));
    if (!d.pd.parse(rd, rdesc_len)) { log("not a pointing device"); return false; }
    for (int k = 0; k < d.pd.feature_count(); ++k) {            // Precision Touchpad -> multi-touch reports
        uint8_t fb[16], rid = 0;
        const int fl = d.pd.build_feature(k, fb, sizeof fb, &rid);
        if (fl) set_feature(d, rid, fb, static_cast<uint32_t>(fl));
    }
    log(d.pd.is_touchpad() ? "touchpad ready" : "pointer ready");
    return true;
}

Bus* bus_for(const acpi::I2cHidInfo& info) {
    uint64_t mmio = info.bus_mmio;
    if (!mmio) {                                               // Intel LPSS: PCI function, found by DesignWare magic
        const int want = info.bus_name[3] >= '0' && info.bus_name[3] <= '9' ? info.bus_name[3] - '0' : 0;
        int seen = 0;
        pcix::scan([&](const pcix::Dev& p) {
            if (mmio || p.vendor != 0x8086) return;
            const uint32_t c = p.cls >> 8;
            if (c != 0x1180 && c != 0x0C80) return;
            uint64_t b;
            if (!pcix::bar_mem(p, 0, &b)) return;
            pcix::enable(p);
            if (!vmm::map_mmio(b, 0x1000, vmm::CacheMode::Uncached)) return;
            if (*reinterpret_cast<volatile uint32_t*>(b + 0xFC) != 0x44570140) return;
            if (seen++ == want) mmio = b;
        });
    }
    if (!mmio) return nullptr;
    for (int i = 0; i < g_nbus; ++i) if (g_bus[i].base == mmio) return &g_bus[i];
    if (g_nbus >= kMaxBus) return nullptr;
    if (!vmm::map_mmio(mmio, 0x1000, vmm::CacheMode::Uncached)) return nullptr;
    Bus& b = g_bus[g_nbus];
    if (!b.init(mmio)) return nullptr;
    ++g_nbus;
    return &b;
}

}  // namespace

bool init(const uint8_t* mbi) {
    if (!g_calibrated) { tm::calibrate(); g_calibrated = true; }
    if (!acpi::init(mbi)) { log("no ACPI tables"); return false; }
    acpi::I2cHidInfo info[6];
    const int n = acpi::find_i2c_hid(info, 6);
    logv("ACPI I2C-HID candidates:", static_cast<uint64_t>(n));
    for (int i = 0; i < n && g_ndev < kMaxDev; ++i) {
        logv("  candidate addr", info[i].addr);
        Bus* bus = bus_for(info[i]);
        if (!bus) { log("  controller not usable"); continue; }
        Dev& d = g_dev[g_ndev];
        d.ok = false;
        d.bus = bus; d.addr = static_cast<uint8_t>(info[i].addr);
        if (!bring_up(d, g_ndev)) continue;
        d.ok = true;
        ++g_ndev;
    }
    return g_ndev > 0;
}

void poll() {
    for (int i = 0; i < g_ndev; ++i) {
        Dev& d = g_dev[i];
        if (!d.ok) continue;
        const uint32_t now = tm::ms();
        d.pd.tick(now);
        if (now - d.last_poll < 8) continue;
        d.last_poll = now;
        if (!d.bus->xfer(d.addr, nullptr, 0, d.buf, d.max_in)) continue;
        const uint16_t len = le16(d.buf);
        if (len < 3 || len == 0xFFFF || len > d.max_in) continue;       // 0 = nothing pending
        d.pd.handle(d.buf + 2, len - 2u, now);
    }
}

int device_count() { return g_ndev; }

}  // namespace aos::i2c
