/* SPDX-License-Identifier: MIT */
// xhci.cpp - polling xHCI driver. No interrupts needed: the event ring is drained from usb::poll().
// Scope: root-port devices (no hubs yet), control + interrupt-IN transfers, HID pointing devices.
#include "xhci.hpp"
#include "../input/hid.hpp"
#include "../pciutil.hpp"
#include "../timeutil.hpp"
#include "../ps2_mouse.hpp"
#include "../../memory/physical/pmm.h"
#include "../../memory/virtual/vmm.hpp"

extern "C" {
void aos_serial_write(const char* text);
void aos_serial_write_hex64(uint64_t value);
void* memset(void* s, int c, unsigned long n);
void* memcpy(void* d, const void* s, unsigned long n);
}

namespace aos::usb {
namespace {

// ---- logging ------------------------------------------------------------------------------
void log(const char* s) { aos_serial_write("[usb] "); aos_serial_write(s); aos_serial_write("\r\n"); }
void logv(const char* s, uint64_t v) {
    aos_serial_write("[usb] "); aos_serial_write(s); aos_serial_write(" "); aos_serial_write_hex64(v); aos_serial_write("\r\n");
}

// ---- MMIO / DMA helpers -------------------------------------------------------------------
inline uint32_t rd32(uint64_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
inline void wr32(uint64_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(a) = v; }
inline void wr64(uint64_t a, uint64_t v) { wr32(a, static_cast<uint32_t>(v)); wr32(a + 4, static_cast<uint32_t>(v >> 32)); }
inline void barrier() { asm volatile("mfence" ::: "memory"); }

void* dma_alloc(unsigned pages, uint64_t* phys) {
    const uint64_t p = pmm_alloc_contiguous(pages);
    if (!p || p >= 0x100000000ULL) return nullptr;       // identity mapping covers the low 4 GiB only
    memset(reinterpret_cast<void*>(p), 0, pages * 4096UL);
    *phys = p;
    return reinterpret_cast<void*>(p);
}
void dma_free(void* p, unsigned pages) {
    if (!p) return;
    for (unsigned i = 0; i < pages; ++i) pmm_free_frame(reinterpret_cast<uint64_t>(p) + i * 4096ULL);
}

// ---- constants ----------------------------------------------------------------------------
enum : uint32_t {
    CMD_RS = 1, CMD_HCRST = 2, STS_HCH = 1, STS_CNR = 1u << 11,
    PS_CCS = 1, PS_PED = 2, PS_PR = 1u << 4, PS_PP = 1u << 9, PS_CHANGE = 0x7Fu << 17, PS_PRC = 1u << 21,
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS = 11, TRB_CONFIG_EP = 12, TRB_EVAL_CTX = 13,
    TRB_RESET_EP = 14, TRB_SET_DEQ = 16,
    EV_TRANSFER = 32, EV_CMD_DONE = 33, EV_PORT = 34,
    CC_SUCCESS = 1, CC_SHORT = 13,
};
constexpr int kMaxCtrl = 4, kMaxDev = 32, kMaxEps = 3, kPool = 5;
constexpr uint32_t kEvRing = 256;

struct Trb { uint64_t param; uint32_t status; uint32_t control; };
struct Ring { volatile Trb* t; uint64_t phys; uint32_t n, enq, cyc; };

// Returns the physical address of the TRB just written.
uint64_t ring_push(Ring& r, uint64_t param, uint32_t status, uint32_t control) {
    volatile Trb& x = r.t[r.enq];
    const uint64_t at = r.phys + static_cast<uint64_t>(r.enq) * 16;
    x.param = param;
    x.status = status;
    barrier();
    x.control = (control & ~1u) | r.cyc;
    barrier();
    if (++r.enq == r.n - 1) {                             // last slot is the link TRB
        volatile Trb& l = r.t[r.enq];
        l.control = (l.control & ~1u) | r.cyc;
        r.enq = 0;
        r.cyc ^= 1;
    }
    return at;
}

bool ring_init(Ring& r, uint32_t n) {
    uint64_t ph;
    void* mem = dma_alloc(1, &ph);
    if (!mem) return false;
    r.t = static_cast<volatile Trb*>(mem);
    r.phys = ph; r.n = n; r.enq = 0; r.cyc = 1;
    r.t[n - 1].param = ph;
    r.t[n - 1].status = 0;
    r.t[n - 1].control = (TRB_LINK << 10) | (1u << 1);    // toggle-cycle link back to the start
    return true;
}

struct Ep {
    bool active = false, need_recover = false;
    uint8_t dci = 0, iface = 0;
    uint16_t mps = 0, slot_sz = 0, count = 0, req_len = 0;
    uint8_t interval = 0;
    Ring ring{};
    uint8_t* buf = nullptr; uint64_t buf_phys = 0;
    hid::PointerDevice* pd = nullptr;
    int pool_idx = -1;
};

struct Dev {
    bool used = false;
    uint8_t slot = 0, port = 0, speed = 0;
    uint8_t* out = nullptr; uint64_t out_phys = 0;
    uint8_t* in = nullptr;  uint64_t in_phys = 0;
    Ring ep0{};
    Ep ep[kMaxEps];
};

struct Ctrl {
    bool ok = false;
    pcix::Dev pci{};
    uint64_t cap = 0, op = 0, rt = 0, db = 0;
    uint32_t ctxsz = 0, maxslots = 0, maxports = 0;
    uint64_t* dcbaa = nullptr; uint64_t dcbaa_phys = 0;
    Ring cmd{};
    volatile Trb* ev = nullptr; uint64_t ev_phys = 0; uint32_t ev_deq = 0, ev_cyc = 0;
    volatile bool cmd_done = false; uint32_t cmd_cc = 0, cmd_slot = 0;
    volatile bool xfer_done = false; uint32_t xfer_cc = 0; uint64_t xfer_trb = 0; uint8_t xfer_slot = 0;
    uint64_t port_changed = 0;
    uint8_t* ctl_buf = nullptr; uint64_t ctl_phys = 0;
    Dev dev[kMaxDev];
};

Ctrl g_ctrl[kMaxCtrl];
int g_nctrl = 0;
hid::PointerDevice g_pool[kPool];
bool g_pool_used[kPool];
bool g_calibrated = false;

inline uint64_t portsc_addr(const Ctrl& c, uint32_t port) { return c.op + 0x400 + 0x10ULL * (port - 1); }
inline uint32_t port_neutral(uint32_t ps) { return ps & PS_PP; }   // keep power, never write RW1C bits back

bool wait_for(uint64_t reg, uint32_t mask, uint32_t val, uint32_t timeout_ms) {
    const uint32_t t0 = tm::ms();
    while ((rd32(reg) & mask) != val) {
        if (tm::ms() - t0 > timeout_ms) return false;
        tm::delay_us(50);
    }
    return true;
}

// ---- event ring ---------------------------------------------------------------------------
void events(Ctrl& c);

bool wait_cmd(Ctrl& c, uint32_t timeout_ms) {
    const uint32_t t0 = tm::ms();
    while (!c.cmd_done) {
        events(c);
        if (tm::ms() - t0 > timeout_ms) return false;
    }
    return true;
}

bool command(Ctrl& c, uint64_t param, uint32_t status, uint32_t control, uint32_t* slot_out = nullptr) {
    c.cmd_done = false;
    ring_push(c.cmd, param, status, control);
    wr32(c.db, 0);
    if (!wait_cmd(c, 1500)) { log("command timeout"); return false; }
    if (slot_out) *slot_out = c.cmd_slot;
    if (c.cmd_cc != CC_SUCCESS) { logv("command failed, cc=", c.cmd_cc); return false; }
    return true;
}

void queue_int_trbs(Ctrl& c, Dev& d, Ep& e) {
    for (uint32_t i = 0; i < e.count; ++i)
        ring_push(e.ring, e.buf_phys + static_cast<uint64_t>(i) * e.slot_sz, e.req_len,
                  (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));          // IOC + ISP
    barrier();
    wr32(c.db + 4ULL * d.slot, e.dci);
}

void events(Ctrl& c) {
    bool any = false;
    for (;;) {
        volatile Trb& e = c.ev[c.ev_deq];
        const uint32_t ctl = e.control;
        if ((ctl & 1) != c.ev_cyc) break;
        barrier();
        const uint64_t param = e.param;
        const uint32_t status = e.status;
        const uint32_t type = (ctl >> 10) & 63;

        if (type == EV_CMD_DONE) {
            c.cmd_cc = status >> 24;
            c.cmd_slot = ctl >> 24;
            c.cmd_done = true;
        } else if (type == EV_PORT) {
            const uint32_t port = static_cast<uint32_t>(param >> 24) & 0xFF;
            if (port >= 1 && port <= 63) c.port_changed |= 1ULL << port;
        } else if (type == EV_TRANSFER) {
            const uint32_t slot = ctl >> 24, dci = (ctl >> 16) & 31, cc = status >> 24;
            if (slot > 0 && slot < kMaxDev && c.dev[slot].used) {
                Dev& d = c.dev[slot];
                if (dci == 1) {
                    if (param == c.xfer_trb || (cc != CC_SUCCESS && cc != CC_SHORT)) {
                        c.xfer_cc = cc;
                        c.xfer_done = true;
                    }
                } else {
                    for (Ep& ep : d.ep) {
                        if (!ep.active || ep.dci != dci) continue;
                        if (cc == CC_SUCCESS || cc == CC_SHORT) {
                            const uint64_t pos = (param - ep.ring.phys) / 16;
                            if (pos < ep.count) {
                                const uint32_t got = ep.req_len - (status & 0xFFFFFF);
                                if (got) ep.pd->handle(ep.buf + pos * ep.slot_sz, got, tm::ms());
                                ring_push(ep.ring, ep.buf_phys + pos * ep.slot_sz, ep.req_len,
                                          (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
                            }
                        } else {
                            ep.need_recover = true;     // halted endpoint: fixed up in poll()
                        }
                        break;
                    }
                }
            }
        }
        if (++c.ev_deq == kEvRing) { c.ev_deq = 0; c.ev_cyc ^= 1; }
        any = true;
    }
    if (any) wr64(c.rt + 0x38, (c.ev_phys + static_cast<uint64_t>(c.ev_deq) * 16) | 8);   // ERDP, clear EHB
}

// ---- control transfers on EP0 -------------------------------------------------------------
bool recover_endpoint(Ctrl& c, Dev& d, uint32_t dci, Ring& ring, bool rebuild) {
    command(c, 0, 0, (TRB_RESET_EP << 10) | (dci << 16) | (static_cast<uint32_t>(d.slot) << 24));
    if (rebuild) {
        for (uint32_t i = 0; i < ring.n - 1; ++i) { ring.t[i].param = 0; ring.t[i].status = 0; ring.t[i].control = 0; }
        ring.enq = 0; ring.cyc = 1;
    }
    return command(c, (ring.phys + static_cast<uint64_t>(ring.enq) * 16) | ring.cyc, 0,
                   (TRB_SET_DEQ << 10) | (dci << 16) | (static_cast<uint32_t>(d.slot) << 24));
}

bool ctl(Ctrl& c, Dev& d, uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, void* data, uint16_t len) {
    const bool in = rt & 0x80;
    if (len > 4096) return false;
    if (len && !in) memcpy(c.ctl_buf, data, len);
    const uint64_t setup = rt | (static_cast<uint64_t>(rq) << 8) | (static_cast<uint64_t>(val) << 16) |
                           (static_cast<uint64_t>(idx) << 32) | (static_cast<uint64_t>(len) << 48);
    const uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    c.xfer_done = false;
    ring_push(d.ep0, setup, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));      // IDT
    if (len) ring_push(d.ep0, c.ctl_phys, len, (TRB_DATA << 10) | (in ? ((1u << 16) | (1u << 2)) : 0u));
    c.xfer_trb = ring_push(d.ep0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));
    c.xfer_slot = d.slot;
    barrier();
    wr32(c.db + 4ULL * d.slot, 1);

    const uint32_t t0 = tm::ms();
    while (!c.xfer_done) {
        events(c);
        if (tm::ms() - t0 > 600) break;
    }
    if (!c.xfer_done || (c.xfer_cc != CC_SUCCESS && c.xfer_cc != CC_SHORT)) {
        logv("control transfer failed, cc=", c.xfer_done ? c.xfer_cc : 0xFFFF);
        recover_endpoint(c, d, 1, d.ep0, false);
        return false;
    }
    if (len && in) memcpy(data, c.ctl_buf, len);
    return true;
}

inline uint32_t* ctx_at(const Ctrl& c, uint8_t* base, uint32_t index) {
    return reinterpret_cast<uint32_t*>(base + static_cast<uint64_t>(index) * c.ctxsz);
}

uint32_t ep_interval(uint8_t speed, uint8_t b) {
    if (speed >= 3) { uint32_t v = b ? b - 1u : 0u; return v > 15 ? 15 : v; }
    uint32_t ms = b ? b : 1u, v = 3;                        // LS/FS: bInterval in ms -> 2^v * 125 us
    while ((1u << (v - 3)) < ms && v < 10) ++v;
    return v;
}

// ---- device lifetime ----------------------------------------------------------------------
void free_dev(Ctrl& c, Dev& d, bool disable_slot) {
    for (Ep& e : d.ep) {
        if (e.pool_idx >= 0) {
            mouse_push(static_cast<uint8_t>(e.pool_idx + 1), 0, 0, 0, 0);     // release stuck buttons
            g_pool_used[e.pool_idx] = false;
        }
        dma_free(const_cast<Trb*>(e.ring.t), 1);
        dma_free(e.buf, 2);
        e = Ep{};
    }
    if (disable_slot && d.slot) {
        command(c, 0, 0, (TRB_DISABLE_SLOT << 10) | (static_cast<uint32_t>(d.slot) << 24));
        c.dcbaa[d.slot] = 0;
    }
    dma_free(d.out, 1); dma_free(d.in, 1); dma_free(const_cast<Trb*>(d.ep0.t), 1);
    d = Dev{};
}

bool enumerate(Ctrl& c, uint32_t port) {
    const uint64_t psa = portsc_addr(c, port);
    uint32_t ps = rd32(psa);
    if (!(ps & PS_CCS)) return false;
    wr32(psa, port_neutral(ps) | PS_CHANGE);                               // ack change bits
    if (!(ps & PS_PED)) {                                                  // USB2 ports need a reset to enable
        wr32(psa, port_neutral(ps) | PS_PR);
        if (!wait_for(psa, PS_PRC, PS_PRC, 600)) { log("port reset timeout"); return false; }
        tm::delay_ms(20);
        ps = rd32(psa);
        wr32(psa, port_neutral(ps) | PS_CHANGE);
        tm::delay_ms(10);
    }
    ps = rd32(psa);
    if (!(ps & PS_CCS) || !(ps & PS_PED)) { logv("port not enabled, portsc=", ps); return false; }
    const uint32_t speed = (ps >> 10) & 15;
    if (!speed) return false;

    uint32_t slot = 0;
    if (!command(c, 0, 0, TRB_ENABLE_SLOT << 10, &slot) || slot == 0 || slot >= kMaxDev) return false;
    Dev& d = c.dev[slot];
    d = Dev{};
    d.used = true; d.slot = static_cast<uint8_t>(slot); d.port = static_cast<uint8_t>(port); d.speed = static_cast<uint8_t>(speed);
    d.out = static_cast<uint8_t*>(dma_alloc(1, &d.out_phys));
    d.in  = static_cast<uint8_t*>(dma_alloc(1, &d.in_phys));
    if (!d.out || !d.in || !ring_init(d.ep0, 256)) { free_dev(c, d, true); return false; }
    c.dcbaa[slot] = d.out_phys;

    uint32_t mps = speed == 4 ? 512 : (speed == 3 ? 64 : 8);               // LS/FS start at 8, fixed after 1st read
    {
        uint32_t* ic = ctx_at(c, d.in, 0);
        ic[1] = 3;                                                         // add slot + EP0
        uint32_t* sc = ctx_at(c, d.in, 1);
        sc[0] = (1u << 27) | (speed << 20);
        sc[1] = port << 16;
        uint32_t* e0 = ctx_at(c, d.in, 2);
        e0[1] = (3u << 1) | (4u << 3) | (mps << 16);
        e0[2] = static_cast<uint32_t>(d.ep0.phys) | 1;
        e0[3] = static_cast<uint32_t>(d.ep0.phys >> 32);
        e0[4] = 8;
    }
    if (!command(c, d.in_phys, 0, (TRB_ADDRESS << 10) | (slot << 24))) { free_dev(c, d, true); return false; }
    tm::delay_ms(10);

    uint8_t buf[256];
    if (!ctl(c, d, 0x80, 6, 0x0100, 0, buf, 8)) { free_dev(c, d, true); return false; }
    const uint32_t real_mps = speed == 4 ? (1u << buf[7]) : buf[7];
    if (real_mps && real_mps != mps) {
        uint32_t* ic = ctx_at(c, d.in, 0);
        ic[0] = 0; ic[1] = 2;                                              // evaluate EP0 only
        uint32_t* e0 = ctx_at(c, d.in, 2);
        e0[1] = (e0[1] & 0xFFFF) | (real_mps << 16);
        if (!command(c, d.in_phys, 0, (TRB_EVAL_CTX << 10) | (slot << 24))) { free_dev(c, d, true); return false; }
        mps = real_mps;
    }
    if (!ctl(c, d, 0x80, 6, 0x0100, 0, buf, 18)) { free_dev(c, d, true); return false; }
    logv("device attached: vid:pid", (static_cast<uint64_t>(buf[8] | buf[9] << 8) << 16) | (buf[10] | buf[11] << 8));
    const uint8_t dev_class = buf[4];
    if (dev_class == 9) { log("USB hub: hubs are not supported yet (plug the device into a root port)"); free_dev(c, d, true); return false; }

    // ---- configuration descriptor
    if (!ctl(c, d, 0x80, 6, 0x0200, 0, buf, 9)) { free_dev(c, d, true); return false; }
    uint32_t total = buf[2] | (buf[3] << 8);
    const uint8_t cfg_value = buf[5];
    if (total > 4000) total = 4000;
    static uint8_t cfg[4096];
    if (!ctl(c, d, 0x80, 6, 0x0200, 0, cfg, static_cast<uint16_t>(total))) { free_dev(c, d, true); return false; }

    struct If { uint8_t num, cls, sub, proto, ep_addr, interval; uint16_t mps, rdesc_len; } ifs[4];
    int nif = 0, cur = -1;
    for (uint32_t off = 0; off + 2 <= total && cfg[off] >= 2;) {
        const uint8_t* p = cfg + off;
        if (p[1] == 4) {
            cur = -1;
            if (p[3] == 0 && p[5] == 3 && nif < 4) { cur = nif++; ifs[cur] = If{p[2], p[5], p[6], p[7], 0, 0, 0, 0}; }
        } else if (p[1] == 0x21 && cur >= 0 && p[0] >= 9) {
            for (int k = 0; k < p[5] && 6 + 3 * k + 2 < p[0]; ++k)
                if (p[6 + 3 * k] == 0x22) ifs[cur].rdesc_len = static_cast<uint16_t>(p[7 + 3 * k] | (p[8 + 3 * k] << 8));
        } else if (p[1] == 5 && cur >= 0 && p[0] >= 7) {
            if ((p[2] & 0x80) && (p[3] & 3) == 3 && !ifs[cur].ep_addr) {
                ifs[cur].ep_addr = p[2]; ifs[cur].mps = static_cast<uint16_t>(p[4] | (p[5] << 8)); ifs[cur].interval = p[6];
            }
        }
        off += cfg[off];
    }
    if (nif == 0) { log("no HID interface"); free_dev(c, d, true); return false; }
    if (!ctl(c, d, 0x00, 9, cfg_value, 0, nullptr, 0)) { free_dev(c, d, true); return false; }

    // ---- per interface: report descriptor -> pointer device
    int nep = 0;
    static uint8_t rdesc[4096];
    for (int i = 0; i < nif && nep < kMaxEps; ++i) {
        If& f = ifs[i];
        if (!f.ep_addr || !f.rdesc_len || f.rdesc_len > 4096) continue;
        int pi = -1;
        for (int k = 0; k < kPool; ++k) if (!g_pool_used[k]) { pi = k; break; }
        if (pi < 0) { log("pointer pool full"); break; }
        if (!ctl(c, d, 0x81, 6, 0x2200, f.num, rdesc, f.rdesc_len)) continue;
        hid::PointerDevice& pd = g_pool[pi];
        pd.set_source(static_cast<uint8_t>(pi + 1));
        bool usable = pd.parse(rdesc, f.rdesc_len);
        if (!usable && f.sub == 1 && f.proto == 2) {                     // boot mouse fallback
            ctl(c, d, 0x21, 0x0B, 0, f.num, nullptr, 0);                // SET_PROTOCOL(boot)
            pd.use_boot_mouse();
            usable = true;
        } else if (usable && f.sub == 1) {
            ctl(c, d, 0x21, 0x0B, 1, f.num, nullptr, 0);                // SET_PROTOCOL(report)
        }
        if (!usable) continue;                                          // keyboard etc.
        ctl(c, d, 0x21, 0x0A, 0, f.num, nullptr, 0);                    // SET_IDLE(0): only report on change
        for (int k = 0; k < pd.feature_count(); ++k) {                  // Precision Touchpad: leave mouse emulation
            uint8_t fb[16], rid = 0;
            const int fl = pd.build_feature(k, fb, sizeof fb, &rid);
            if (fl) ctl(c, d, 0x21, 9, static_cast<uint16_t>((3 << 8) | rid), f.num, fb, static_cast<uint16_t>(fl));
        }
        Ep& e = d.ep[nep++];
        e.active = false;
        e.iface = f.num; e.dci = static_cast<uint8_t>((f.ep_addr & 15) * 2 + 1);
        e.mps = f.mps & 0x7FF; e.interval = static_cast<uint8_t>(ep_interval(speed, f.interval));
        if (e.mps == 0 || e.mps > 1024) { --nep; continue; }
        e.req_len = e.mps;
        uint16_t sz = 8;
        while (sz < e.mps) sz = static_cast<uint16_t>(sz * 2);          // power of two: buffers never straddle 64 KiB
        e.slot_sz = sz;
        e.count = static_cast<uint16_t>(8192 / sz); if (e.count > 128) e.count = 128;
        e.pd = &pd; e.pool_idx = pi; g_pool_used[pi] = true;
        e.buf = static_cast<uint8_t*>(dma_alloc(2, &e.buf_phys));
        if (!e.buf || !ring_init(e.ring, e.count + 1u)) { g_pool_used[pi] = false; dma_free(e.buf, 2); e = Ep{}; --nep; continue; }
        logv(pd.is_touchpad() ? "  touchpad on interface" : "  pointer on interface", f.num);
    }
    if (nep == 0) { log("no usable pointing device on this USB device"); free_dev(c, d, true); return false; }

    // ---- Configure Endpoint
    uint32_t maxdci = 1;
    uint32_t* ic = ctx_at(c, d.in, 0);
    memset(d.in, 0, 4096);
    ic[1] = 1;
    for (int i = 0; i < nep; ++i) {
        Ep& e = d.ep[i];
        ic[1] |= 1u << e.dci;
        if (e.dci > maxdci) maxdci = e.dci;
        uint32_t* ec = ctx_at(c, d.in, e.dci + 1u);
        ec[0] = static_cast<uint32_t>(e.interval) << 16;
        ec[1] = (3u << 1) | (7u << 3) | (static_cast<uint32_t>(e.mps) << 16);
        ec[2] = static_cast<uint32_t>(e.ring.phys) | 1;
        ec[3] = static_cast<uint32_t>(e.ring.phys >> 32);
        ec[4] = e.mps | (static_cast<uint32_t>(e.mps) << 16);
    }
    uint32_t* sc = ctx_at(c, d.in, 1);
    const uint32_t* osc = ctx_at(c, d.out, 0);
    for (int i = 0; i < 8; ++i) sc[i] = osc[i];
    sc[0] = (sc[0] & ~(31u << 27)) | (maxdci << 27);
    if (!command(c, d.in_phys, 0, (TRB_CONFIG_EP << 10) | (slot << 24))) { log("configure endpoint failed"); free_dev(c, d, true); return false; }

    for (int i = 0; i < nep; ++i) { d.ep[i].active = true; queue_int_trbs(c, d, d.ep[i]); }
    logv("pointer device ready on port", port);
    return true;
}

// ---- controller bring-up ------------------------------------------------------------------
void legacy_handoff(Ctrl& c, uint32_t hcc1) {
    uint32_t off = ((hcc1 >> 16) & 0xFFFF) << 2;
    for (int guard = 0; off && guard < 64; ++guard) {
        const uint32_t dw = rd32(c.cap + off);
        if ((dw & 0xFF) == 1) {
            if (dw & (1u << 16)) {                                     // BIOS owns it: ask nicely
                wr32(c.cap + off, dw | (1u << 24));
                wait_for(c.cap + off, 1u << 16, 0, 1000);
            }
            uint32_t s = rd32(c.cap + off + 4);
            s &= (0x3u << 1) | (0xFFu << 5) | (0x7u << 17);            // keep reserved bits, drop SMI enables
            s |= 0x7u << 29;                                           // ack pending SMI events
            wr32(c.cap + off + 4, s);
        }
        const uint32_t next = (dw >> 8) & 0xFF;
        if (!next) break;
        off += next << 2;
    }
}

bool start_controller(Ctrl& c) {
    const uint32_t caplen = rd32(c.cap) & 0xFF;
    const uint32_t hcs1 = rd32(c.cap + 4), hcs2 = rd32(c.cap + 8), hcc1 = rd32(c.cap + 0x10);
    c.op = c.cap + caplen;
    c.db = c.cap + (rd32(c.cap + 0x14) & ~3u);
    c.rt = c.cap + (rd32(c.cap + 0x18) & ~0x1Fu);
    c.ctxsz = (hcc1 & 4) ? 64 : 32;
    c.maxslots = hcs1 & 0xFF;
    c.maxports = hcs1 >> 24;
    if (c.maxslots > kMaxDev - 1) c.maxslots = kMaxDev - 1;

    legacy_handoff(c, hcc1);

    wr32(c.op, rd32(c.op) & ~CMD_RS);                                  // stop
    if (!wait_for(c.op + 4, STS_HCH, STS_HCH, 200)) { log("controller won't halt"); return false; }
    wr32(c.op, CMD_HCRST);                                             // reset
    if (!wait_for(c.op, CMD_HCRST, 0, 1000) || !wait_for(c.op + 4, STS_CNR, 0, 1000)) { log("reset timeout"); return false; }

    c.dcbaa = static_cast<uint64_t*>(dma_alloc(1, &c.dcbaa_phys));
    c.ctl_buf = static_cast<uint8_t*>(dma_alloc(1, &c.ctl_phys));
    uint64_t evseg_phys = 0, erst_phys = 0;
    c.ev = static_cast<volatile Trb*>(dma_alloc(1, &c.ev_phys));
    uint32_t* erst = static_cast<uint32_t*>(dma_alloc(1, &erst_phys));
    if (!c.dcbaa || !c.ctl_buf || !c.ev || !erst || !ring_init(c.cmd, 256)) return false;
    evseg_phys = c.ev_phys;

    const uint32_t scratch = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (scratch) {
        uint64_t arr_phys;
        uint64_t* arr = static_cast<uint64_t*>(dma_alloc(1, &arr_phys));
        if (!arr || scratch > 512) return false;
        for (uint32_t i = 0; i < scratch; ++i) {
            uint64_t p;
            if (!dma_alloc(1, &p)) return false;
            arr[i] = p;
        }
        c.dcbaa[0] = arr_phys;
    }

    wr32(c.op + 0x38, c.maxslots);                                     // CONFIG
    wr64(c.op + 0x30, c.dcbaa_phys);                                   // DCBAAP
    wr64(c.op + 0x18, c.cmd.phys | 1);                                 // CRCR (RCS=1)

    erst[0] = static_cast<uint32_t>(evseg_phys); erst[1] = static_cast<uint32_t>(evseg_phys >> 32);
    erst[2] = kEvRing; erst[3] = 0;
    wr32(c.rt + 0x28, 1);                                              // ERSTSZ
    wr64(c.rt + 0x38, evseg_phys);                                     // ERDP
    wr64(c.rt + 0x30, erst_phys);                                      // ERSTBA
    c.ev_deq = 0; c.ev_cyc = 1;

    wr32(c.op, CMD_RS);                                                // run (interrupts stay off: we poll)
    if (!wait_for(c.op + 4, STS_HCH, 0, 500)) { log("controller won't run"); return false; }
    return true;
}

void try_init(const pcix::Dev& pd) {
    if (g_nctrl >= kMaxCtrl) return;
    uint64_t base;
    if (!pcix::bar_mem(pd, 0, &base)) return;
    pcix::enable(pd);
    if (!vmm::map_mmio(base, 0x40000, vmm::CacheMode::Uncached)) { log("cannot map xHCI registers"); return; }
    Ctrl& c = g_ctrl[g_nctrl];
    c.pci = pd; c.cap = base;
    logv("xHCI controller at", base);
    if (!start_controller(c)) { log("xHCI init failed"); return; }
    c.ok = true;
    ++g_nctrl;
}

void scan_ports(Ctrl& c) {
    for (uint32_t p = 1; p <= c.maxports; ++p) {
        uint32_t ps = rd32(portsc_addr(c, p));
        if (!(ps & PS_PP)) { wr32(portsc_addr(c, p), PS_PP); }
    }
    tm::delay_ms(120);                                                 // let devices announce themselves
    for (uint32_t p = 1; p <= c.maxports; ++p) {
        bool have = false;
        for (Dev& d : c.dev) if (d.used && d.port == p) have = true;
        if (!have && (rd32(portsc_addr(c, p)) & PS_CCS)) enumerate(c, p);
    }
    events(c);
    c.port_changed = 0;
}

void handle_port_changes(Ctrl& c) {
    if (!c.port_changed) return;
    const uint64_t mask = c.port_changed;
    c.port_changed = 0;
    for (uint32_t p = 1; p <= c.maxports && p < 64; ++p) {
        if (!(mask & (1ULL << p))) continue;
        const uint64_t a = portsc_addr(c, p);
        const uint32_t ps = rd32(a);
        wr32(a, port_neutral(ps) | PS_CHANGE);
        Dev* have = nullptr;
        for (Dev& d : c.dev) if (d.used && d.port == p) have = &d;
        if (!(ps & PS_CCS)) {
            if (have) { log("device removed"); free_dev(c, *have, true); }
        } else if (!have) {
            tm::delay_ms(100);                                         // debounce
            enumerate(c, p);
        }
    }
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------
bool init() {
    if (!g_calibrated) { tm::calibrate(); g_calibrated = true; }
    pcix::scan([](const pcix::Dev& d) { if (d.cls == 0x0C0330) try_init(d); });
    for (int i = 0; i < g_nctrl; ++i) scan_ports(g_ctrl[i]);
    if (!g_nctrl) log("no xHCI controller found");
    return g_nctrl > 0;
}

void poll() {
    for (int i = 0; i < g_nctrl; ++i) {
        Ctrl& c = g_ctrl[i];
        if (!c.ok) continue;
        events(c);
        if (c.port_changed) handle_port_changes(c);
        for (Dev& d : c.dev) {
            if (!d.used) continue;
            for (Ep& e : d.ep) {
                if (!e.active) continue;
                if (e.need_recover) {
                    e.need_recover = false;
                    if (recover_endpoint(c, d, e.dci, e.ring, true)) queue_int_trbs(c, d, e);
                    else log("endpoint recovery failed");
                }
                e.pd->tick(tm::ms());
            }
        }
    }
}

int controller_count() { return g_nctrl; }
int pointer_count() { int n = 0; for (int i = 0; i < kPool; ++i) if (g_pool_used[i]) ++n; return n; }

}  // namespace aos::usb
