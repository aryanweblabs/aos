/* SPDX-License-Identifier: MIT */
// nvme.cpp - NVMe driver. Safe by construction: only reads/writes the LBAs it is explicitly asked for;
// the filesystem layer confines itself to an AOS partition (see storage_mgr.cpp).
#include "nvme.hpp"
#include "../pciutil.hpp"
#include "../timeutil.hpp"
#include "../../memory/physical/pmm.h"
#include "../../memory/virtual/vmm.hpp"

extern "C" {
void aos_serial_write(const char* text);
void aos_serial_write_hex64(uint64_t value);
void* memset(void* s, int c, unsigned long n);
void* memcpy(void* d, const void* s, unsigned long n);
}

namespace aos::storage::nvme {
namespace {

void log(const char* s) { aos_serial_write("[nvme] "); aos_serial_write(s); aos_serial_write("\r\n"); }
void logv(const char* s, uint64_t v) { aos_serial_write("[nvme] "); aos_serial_write(s); aos_serial_write(" "); aos_serial_write_hex64(v); aos_serial_write("\r\n"); }

constexpr int kMaxCtrl = 2, kMaxNs = 4;
constexpr uint32_t kAdminQ = 16, kIoQ = 64;
constexpr uint32_t kChunk = 128 * 1024;                 // bounce buffer / max bytes per command

enum : uint32_t { REG_CAP = 0x00, REG_VS = 0x08, REG_INTMS = 0x0C, REG_CC = 0x14, REG_CSTS = 0x1C, REG_AQA = 0x24, REG_ASQ = 0x28, REG_ACQ = 0x30 };

inline uint32_t rd32(uint64_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
inline void wr32(uint64_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(a) = v; }
inline uint64_t rd64(uint64_t a) { return rd32(a) | (static_cast<uint64_t>(rd32(a + 4)) << 32); }
inline void wr64(uint64_t a, uint64_t v) { wr32(a, static_cast<uint32_t>(v)); wr32(a + 4, static_cast<uint32_t>(v >> 32)); }
inline void barrier() { asm volatile("mfence" ::: "memory"); }

struct Sqe { uint32_t d[16]; };
struct Cqe { uint32_t d[4]; };

struct Queue {
    volatile Sqe* sq = nullptr; volatile Cqe* cq = nullptr;
    uint64_t sq_phys = 0, cq_phys = 0;
    uint32_t size = 0, tail = 0, head = 0, phase = 1;
    uint16_t qid = 0;
};

struct Ctrl {
    bool ok = false, dead = false;
    uint64_t bar = 0;
    uint32_t dstrd = 0;
    Queue admin, io;
    uint8_t* bounce = nullptr; uint64_t bounce_phys = 0;   // kChunk bytes
    uint64_t* prplist = nullptr; uint64_t prplist_phys = 0;
    uint8_t* idbuf = nullptr; uint64_t idbuf_phys = 0;     // 4 KiB
    uint32_t chunk = kChunk;
    uint16_t next_cid = 1;
    char model[41] = {}, serial[21] = {};
};

struct Ns {
    Ctrl* c = nullptr;
    uint32_t nsid = 0, lbads = 9;
    uint64_t sectors512 = 0;
    BlockDevice dev{};
    Info info{};
};

Ctrl g_ctrl[kMaxCtrl];
int g_nctrl = 0;
Ns g_ns[kMaxNs];
int g_nns = 0;

void* dma(unsigned pages, uint64_t* phys) {
    const uint64_t p = pmm_alloc_contiguous(pages);
    if (!p || p >= 0x100000000ULL) return nullptr;
    memset(reinterpret_cast<void*>(p), 0, pages * 4096UL);
    *phys = p;
    return reinterpret_cast<void*>(p);
}

bool wait_rdy(Ctrl& c, uint32_t want, uint32_t timeout_ms) {
    const uint32_t t0 = tm::ms();
    while ((rd32(c.bar + REG_CSTS) & 1) != want) {
        if (tm::ms() - t0 > timeout_ms) return false;
        tm::delay_us(100);
    }
    return true;
}

inline uint64_t sq_db(const Ctrl& c, uint16_t qid) { return c.bar + 0x1000 + (2ULL * qid) * (4ULL << c.dstrd); }
inline uint64_t cq_db(const Ctrl& c, uint16_t qid) { return c.bar + 0x1000 + (2ULL * qid + 1) * (4ULL << c.dstrd); }

// Submit one command and poll for its completion. Returns status (0 = success), 0xFFFF on timeout.
uint32_t submit(Ctrl& c, Queue& q, uint32_t opc, uint32_t nsid, uint64_t prp1, uint64_t prp2,
                uint32_t d10, uint32_t d11, uint32_t d12, uint32_t timeout_ms = 4000, uint32_t* result = nullptr) {
    if (c.dead) return 0xFFFF;
    volatile Sqe& e = q.sq[q.tail];
    const uint16_t cid = c.next_cid++;
    uint32_t v[16] = {};
    v[0] = opc | (static_cast<uint32_t>(cid) << 16);
    v[1] = nsid;
    v[6] = static_cast<uint32_t>(prp1); v[7] = static_cast<uint32_t>(prp1 >> 32);
    v[8] = static_cast<uint32_t>(prp2); v[9] = static_cast<uint32_t>(prp2 >> 32);
    v[10] = d10; v[11] = d11; v[12] = d12;
    for (int i = 0; i < 16; ++i) e.d[i] = v[i];
    barrier();
    q.tail = (q.tail + 1) % q.size;
    wr32(sq_db(c, q.qid), q.tail);

    const uint32_t t0 = tm::ms();
    for (;;) {
        volatile Cqe& cqe = q.cq[q.head];
        const uint32_t d3 = cqe.d[3];
        if (((d3 >> 16) & 1) == q.phase) {
            if (result) *result = cqe.d[0];
            if (++q.head == q.size) { q.head = 0; q.phase ^= 1; }
            wr32(cq_db(c, q.qid), q.head);
            return (d3 >> 17) & 0x7FFF;                       // status code type + code
        }
        if (tm::ms() - t0 > timeout_ms) { c.dead = true; log("command timeout - controller marked dead"); return 0xFFFF; }
    }
}

bool admin(Ctrl& c, uint32_t opc, uint32_t nsid, uint64_t prp1, uint32_t d10, uint32_t d11 = 0) {
    return submit(c, c.admin, opc, nsid, prp1, 0, d10, d11, 0) == 0;
}

void trim_copy(char* out, const uint8_t* in, int n) {
    int e = n;
    while (e > 0 && (in[e - 1] == ' ' || in[e - 1] == 0)) --e;
    int s = 0;
    while (s < e && in[s] == ' ') ++s;
    int k = 0;
    for (int i = s; i < e; ++i) out[k++] = static_cast<char>(in[i]);
    out[k] = 0;
}

// ---- data path -------------------------------------------------------------------------------
bool io_chunk(Ns& n, bool write, uint64_t nat_lba, uint32_t nat_cnt) {
    Ctrl& c = *n.c;
    const uint32_t bytes = nat_cnt << n.lbads;
    const uint32_t pages = (bytes + 4095) / 4096;
    uint64_t prp2 = 0;
    if (pages == 2) prp2 = c.bounce_phys + 4096;
    else if (pages > 2) {
        for (uint32_t i = 1; i < pages; ++i) c.prplist[i - 1] = c.bounce_phys + static_cast<uint64_t>(i) * 4096;
        prp2 = c.prplist_phys;
    }
    const uint32_t st = submit(c, c.io, write ? 0x01 : 0x02, n.nsid, c.bounce_phys, prp2,
                               static_cast<uint32_t>(nat_lba), static_cast<uint32_t>(nat_lba >> 32), nat_cnt - 1);
    if (st) { logv(write ? "write failed, status" : "read failed, status", st); return false; }
    return true;
}

bool ns_rw(Ns& n, uint64_t lba, uint32_t count, void* buf, bool write) {
    if (!n.c || !n.c->ok) return false;
    const uint32_t sh = n.lbads - 9, mask = (1u << sh) - 1;
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (count) {
        const uint64_t nat = lba >> sh;
        const uint32_t off = static_cast<uint32_t>(lba & mask);
        const uint32_t max_nat = n.c->chunk >> n.lbads;
        const uint32_t cover = (max_nat << sh) - off;
        const uint32_t take = count < cover ? count : cover;
        const uint32_t nat_cnt = (off + take + mask) >> sh;
        const bool partial = off != 0 || ((off + take) & mask) != 0;
        if (!write || partial) { if (!io_chunk(n, false, nat, nat_cnt)) return false; }
        if (write) {
            memcpy(n.c->bounce + static_cast<uint64_t>(off) * 512, p, static_cast<unsigned long>(take) * 512);
            if (!io_chunk(n, true, nat, nat_cnt)) return false;
        } else {
            memcpy(p, n.c->bounce + static_cast<uint64_t>(off) * 512, static_cast<unsigned long>(take) * 512);
        }
        p += static_cast<uint64_t>(take) * 512;
        lba += take; count -= take;
    }
    return true;
}

bool cb_read(void* ctx, uint64_t lba, uint32_t count, void* buf) { return ns_rw(*static_cast<Ns*>(ctx), lba, count, buf, false); }
bool cb_write(void* ctx, uint64_t lba, uint32_t count, const void* buf) { return ns_rw(*static_cast<Ns*>(ctx), lba, count, const_cast<void*>(buf), true); }
bool cb_flush(void* ctx) {
    Ns& n = *static_cast<Ns*>(ctx);
    if (!n.c || !n.c->ok) return false;
    return submit(*n.c, n.c->io, 0x00, n.nsid, 0, 0, 0, 0, 0) == 0;
}

// ---- bring-up --------------------------------------------------------------------------------
bool setup_queue(Queue& q, uint16_t qid, uint32_t size) {
    q.size = size; q.qid = qid; q.tail = q.head = 0; q.phase = 1;
    q.sq = static_cast<volatile Sqe*>(dma(1, &q.sq_phys));
    q.cq = static_cast<volatile Cqe*>(dma(1, &q.cq_phys));
    return q.sq && q.cq;
}

bool start(Ctrl& c, const pcix::Dev& pd) {
    const uint64_t cap = rd64(c.bar + REG_CAP);
    const uint32_t to_ms = (static_cast<uint32_t>((cap >> 24) & 0xFF) + 1) * 500;
    c.dstrd = static_cast<uint32_t>((cap >> 32) & 15);
    const uint32_t mpsmin = static_cast<uint32_t>((cap >> 48) & 15);
    const uint32_t mqes = static_cast<uint32_t>(cap & 0xFFFF) + 1;
    logv("controller version", rd32(c.bar + REG_VS));
    if (mpsmin != 0) { log("controller needs pages > 4 KiB, unsupported"); return false; }
    if (!((cap >> 37) & 1)) { log("NVM command set not supported"); return false; }
    if (mqes < 2) return false;

    wr32(c.bar + REG_INTMS, 0xFFFFFFFF);                       // we poll: mask pin-based interrupts
    uint32_t cc = rd32(c.bar + REG_CC);
    if (cc & 1) {
        wr32(c.bar + REG_CC, cc & ~1u);
        if (!wait_rdy(c, 0, to_ms)) { log("controller won't disable"); return false; }
    }
    if (!setup_queue(c.admin, 0, kAdminQ)) return false;
    c.bounce = static_cast<uint8_t*>(dma(kChunk / 4096, &c.bounce_phys));
    c.prplist = static_cast<uint64_t*>(dma(1, &c.prplist_phys));
    c.idbuf = static_cast<uint8_t*>(dma(1, &c.idbuf_phys));
    if (!c.bounce || !c.prplist || !c.idbuf) return false;

    wr32(c.bar + REG_AQA, ((kAdminQ - 1) << 16) | (kAdminQ - 1));
    wr64(c.bar + REG_ASQ, c.admin.sq_phys);
    wr64(c.bar + REG_ACQ, c.admin.cq_phys);
    wr32(c.bar + REG_CC, (6u << 16) | (4u << 20) | 1u);        // 64-byte SQE, 16-byte CQE, 4 KiB pages, NVM, enable
    if (!wait_rdy(c, 1, to_ms)) { log("controller won't become ready"); return false; }
    (void)pd;

    // Identify controller
    memset(c.idbuf, 0, 4096);
    if (!admin(c, 0x06, 0, c.idbuf_phys, 1)) { log("identify controller failed"); return false; }
    trim_copy(c.serial, c.idbuf + 4, 20);
    trim_copy(c.model, c.idbuf + 24, 40);
    const uint32_t mdts = c.idbuf[77];
    if (mdts) { const uint32_t lim = 4096u << mdts; if (lim < c.chunk) c.chunk = lim; }
    if (c.chunk < 4096) c.chunk = 4096;
    aos_serial_write("[nvme] model: "); aos_serial_write(c.model); aos_serial_write("\r\n");

    // one I/O queue pair
    admin(c, 0x09, 0, 0, 0x07, 0);                              // Set Features: number of queues (best effort)
    if (!setup_queue(c.io, 1, kIoQ)) return false;
    if (!admin(c, 0x05, 0, c.io.cq_phys, ((kIoQ - 1) << 16) | 1, 1)) { log("create I/O CQ failed"); return false; }
    if (!admin(c, 0x01, 0, c.io.sq_phys, ((kIoQ - 1) << 16) | 1, (1u << 16) | 1)) { log("create I/O SQ failed"); return false; }
    c.ok = true;
    return true;
}

void add_namespace(Ctrl& c, uint32_t nsid) {
    if (g_nns >= kMaxNs) return;
    memset(c.idbuf, 0, 4096);
    if (!admin(c, 0x06, nsid, c.idbuf_phys, 0)) return;
    uint64_t nsze = 0;
    for (int i = 7; i >= 0; --i) nsze = (nsze << 8) | c.idbuf[i];
    if (!nsze) return;
    const uint32_t flbas = c.idbuf[26] & 0xF;
    const uint8_t* lbaf = c.idbuf + 128 + 4 * flbas;
    const uint32_t ms = lbaf[0] | (lbaf[1] << 8), lbads = lbaf[2];
    if (ms != 0) { logv("namespace uses per-LBA metadata, skipped:", nsid); return; }
    if (lbads != 9 && lbads != 12) { logv("unsupported block size, skipped:", nsid); return; }
    Ns& n = g_ns[g_nns];
    n = Ns{};
    n.c = &c; n.nsid = nsid; n.lbads = lbads;
    n.sectors512 = nsze << (lbads - 9);
    n.dev.context = &n; n.dev.block_count = n.sectors512; n.dev.block_size = 512;
    n.dev.read = cb_read; n.dev.write = cb_write; n.dev.flush = cb_flush;
    for (int i = 0; i < 41; ++i) n.info.model[i] = c.model[i];
    for (int i = 0; i < 21; ++i) n.info.serial[i] = c.serial[i];
    n.info.nsid = nsid; n.info.bytes = n.sectors512 * 512; n.info.native_block = 1u << lbads;
    ++g_nns;
    logv("namespace", nsid);
    logv("  size in MiB", n.sectors512 / 2048);
}

void probe(const pcix::Dev& pd) {
    if (g_nctrl >= kMaxCtrl) return;
    uint64_t bar;
    if (!pcix::bar_mem(pd, 0, &bar)) return;
    pcix::enable(pd);
    if (!vmm::map_mmio(bar, 0x4000, vmm::CacheMode::Uncached)) { log("cannot map registers"); return; }
    Ctrl& c = g_ctrl[g_nctrl];
    c.bar = bar;
    logv("NVMe controller at", bar);
    if (!start(c, pd)) { log("controller init failed"); return; }
    ++g_nctrl;

    // active namespace list (CNS 2); fall back to probing NSID 1..4
    uint32_t ids[16]; int nids = 0;
    memset(c.idbuf, 0, 4096);
    if (admin(c, 0x06, 0, c.idbuf_phys, 2)) {
        const uint32_t* l = reinterpret_cast<const uint32_t*>(c.idbuf);
        for (int i = 0; i < 16 && l[i]; ++i) ids[nids++] = l[i];
    }
    if (!nids) for (uint32_t i = 1; i <= 4; ++i) ids[nids++] = i;
    for (int i = 0; i < nids; ++i) add_namespace(c, ids[i]);
}

}  // namespace

bool init() {
    static bool done = false;
    if (done) return g_nns > 0;
    done = true;
    tm::calibrate();
    pcix::scan([](const pcix::Dev& d) { if ((d.cls >> 8) == 0x0108) probe(d); });   // class 01, subclass 08 = NVM
    if (!g_nctrl) log("no NVMe controller");
    return g_nns > 0;
}

int namespace_count() { return g_nns; }
const BlockDevice* device(int i) { return (i >= 0 && i < g_nns) ? &g_ns[i].dev : nullptr; }
const Info* info(int i) { return (i >= 0 && i < g_nns) ? &g_ns[i].info : nullptr; }

}  // namespace aos::storage::nvme
