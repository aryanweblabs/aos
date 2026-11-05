/* SPDX-License-Identifier: MIT */
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
// ahci.cpp - Universal AHCI (SATA) class driver.
// Binds to any PCI class 0x010601 controller and exports each active port as a BlockDevice.
#include "ahci.hpp"
#include "../pciutil.hpp"
#include "../timeutil.hpp"
#include "../../memory/physical/pmm.h"
#include "../../memory/virtual/vmm.hpp"
#include "../../arch/x86_64/sync.h"

extern "C" void aos_serial_write(const char* text);

namespace aos::storage::ahci {
namespace {

constexpr unsigned kMaxPorts = 32;
constexpr unsigned kMaxDev = 8;
constexpr uint32_t FIS_SIZE = 256;
constexpr uint32_t CMD_LIST_SIZE = 1024;   // 32 headers * 32 bytes
constexpr uint32_t CMD_TBL_SIZE = 256;

struct HbaMem {
    uint32_t cap, ghc, is, pi, vs;
    uint32_t ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
} __attribute__((packed));

struct HbaPort {
    uint64_t clb;
    uint64_t fb;
    uint32_t is, ie, cmd, rsv0, tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed));

struct CmdHeader {
    uint16_t flags;      // CFL,W,P,R,B,C,PMP
    uint16_t prdtl;
    uint32_t prdbc;
    uint64_t ctba;
    uint32_t rsv[4];
} __attribute__((packed));

struct PrdtEntry {
    uint64_t dba;
    uint32_t rsv;
    uint32_t dbc;        // bit 31 interrupt
} __attribute__((packed));

struct CmdTable {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    PrdtEntry prdt[1];
} __attribute__((packed));

struct PortDev {
    bool present;
    volatile HbaPort* port;
    CmdHeader* cl;
    uint64_t cl_phys;
    uint8_t* fis;
    uint64_t fis_phys;
    CmdTable* ct;
    uint64_t ct_phys;
    uint64_t sectors;
    char model[41];
    char serial[21];
    BlockDevice bdev;
    Info info;
    aos_spinlock_t lock;
};

PortDev g_dev[kMaxDev];
int g_ndev = 0;

void log(const char* s) { aos_serial_write("[ahci] "); aos_serial_write(s); aos_serial_write("\r\n"); }

void* dma_z(unsigned pages, uint64_t* phys) {
    const uint64_t p = pmm_alloc_contiguous(pages);
    if (!p || p >= 0x100000000ULL) return nullptr;
    auto* v = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(p));
    for (unsigned i = 0; i < pages * 4096u; ++i) v[i] = 0;
    *phys = p;
    return v;
}

inline uint32_t rd(volatile uint32_t* r) { return *r; }
inline void wr(volatile uint32_t* r, uint32_t v) { *r = v; }

bool port_idle(volatile HbaPort* p) {
    const uint32_t t0 = tm::ms();
    while (tm::ms() - t0 < 500) {
        const uint32_t cmd = rd(&p->cmd);
        if (!(cmd & (1u << 15)) && !(cmd & (1u << 14))) return true;  // not CR, not FR
    }
    return false;
}

bool stop_port(volatile HbaPort* p) {
    uint32_t cmd = rd(&p->cmd);
    cmd &= ~1u;          // ~ST
    cmd &= ~(1u << 4);   // ~FRE
    wr(&p->cmd, cmd);
    return port_idle(p);
}

bool start_port(volatile HbaPort* p) {
    if (!port_idle(p)) return false;
    uint32_t cmd = rd(&p->cmd);
    cmd |= (1u << 4);  // FRE
    wr(&p->cmd, cmd);
    cmd |= 1u;         // ST
    wr(&p->cmd, cmd);
    return true;
}

void build_fis(uint8_t* cfis, uint8_t cmd, uint64_t lba, uint16_t count, bool write) {
    for (int i = 0; i < 64; ++i) cfis[i] = 0;
    cfis[0] = 0x27;          // Host to device
    cfis[1] = 1 << 7;        // command
    cfis[2] = cmd;
    cfis[4] = static_cast<uint8_t>(lba);
    cfis[5] = static_cast<uint8_t>(lba >> 8);
    cfis[6] = static_cast<uint8_t>(lba >> 16);
    cfis[7] = 0x40;          // LBA mode
    cfis[8] = static_cast<uint8_t>(lba >> 24);
    cfis[9] = static_cast<uint8_t>(lba >> 32);
    cfis[10] = static_cast<uint8_t>(lba >> 40);
    cfis[12] = static_cast<uint8_t>(count);
    cfis[13] = static_cast<uint8_t>(count >> 8);
    (void)write;
}

bool issue(PortDev& d, uint8_t ata_cmd, uint64_t lba, uint16_t count, void* buf, bool is_write) {
    const uint64_t f = aos_spin_lock_irqsave(&d.lock);
    volatile HbaPort* p = d.port;

    // wait slot 0 free
    const uint32_t t0 = tm::ms();
    while (rd(&p->ci) & 1u) {
        if (tm::ms() - t0 > 2000) { aos_spin_unlock_irqrestore(&d.lock, f); return false; }
    }

    uint64_t bounce_phys = 0;
    void* bounce = nullptr;
    if (buf && count) {
        bounce = dma_z(1, &bounce_phys);
        if (!bounce) { aos_spin_unlock_irqrestore(&d.lock, f); return false; }
        if (is_write) {
            const uint8_t* s = static_cast<const uint8_t*>(buf);
            uint8_t* b = static_cast<uint8_t*>(bounce);
            for (uint32_t i = 0; i < count * 512u; ++i) b[i] = s[i];
        }
    }

    CmdHeader& h = d.cl[0];
    h = {};
    h.flags = (5) | (is_write ? (1u << 6) : 0);  // CFL=5 dwords, W if write
    h.prdtl = (buf && count) ? 1 : 0;
    h.ctba = d.ct_phys;

    CmdTable& ct = *d.ct;
    for (int i = 0; i < 64; ++i) ct.cfis[i] = 0;
    build_fis(ct.cfis, ata_cmd, lba, count, is_write);
    if (buf && count) {
        ct.prdt[0].dba = bounce_phys;
        ct.prdt[0].dbc = (count * 512u) - 1;
    }

    wr(&p->is, 0xFFFFFFFF);  // clear
    asm volatile("mfence" ::: "memory");
    wr(&p->ci, 1);           // issue slot 0

    bool ok = false;
    const uint32_t t1 = tm::ms();
    while (tm::ms() - t1 < 3000) {
        if (!(rd(&p->ci) & 1u)) {
            const uint32_t tfd = rd(&p->tfd);
            ok = !(tfd & 1u);  // !ERR
            break;
        }
        if (rd(&p->is) & (1u << 30)) {  // TFES
            ok = false;
            break;
        }
    }

    if (ok && buf && count && !is_write) {
        const uint8_t* b = static_cast<const uint8_t*>(bounce);
        uint8_t* dst = static_cast<uint8_t*>(buf);
        for (uint32_t i = 0; i < count * 512u; ++i) dst[i] = b[i];
    }
    if (bounce_phys) pmm_free_frame(bounce_phys);
    aos_spin_unlock_irqrestore(&d.lock, f);
    return ok;
}

bool identify(PortDev& d) {
    uint16_t id[256];
    for (int i = 0; i < 256; ++i) id[i] = 0;
    if (!issue(d, 0xEC, 0, 1, id, false)) return false;

    // model at words 27..46, serial 10..19, sectors 100..103 if LBA48
    char model[41]{};
    for (int i = 0; i < 20; ++i) {
        model[i * 2] = static_cast<char>(id[27 + i] >> 8);
        model[i * 2 + 1] = static_cast<char>(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    // trim spaces
    int end = 39;
    while (end > 0 && model[end] == ' ') model[end--] = 0;
    for (int i = 0; i < 41; ++i) d.model[i] = model[i];

    char serial[21]{};
    for (int i = 0; i < 10; ++i) {
        serial[i * 2] = static_cast<char>(id[10 + i] >> 8);
        serial[i * 2 + 1] = static_cast<char>(id[10 + i] & 0xFF);
    }
    serial[20] = 0;
    for (int i = 0; i < 21; ++i) d.serial[i] = serial[i];

    uint64_t sectors = 0;
    if (id[83] & (1u << 10)) {
        sectors = id[100] | (static_cast<uint64_t>(id[101]) << 16) |
                  (static_cast<uint64_t>(id[102]) << 32) | (static_cast<uint64_t>(id[103]) << 48);
    } else {
        sectors = id[60] | (static_cast<uint64_t>(id[61]) << 16);
    }
    if (!sectors) return false;
    d.sectors = sectors;
    return true;
}

bool read_cb(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    auto* d = static_cast<PortDev*>(ctx);
    uint8_t* out = static_cast<uint8_t*>(buffer);
    while (count) {
        uint16_t n = count > 8 ? 8 : static_cast<uint16_t>(count);
        // READ DMA EXT
        if (!issue(*d, 0x25, lba, n, out, false)) return false;
        lba += n;
        out += n * 512u;
        count -= n;
    }
    return true;
}

bool write_cb(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    auto* d = static_cast<PortDev*>(ctx);
    const uint8_t* in = static_cast<const uint8_t*>(buffer);
    while (count) {
        uint16_t n = count > 8 ? 8 : static_cast<uint16_t>(count);
        // WRITE DMA EXT
        if (!issue(*d, 0x35, lba, n, const_cast<uint8_t*>(in), true)) return false;
        lba += n;
        in += n * 512u;
        count -= n;
    }
    return true;
}

bool setup_port(volatile HbaPort* p, uint8_t port_no) {
    if (static_cast<unsigned>(g_ndev) >= kMaxDev) return false;
    const uint32_t ssts = rd(&p->ssts);
    if ((ssts & 0xF) != 3) return false;   // not present + active
    if ((ssts & 0xF0) != 0x10) return false;  // not Gen speed ok — still try if DET=3
    const uint32_t sig = rd(&p->sig);
    if (sig != 0x00000101) return false;  // ATA, not ATARA

    if (!stop_port(p)) return false;

    PortDev& d = g_dev[g_ndev];
    d = {};
    d.port = p;
    d.lock = AOS_SPINLOCK_INIT;

    uint64_t phys = 0;
    // command list (1K aligned) + FIS (256) + cmd table — use 2 pages
    void* mem = dma_z(2, &phys);
    if (!mem) return false;
    d.cl = reinterpret_cast<CmdHeader*>(mem);
    d.cl_phys = phys;
    d.fis = reinterpret_cast<uint8_t*>(mem) + 1024;
    d.fis_phys = phys + 1024;
    d.ct = reinterpret_cast<CmdTable*>(reinterpret_cast<uint8_t*>(mem) + 2048);
    d.ct_phys = phys + 2048;

    p->clb = d.cl_phys;
    p->fb = d.fis_phys;
    wr(&p->serr, 0xFFFFFFFF);
    wr(&p->is, 0xFFFFFFFF);
    if (!start_port(p)) return false;

    if (!identify(d)) {
        stop_port(p);
        return false;
    }

    d.info = {};
    for (int i = 0; i < 41; ++i) d.info.model[i] = d.model[i];
    for (int i = 0; i < 21; ++i) d.info.serial[i] = d.serial[i];
    d.info.bytes = d.sectors * 512ULL;
    d.info.native_block = 512;
    d.info.port = port_no;
    d.bdev.context = &d;
    d.bdev.block_count = d.sectors;
    d.bdev.block_size = 512;
    d.bdev.read = read_cb;
    d.bdev.write = write_cb;
    d.bdev.flush = nullptr;
    d.present = true;
    ++g_ndev;
    log("port device ready");
    return true;
}

bool init_controller(const pcix::Dev& pd) {
    pcix::enable(pd);
    uint64_t abar = 0;
    if (!pcix::bar_mem(pd, 5, &abar)) return false;
    if (!vmm::map_mmio(abar, 0x1100, vmm::CacheMode::Uncached)) return false;

    auto* hba = reinterpret_cast<volatile HbaMem*>(static_cast<uintptr_t>(abar));
    // take ownership
    wr(&hba->ghc, 1u << 31);  // AE
    const uint32_t pi = rd(&hba->pi);
    auto* ports = reinterpret_cast<volatile HbaPort*>(static_cast<uintptr_t>(abar + 0x100));

    int found = 0;
    for (uint8_t i = 0; i < kMaxPorts; ++i) {
        if (!(pi & (1u << i))) continue;
        if (setup_port(&ports[i], i)) ++found;
    }
    if (found) log("controller online");
    return found > 0;
}

}  // namespace

bool init() {
    g_ndev = 0;
    bool any = false;
    pcix::scan([&](const pcix::Dev& pd) {
        // Class 01 subclass 06 prog-if 01 = AHCI
        if (((pd.cls >> 16) & 0xFF) != 0x01) return;
        if (((pd.cls >> 8) & 0xFF) != 0x06) return;
        if ((pd.cls & 0xFF) != 0x01 && (pd.cls & 0xFF) != 0x00) {
            // some report prog-if 0
        }
        if (((pd.cls >> 8) & 0xFF) == 0x06)
            any = init_controller(pd) || any;
    });
    return any;
}

int device_count() { return g_ndev; }
const BlockDevice* device(int i) {
    return (i >= 0 && i < g_ndev && g_dev[i].present) ? &g_dev[i].bdev : nullptr;
}
const Info* info(int i) {
    return (i >= 0 && i < g_ndev && g_dev[i].present) ? &g_dev[i].info : nullptr;
}

}  // namespace aos::storage::ahci
