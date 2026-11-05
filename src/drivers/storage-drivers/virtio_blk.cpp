/* SPDX-License-Identifier: MIT */
// virtio_blk.cpp - Virtio block class driver (PCI modern preferred, legacy fallback).
#include <stddef.h>
// Spec: Virtual I/O Device (VIRTIO) 1.0+ block device. One code path serves all compliant devices.
#include "virtio_blk.hpp"
#include "../pciutil.hpp"
#include "../timeutil.hpp"
#include "../../memory/physical/pmm.h"
#include "../../memory/virtual/vmm.hpp"
#include "../../arch/x86_64/sync.h"

extern "C" void aos_serial_write(const char* text);

namespace aos::storage::virtio_blk {
namespace {

constexpr uint16_t VIRTIO_VENDOR = 0x1AF4;
constexpr uint16_t DEV_BLK_LEGACY = 0x1001;
constexpr uint16_t DEV_BLK_MODERN = 0x1042;

// VIRTIO_F_VERSION_1 is feature bit 32 (dword 1 bit 0)
constexpr uint32_t FEAT_BARRIER = 0;   // legacy bits we care about are small
constexpr uint32_t FEAT_SIZE_MAX = 1;
constexpr uint32_t FEAT_SEG_MAX = 2;
constexpr uint32_t FEAT_GEOMETRY = 4;
constexpr uint32_t FEAT_RO = 5;
constexpr uint32_t FEAT_BLK_SIZE = 6;
constexpr uint32_t FEAT_FLUSH = 9;
constexpr uint32_t FEAT_TOPOLOGY = 10;
constexpr uint32_t FEAT_CONFIG_WCE = 11;
constexpr uint32_t FEAT_DISCARD = 13;
constexpr uint32_t FEAT_WRITE_ZEROES = 14;

constexpr uint32_t VIRTQ_DESC_F_NEXT = 1;
constexpr uint32_t VIRTQ_DESC_F_WRITE = 2;

constexpr uint32_t VIRTIO_BLK_T_IN = 0;
constexpr uint32_t VIRTIO_BLK_T_OUT = 1;
constexpr uint32_t VIRTIO_BLK_T_FLUSH = 4;
constexpr uint8_t  VIRTIO_BLK_S_OK = 0;

constexpr unsigned kMaxDev = 4;
constexpr unsigned kQSize = 128;         // power of two; match typical virtio defaults
constexpr unsigned kDescBytes = 16;

struct VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct VirtqAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[kQSize];
} __attribute__((packed));

struct VirtqUsedElem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct VirtqUsed {
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[kQSize];
} __attribute__((packed));

struct BlkReqHdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

// ---- modern PCI capability structures (virtio 1.0) ----
struct CapHdr {
    uint8_t  cap_vndr, next, len, cfg_type;
    uint8_t  bar, padding[3];
    uint32_t offset, length;
} __attribute__((packed));

struct CommonCfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct Dev {
    bool     present;
    bool     modern;
    uint64_t capacity_sectors;   // 512-byte sectors
    uint32_t blk_size;
    BlockDevice bdev;
    Info     info;

    // queue
    VirtqDesc*  desc;
    VirtqAvail* avail;
    VirtqUsed*  used;
    uint64_t    desc_phys, avail_phys, used_phys;
    uint16_t    qsize;
    uint16_t    last_used;
    uint16_t    free_head;
    uint16_t    num_free;
    aos_spinlock_t lock;

    // modern
    volatile CommonCfg* common;
    volatile uint8_t*   notify_base;
    uint32_t            notify_off_mul;
    volatile uint8_t*   isr;
    volatile uint8_t*   devcfg;

    // legacy
    uint16_t iobase;
};

Dev g_dev[kMaxDev];
int g_ndev = 0;

void log(const char* s) { aos_serial_write("[virtio-blk] "); aos_serial_write(s); aos_serial_write("\r\n"); }

void* dma_pages(unsigned n, uint64_t* phys) {
    const uint64_t p = pmm_alloc_contiguous(n);
    if (!p || p >= 0x100000000ULL) return nullptr;
    auto* v = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(p));
    for (unsigned i = 0; i < n * 4096u; ++i) v[i] = 0;
    *phys = p;
    return v;
}

// ---- modern helpers ----
volatile uint8_t* map_bar_off(const pcix::Dev& pd, uint8_t bar, uint32_t off, uint32_t len) {
    uint64_t base = 0;
    if (!pcix::bar_mem(pd, bar, &base)) return nullptr;
    if (!vmm::map_mmio(base, off + len + 0x1000, vmm::CacheMode::Uncached)) return nullptr;
    return reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(base + off));
}

bool setup_queue_modern(Dev& d) {
    d.common->queue_select = 0;
    uint16_t qs = d.common->queue_size;
    if (!qs) return false;
    if (qs > kQSize) qs = kQSize;
    d.common->queue_size = qs;
    d.qsize = qs;

    // 1 page desc + 1 page avail/used is enough for kQSize=32
    uint64_t phys = 0;
    void* mem = dma_pages(3, &phys);
    if (!mem) return false;
    d.desc = reinterpret_cast<VirtqDesc*>(mem);
    d.desc_phys = phys;
    d.avail = reinterpret_cast<VirtqAvail*>(reinterpret_cast<uint8_t*>(mem) + 4096);
    d.avail_phys = phys + 4096;
    d.used = reinterpret_cast<VirtqUsed*>(reinterpret_cast<uint8_t*>(mem) + 8192);
    d.used_phys = phys + 8192;

    d.free_head = 0;
    d.num_free = qs;
    for (uint16_t i = 0; i < qs - 1; ++i) d.desc[i].next = i + 1;
    d.desc[qs - 1].next = 0xFFFF;
    d.last_used = 0;
    d.avail->idx = 0;
    d.used->idx = 0;

    d.common->queue_desc = d.desc_phys;
    d.common->queue_driver = d.avail_phys;
    d.common->queue_device = d.used_phys;
    d.common->queue_enable = 1;
    return true;
}

void notify_modern(Dev& d) {
    const uint16_t off = d.common->queue_notify_off;
    volatile uint16_t* n = reinterpret_cast<volatile uint16_t*>(
        d.notify_base + static_cast<uint32_t>(off) * d.notify_off_mul);
    *n = 0;
}

// ---- legacy IO helpers ----
inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
inline void outl(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
inline uint16_t inw(uint16_t port) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline uint32_t inl(uint16_t port) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
inline uint8_t inb(uint16_t port) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

constexpr uint16_t LEG_DEV_FEAT = 0, LEG_DRV_FEAT = 4, LEG_QADDR = 8, LEG_QSIZE = 12,
                   LEG_QSEL = 14, LEG_QNOTIFY = 16, LEG_STATUS = 18, LEG_ISR = 19;
constexpr uint16_t LEG_CFG = 20;  // device-specific starts here for blk when no MSI-X

bool setup_queue_legacy(Dev& d) {
    outw(d.iobase + LEG_QSEL, 0);
    uint16_t qs = inw(d.iobase + LEG_QSIZE);
    if (!qs) return false;
    // Legacy queues cannot be resized on many devices — allocate for the reported size (cap kQSize).
    if (qs > kQSize) qs = kQSize;
    d.qsize = qs;

    // Layout: [desc x qs] [avail] [pad to page] [used] — all within contiguous low pages.
    const size_t desc_bytes = static_cast<size_t>(qs) * kDescBytes;
    const size_t avail_bytes = 4 + static_cast<size_t>(qs) * 2 + 2;
    const size_t used_bytes = 4 + static_cast<size_t>(qs) * 8;
    const size_t need = desc_bytes + avail_bytes + 4096 + used_bytes + 4096;
    const unsigned pages = static_cast<unsigned>((need + 4095) / 4096);
    uint64_t phys = 0;
    void* mem = dma_pages(pages ? pages : 4, &phys);
    if (!mem) return false;
    d.desc = reinterpret_cast<VirtqDesc*>(mem);
    d.desc_phys = phys;
    d.avail = reinterpret_cast<VirtqAvail*>(reinterpret_cast<uint8_t*>(mem) + desc_bytes);
    uintptr_t used_u = (reinterpret_cast<uintptr_t>(d.avail) + avail_bytes + 4095) & ~static_cast<uintptr_t>(4095);
    d.used = reinterpret_cast<VirtqUsed*>(used_u);
    d.avail_phys = phys + (reinterpret_cast<uintptr_t>(d.avail) - reinterpret_cast<uintptr_t>(mem));
    d.used_phys = phys + (used_u - reinterpret_cast<uintptr_t>(mem));

    d.free_head = 0;
    d.num_free = qs;
    for (uint16_t i = 0; i < qs - 1; ++i) d.desc[i].next = static_cast<uint16_t>(i + 1);
    d.desc[qs - 1].next = 0xFFFF;
    d.last_used = 0;
    d.avail->flags = 0;
    d.avail->idx = 0;
    d.used->flags = 0;
    d.used->idx = 0;

    outl(d.iobase + LEG_QADDR, static_cast<uint32_t>(phys / 4096));
    return true;
}

void notify_legacy(Dev& d) { outw(d.iobase + LEG_QNOTIFY, 0); }

bool xfer(Dev& d, uint64_t sector, uint32_t nsectors, void* buf, bool write) {
    if (!nsectors || nsectors > 8) return false;   // 4 KiB bounce path simplicity: max 8 sectors
    const uint64_t f = aos_spin_lock_irqsave(&d.lock);
    if (d.num_free < 3) { aos_spin_unlock_irqrestore(&d.lock, f); return false; }

    // bounce buffer in low memory
    uint64_t bounce_phys = 0;
    void* bounce = dma_pages(1, &bounce_phys);
    if (!bounce) { aos_spin_unlock_irqrestore(&d.lock, f); return false; }
    if (write) {
        const uint8_t* s = static_cast<const uint8_t*>(buf);
        uint8_t* b = static_cast<uint8_t*>(bounce);
        for (uint32_t i = 0; i < nsectors * 512u; ++i) b[i] = s[i];
    }

    // header + data + status (1 byte)
    // Use a per-call status in bounce page tail
    uint8_t* status = static_cast<uint8_t*>(bounce) + 4096 - 1;
    *status = 0xFF;
    const uint64_t status_phys = bounce_phys + 4095;

    BlkReqHdr* hdr = reinterpret_cast<BlkReqHdr*>(static_cast<uint8_t*>(bounce) + 2048);
    hdr->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = sector;
    const uint64_t hdr_phys = bounce_phys + 2048;

    uint16_t d0 = d.free_head;
    uint16_t d1 = d.desc[d0].next;
    uint16_t d2 = d.desc[d1].next;
    d.free_head = d.desc[d2].next;
    d.num_free = static_cast<uint16_t>(d.num_free - 3);

    d.desc[d0].addr = hdr_phys;
    d.desc[d0].len = sizeof(BlkReqHdr);
    d.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    d.desc[d0].next = d1;

    d.desc[d1].addr = bounce_phys;
    d.desc[d1].len = nsectors * 512u;
    d.desc[d1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    d.desc[d1].next = d2;

    d.desc[d2].addr = status_phys;
    d.desc[d2].len = 1;
    d.desc[d2].flags = VIRTQ_DESC_F_WRITE;
    d.desc[d2].next = 0;

    const uint16_t aidx = d.avail->idx;
    d.avail->ring[aidx % d.qsize] = d0;
    asm volatile("mfence" ::: "memory");
    d.avail->idx = static_cast<uint16_t>(aidx + 1);
    asm volatile("mfence" ::: "memory");

    if (d.modern) notify_modern(d);
    else notify_legacy(d);

    const uint32_t t0 = tm::ms();
    bool ok = false;
    while (tm::ms() - t0 < 2000) {
        asm volatile("mfence" ::: "memory");
        if (d.used->idx != d.last_used) {
            // reclaim
            while (d.last_used != d.used->idx) {
                const VirtqUsedElem& e = d.used->ring[d.last_used % d.qsize];
                uint16_t id = static_cast<uint16_t>(e.id);
                // free chain of 3
                d.desc[d2].next = d.free_head;
                d.desc[d1].next = d2;
                d.desc[d0].next = d1;
                d.free_head = id;
                d.num_free = static_cast<uint16_t>(d.num_free + 3);
                d.last_used = static_cast<uint16_t>(d.last_used + 1);
                (void)e;
            }
            ok = (*status == VIRTIO_BLK_S_OK);
            break;
        }
    }

    if (!write && ok) {
        const uint8_t* b = static_cast<const uint8_t*>(bounce);
        uint8_t* dst = static_cast<uint8_t*>(buf);
        for (uint32_t i = 0; i < nsectors * 512u; ++i) dst[i] = b[i];
    }
    pmm_free_frame(bounce_phys);  // only first page — dma_pages(1)
    aos_spin_unlock_irqrestore(&d.lock, f);
    return ok;
}

bool read_cb(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    auto* d = static_cast<Dev*>(ctx);
    uint8_t* out = static_cast<uint8_t*>(buffer);
    while (count) {
        uint32_t n = count > 8 ? 8 : count;
        if (!xfer(*d, lba, n, out, false)) return false;
        lba += n;
        out += n * 512u;
        count -= n;
    }
    return true;
}

bool write_cb(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    auto* d = static_cast<Dev*>(ctx);
    const uint8_t* in = static_cast<const uint8_t*>(buffer);
    while (count) {
        uint32_t n = count > 8 ? 8 : count;
        if (!xfer(*d, lba, n, const_cast<uint8_t*>(in), true)) return false;
        lba += n;
        in += n * 512u;
        count -= n;
    }
    return true;
}

bool flush_cb(void* ctx) {
    auto* d = static_cast<Dev*>(ctx);
    // optional flush request — best-effort via type FLUSH with zero data
    const uint64_t f = aos_spin_lock_irqsave(&d->lock);
    if (d->num_free < 2) { aos_spin_unlock_irqrestore(&d->lock, f); return true; }
    uint64_t phys = 0;
    void* mem = dma_pages(1, &phys);
    if (!mem) { aos_spin_unlock_irqrestore(&d->lock, f); return true; }
    BlkReqHdr* hdr = static_cast<BlkReqHdr*>(mem);
    hdr->type = VIRTIO_BLK_T_FLUSH;
    hdr->reserved = 0;
    hdr->sector = 0;
    uint8_t* status = static_cast<uint8_t*>(mem) + 64;
    *status = 0xFF;

    uint16_t d0 = d->free_head;
    uint16_t d1 = d->desc[d0].next;
    d->free_head = d->desc[d1].next;
    d->num_free = static_cast<uint16_t>(d->num_free - 2);
    d->desc[d0].addr = phys;
    d->desc[d0].len = sizeof(BlkReqHdr);
    d->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    d->desc[d0].next = d1;
    d->desc[d1].addr = phys + 64;
    d->desc[d1].len = 1;
    d->desc[d1].flags = VIRTQ_DESC_F_WRITE;

    const uint16_t aidx = d->avail->idx;
    d->avail->ring[aidx % d->qsize] = d0;
    asm volatile("mfence" ::: "memory");
    d->avail->idx = static_cast<uint16_t>(aidx + 1);
    if (d->modern) notify_modern(*d); else notify_legacy(*d);

    const uint32_t t0 = tm::ms();
    while (tm::ms() - t0 < 1000 && d->used->idx == d->last_used) {}
    if (d->used->idx != d->last_used) {
        d->last_used = d->used->idx;
        d->desc[d1].next = d->free_head;
        d->desc[d0].next = d1;
        d->free_head = d0;
        d->num_free = static_cast<uint16_t>(d->num_free + 2);
    }
    pmm_free_frame(phys);
    aos_spin_unlock_irqrestore(&d->lock, f);
    return *status == VIRTIO_BLK_S_OK || *status == 0xFF;
}

bool init_modern(const pcix::Dev& pd) {
    if (static_cast<unsigned>(g_ndev) >= kMaxDev) return false;
    pcix::enable(pd);

    // Walk vendor capabilities for virtio
    uint8_t cap = (pcix::rd(pd, 0x34) & 0xFF);
    volatile CommonCfg* common = nullptr;
    volatile uint8_t* notify = nullptr;
    uint32_t notify_mul = 0;
    volatile uint8_t* isr = nullptr;
    volatile uint8_t* devcfg = nullptr;

    while (cap) {
        const uint32_t dw0 = pcix::rd(pd, cap);
        const uint8_t id = dw0 & 0xFF;
        const uint8_t next = (dw0 >> 8) & 0xFF;
        if (id == 0x09) {   // vendor specific
            const uint8_t cfg_type = (pcix::rd(pd, cap + 3) >> 0) & 0xFF;
            // re-read properly
            uint8_t ctype = 0, bar = 0;
            {
                const uint32_t a = pcix::rd(pd, cap);
                const uint32_t b = pcix::rd(pd, cap + 4);
                const uint32_t c = pcix::rd(pd, cap + 8);
                const uint32_t e = pcix::rd(pd, cap + 12);
                ctype = (a >> 24) & 0xFF;
                bar = b & 0xFF;
                const uint32_t offset = c;
                const uint32_t length = e;
                (void)length;
                auto* p = map_bar_off(pd, bar, offset, length ? length : 0x1000);
                if (!p) { cap = next; continue; }
                if (ctype == 1) common = reinterpret_cast<volatile CommonCfg*>(p);
                else if (ctype == 2) {
                    notify = p;
                    // notify_off_multiplier at cap+16
                    notify_mul = pcix::rd(pd, cap + 16);
                    if (!notify_mul) notify_mul = 1;
                } else if (ctype == 3) isr = p;
                else if (ctype == 4) devcfg = p;
            }
            (void)cfg_type;
        }
        cap = next;
    }
    if (!common || !notify || !devcfg) return false;

    Dev& d = g_dev[g_ndev];
    d = {};
    d.modern = true;
    d.common = common;
    d.notify_base = notify;
    d.notify_off_mul = notify_mul;
    d.isr = isr;
    d.devcfg = devcfg;
    d.lock = AOS_SPINLOCK_INIT;

    // reset + status
    common->device_status = 0;
    asm volatile("mfence" ::: "memory");
    common->device_status = 1;   // ACKNOWLEDGE
    common->device_status = 3;   // DRIVER

    // feature negotiation (minimal)
    common->device_feature_select = 0;
    uint32_t feat0 = common->device_feature;
    common->driver_feature_select = 0;
    uint32_t offer = 0;
    if (feat0 & (1u << FEAT_BLK_SIZE)) offer |= (1u << FEAT_BLK_SIZE);
    if (feat0 & (1u << FEAT_FLUSH)) offer |= (1u << FEAT_FLUSH);
    common->driver_feature = offer;
    common->device_feature_select = 1;
    uint32_t feat1 = common->device_feature;
    common->driver_feature_select = 1;
    common->driver_feature = (feat1 & (1u << 0)) ? (1u << 0) : 0;  // VIRTIO_F_VERSION_1 bit0 of dword1
    common->device_status = 11;  // FEATURES_OK | DRIVER | ACK
    if (!(common->device_status & 8)) { log("FEATURES_OK rejected"); return false; }

    if (!setup_queue_modern(d)) { log("queue setup failed"); return false; }
    common->device_status = 15;  // DRIVER_OK

    // capacity: first 8 bytes of blk config
    uint64_t capacity = 0;
    for (int i = 0; i < 8; ++i)
        capacity |= static_cast<uint64_t>(devcfg[i]) << (8 * i);
    d.capacity_sectors = capacity;
    d.blk_size = 512;
    if (offer & (1u << FEAT_BLK_SIZE)) {
        uint32_t bs = 0;
        for (int i = 0; i < 4; ++i) bs |= static_cast<uint32_t>(devcfg[20 + i]) << (8 * i);
        if (bs == 512 || bs == 4096) d.blk_size = bs;
    }

    d.info = {};
    for (int i = 0; i < 10 && "virtio-blk"[i]; ++i) d.info.model[i] = "virtio-blk"[i];
    d.info.bytes = d.capacity_sectors * 512ULL;
    d.info.native_block = d.blk_size;
    d.bdev.context = &d;
    d.bdev.block_count = d.capacity_sectors;
    d.bdev.block_size = 512;
    d.bdev.read = read_cb;
    d.bdev.write = write_cb;
    d.bdev.flush = flush_cb;
    d.present = true;
    ++g_ndev;
    log("modern device ready");
    return true;
}

bool init_legacy(const pcix::Dev& pd) {
    if (static_cast<unsigned>(g_ndev) >= kMaxDev) return false;
    pcix::enable(pd);
    const uint32_t bar0 = pcix::rd(pd, 0x10);
    if (!(bar0 & 1)) return false;   // need IO BAR
    uint16_t iobase = static_cast<uint16_t>(bar0 & ~3u);
    if (!iobase) return false;

    Dev& d = g_dev[g_ndev];
    d = {};
    d.modern = false;
    d.iobase = iobase;
    d.lock = AOS_SPINLOCK_INIT;

    outb(iobase + LEG_STATUS, 0);
    outb(iobase + LEG_STATUS, 1);
    outb(iobase + LEG_STATUS, 3);

    uint32_t features = inl(iobase + LEG_DEV_FEAT);
    uint32_t offer = 0;
    if (features & (1u << FEAT_BLK_SIZE)) offer |= (1u << FEAT_BLK_SIZE);
    if (features & (1u << FEAT_FLUSH)) offer |= (1u << FEAT_FLUSH);
    outl(iobase + LEG_DRV_FEAT, offer);

    if (!setup_queue_legacy(d)) return false;
    outb(iobase + LEG_STATUS, 7);  // DRIVER_OK (legacy)

    // capacity at config offset
    uint64_t capacity = inl(iobase + LEG_CFG) | (static_cast<uint64_t>(inl(iobase + LEG_CFG + 4)) << 32);
    d.capacity_sectors = capacity;
    d.blk_size = 512;

    d.info = {};
    for (int i = 0; i < 12 && "virtio-blk-L"[i]; ++i) d.info.model[i] = "virtio-blk-L"[i];
    d.info.bytes = capacity * 512ULL;
    d.info.native_block = 512;
    d.bdev.context = &d;
    d.bdev.block_count = capacity;
    d.bdev.block_size = 512;
    d.bdev.read = read_cb;
    d.bdev.write = write_cb;
    d.bdev.flush = flush_cb;
    d.present = true;
    ++g_ndev;
    log("legacy device ready");
    return true;
}

}  // namespace

bool init() {
    g_ndev = 0;
    pcix::scan([](const pcix::Dev& pd) {
        if (pd.vendor != VIRTIO_VENDOR) return;
        // Block device: modern 0x1042, legacy 0x1001, or class 0x01 subclass 0x00 with virtio
        const bool is_blk = (pd.device == DEV_BLK_MODERN || pd.device == DEV_BLK_LEGACY ||
                             ((pd.cls >> 8) == 0x0100 && pd.device >= 0x1000));
        if (!is_blk && pd.device != DEV_BLK_MODERN && pd.device != DEV_BLK_LEGACY) {
            // virtio transitional: subsystem / device id check
            if (pd.device < 0x1000 || pd.device > 0x10FF) return;
            // device type in PCI device id for legacy: 0x1001 = block
            if (pd.device != DEV_BLK_LEGACY && pd.device != DEV_BLK_MODERN) return;
        }
        if (pd.device == DEV_BLK_MODERN || (pd.device >= 0x1040 && pd.device <= 0x107F)) {
            if (!init_modern(pd)) init_legacy(pd);
        } else if (pd.device == DEV_BLK_LEGACY) {
            if (!init_legacy(pd)) init_modern(pd);
        }
    });
    return g_ndev > 0;
}

int device_count() { return g_ndev; }
const BlockDevice* device(int i) {
    return (i >= 0 && i < g_ndev && g_dev[i].present) ? &g_dev[i].bdev : nullptr;
}
const Info* info(int i) {
    return (i >= 0 && i < g_ndev && g_dev[i].present) ? &g_dev[i].info : nullptr;
}

}  // namespace aos::storage::virtio_blk
