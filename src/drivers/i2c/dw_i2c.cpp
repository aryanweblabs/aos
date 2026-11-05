/* SPDX-License-Identifier: MIT */
#include "dw_i2c.hpp"
#include "../timeutil.hpp"

namespace aos::i2c {
namespace {
enum : uint32_t {
    CON = 0x00, TAR = 0x04, DATA = 0x10, FS_H = 0x1C, FS_L = 0x20, INTR_MASK = 0x30, RAW = 0x34, RX_TL = 0x38,
    TX_TL = 0x3C, CLR_INTR = 0x40, CLR_ABRT = 0x54, ENABLE = 0x6C, STATUS = 0x70, ABRT_SRC = 0x80,
    ENABLE_STATUS = 0x9C, COMP_PARAM = 0xF4, COMP_TYPE = 0xFC,
};
inline uint32_t rd(uint64_t b, uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(b + o); }
inline void wr(uint64_t b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(b + o) = v; }

bool set_enabled(uint64_t b, bool on) {
    wr(b, ENABLE, on ? 1 : 0);
    const uint32_t t0 = tm::ms();
    while (((rd(b, ENABLE_STATUS) & 1) != 0) != on) {
        if (tm::ms() - t0 > 50) return false;
        tm::delay_us(20);
    }
    return true;
}
}  // namespace

bool Bus::init(uint64_t mmio) {
    base = mmio;
    if (rd(base, COMP_TYPE) != 0x44570140) return false;               // "DW" component magic
    const uint32_t param = rd(base, COMP_PARAM);
    rx_depth = ((param >> 8) & 0xFF) + 1;
    if (!set_enabled(base, false)) return false;
    wr(base, CON, 0x65);                                               // master, fast mode, restart on, slave off
    wr(base, INTR_MASK, 0);
    wr(base, RX_TL, 0);
    wr(base, TX_TL, 0);
    if (rd(base, FS_H) == 0 || rd(base, FS_L) == 0) { wr(base, FS_H, 90); wr(base, FS_L, 160); }   // ~400 kHz @ 100 MHz
    rd(base, CLR_INTR);
    return true;
}

bool Bus::xfer(uint8_t addr, const uint8_t* w, uint32_t wl, uint8_t* r, uint32_t rl) {
    const uint32_t total = wl + rl;
    if (!total) return true;
    if (!set_enabled(base, false)) return false;
    wr(base, TAR, addr & 0x7F);
    if (!set_enabled(base, true)) return false;
    rd(base, CLR_INTR);

    uint32_t sent = 0, got = 0;
    const uint32_t t0 = tm::ms();
    uint32_t window = rx_depth > 1 ? rx_depth - 1 : 1;
    if (window > 8) window = 8;
    while (sent < total || got < rl) {
        while (sent < total && (rd(base, STATUS) & 2)) {               // TX FIFO not full
            const bool last = sent == total - 1;
            uint32_t cmd;
            if (sent < wl) cmd = w[sent] | (last ? (1u << 9) : 0u);
            else cmd = (1u << 8) | ((sent == wl && wl > 0) ? (1u << 10) : 0u) | (last ? (1u << 9) : 0u);
            if (sent >= wl && (sent - wl) - got >= window) break;      // don't overrun the RX FIFO
            wr(base, DATA, cmd);
            ++sent;
        }
        while (got < rl && (rd(base, STATUS) & 8)) r[got++] = static_cast<uint8_t>(rd(base, DATA));
        if (rd(base, RAW) & (1u << 6)) {                               // TX_ABRT (NAK, arbitration lost...)
            rd(base, CLR_ABRT);
            set_enabled(base, false);
            return false;
        }
        if (tm::ms() - t0 > 100) { set_enabled(base, false); return false; }
    }
    const uint32_t t1 = tm::ms();
    while (rd(base, STATUS) & 1) {                                     // wait for STOP / bus idle
        if (tm::ms() - t1 > 20) break;
        tm::delay_us(20);
    }
    return true;
}

}  // namespace aos::i2c
