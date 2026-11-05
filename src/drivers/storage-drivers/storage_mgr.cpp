/* SPDX-License-Identifier: MIT */
#include "storage_mgr.hpp"
#include "nvme.hpp"
#include "virtio_blk.hpp"
#include "ahci.hpp"
#include "ata_pio.hpp"
#include "../../filesystem/aosfs.hpp"

extern "C" void aos_serial_write(const char* text);

namespace aos::storage::mgr {
namespace {

constexpr int kMaxDisks = 6, kMaxParts = 24;
constexpr uint32_t AOSFS_MAGIC = 0x53464F41u;

// type GUID 4D53414F-0001-4A5B-9C3D-5E6F7A8B9C0D in on-disk (mixed endian) byte order
const uint8_t AOS_GUID[16] = {0x4F, 0x41, 0x53, 0x4D, 0x01, 0x00, 0x5B, 0x4A, 0x9C, 0x3D, 0x5E, 0x6F, 0x7A, 0x8B, 0x9C, 0x0D};
const uint8_t EFI_GUID[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
const uint8_t MSBASIC_GUID[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
const uint8_t MSRESERVED_GUID[16] = {0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE};
const uint8_t WINRE_GUID[16] = {0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC};
const uint8_t LINUX_GUID[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

bool same(const uint8_t* a, const uint8_t* b) { for (int i = 0; i < 16; ++i) if (a[i] != b[i]) return false; return true; }
bool zero16(const uint8_t* a) { for (int i = 0; i < 16; ++i) if (a[i]) return false; return true; }

struct PartCtx { const BlockDevice* parent; uint64_t first; };

struct Disk { DiskInfo info; const BlockDevice* dev; };
struct Part { PartInfo info; PartCtx ctx; BlockDevice view; };

Disk g_disk[kMaxDisks];
int g_ndisk = 0;
Part g_part[kMaxParts];
int g_npart = 0;

bool pv_read(void* c, uint64_t lba, uint32_t n, void* b) { auto* x = static_cast<PartCtx*>(c); return read_blocks(x->parent, x->first + lba, n, b); }
bool pv_write(void* c, uint64_t lba, uint32_t n, const void* b) { auto* x = static_cast<PartCtx*>(c); return write_blocks(x->parent, x->first + lba, n, b); }
bool pv_flush(void* c) { return flush_blocks(static_cast<PartCtx*>(c)->parent); }

uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

bool is_blank(const BlockDevice* d) {
    static uint8_t buf[512 * 8];
    if (!read_blocks(d, 0, 8, buf)) return false;
    for (unsigned i = 0; i < sizeof buf; ++i) if (buf[i]) return false;
    return true;
}

bool foreign_signature(const BlockDevice* d) {
    static uint8_t b[512 * 4];
    if (!read_blocks(d, 0, 4, b)) return true;                   // unreadable: be careful
    auto eq = [&](int off, const char* s, int n) { for (int i = 0; i < n; ++i) if (b[off + i] != static_cast<uint8_t>(s[i])) return false; return true; };
    if (eq(3, "NTFS    ", 8) || eq(3, "EXFAT   ", 8) || eq(3, "-FVE-FS-", 8)) return true;
    if (eq(0x52, "FAT32   ", 8) || eq(0x36, "FAT16   ", 8) || eq(0x36, "FAT12   ", 8)) return true;
    if (b[1080] == 0x53 && b[1081] == 0xEF) return true;          // ext2/3/4
    return false;
}

const char* kind_of(const uint8_t* g) {
    if (same(g, AOS_GUID)) return "AOS";
    if (same(g, EFI_GUID)) return "EFI System";
    if (same(g, MSBASIC_GUID)) return "Windows/data";
    if (same(g, MSRESERVED_GUID)) return "MS reserved";
    if (same(g, WINRE_GUID)) return "Windows recovery";
    if (same(g, LINUX_GUID)) return "Linux";
    return "other";
}

void add_part(int disk, int index, const uint8_t* e, uint64_t mult) {
    if (g_npart >= kMaxParts) return;
    Part& p = g_part[g_npart];
    p = Part{};
    p.info.disk = disk; p.info.index = index;
    p.info.first = u64(e + 32) * mult;
    p.info.sectors = (u64(e + 40) - u64(e + 32) + 1) * mult;
    for (int i = 0; i < 27; ++i) { const uint8_t ch = e[56 + 2 * i]; if (!ch) break; p.info.name[i] = static_cast<char>(ch); }
    p.info.kind = kind_of(e);
    p.info.aos_type = same(e, AOS_GUID);
    p.ctx = PartCtx{g_disk[disk].dev, p.info.first};
    p.view.context = &p.ctx; p.view.block_size = 512; p.view.block_count = p.info.sectors;
    p.view.read = pv_read; p.view.write = pv_write; p.view.flush = pv_flush;
    uint32_t magic = 0;
    uint8_t sec[512];
    if (p.info.sectors >= 64 && read_blocks(&p.view, 0, 1, sec)) magic = u32(sec);
    p.info.aos_fs = magic == AOSFS_MAGIC;
    p.info.foreign_fs = !p.info.aos_fs && foreign_signature(&p.view);
    ++g_npart;
}

void scan_gpt(int d) {
    Disk& dk = g_disk[d];
    const uint64_t mult = dk.info.native_block / 512;
    static uint8_t hdr[512];
    if (!read_blocks(dk.dev, 1 * mult, 1, hdr)) return;
    static const char sig[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (int i = 0; i < 8; ++i) if (hdr[i] != static_cast<uint8_t>(sig[i])) return;
    dk.info.has_gpt = true;
    const uint64_t ent_lba = u64(hdr + 72) * mult;
    uint32_t nent = u32(hdr + 80), esz = u32(hdr + 84);
    if (esz < 128 || esz > 512 || (esz & (esz - 1))) return;
    if (nent > 256) nent = 256;
    static uint8_t sec[512];
    uint64_t loaded = ~0ULL;
    for (uint32_t i = 0; i < nent; ++i) {
        const uint64_t byte = static_cast<uint64_t>(i) * esz;
        const uint64_t lba = ent_lba + byte / 512;
        if (lba != loaded) { if (!read_blocks(dk.dev, lba, 1, sec)) return; loaded = lba; }
        const uint8_t* e = sec + (byte % 512);
        if (esz > 512 - (byte % 512)) continue;                   // entry straddles a sector (not for 128-byte entries)
        if (zero16(e)) continue;
        add_part(d, static_cast<int>(i + 1), e, mult);
    }
}

}  // namespace

void scan() {
    g_ndisk = 0; g_npart = 0;
    auto add = [&](const BlockDevice* dev, const char* model, uint64_t bytes, uint32_t native, bool is_nvme) {
        if (!dev || !valid(dev) || g_ndisk >= kMaxDisks) return;
        Disk& d = g_disk[g_ndisk];
        d = Disk{};
        d.dev = dev;
        for (int k = 0; k < 40 && model && model[k]; ++k) d.info.model[k] = model[k];
        d.info.bytes = bytes;
        d.info.native_block = native;
        d.info.is_nvme = is_nvme;
        d.info.blank = is_blank(d.dev);
        ++g_ndisk;
        scan_gpt(g_ndisk - 1);
    };
    // Prefer class drivers: virtio-blk -> AHCI -> NVMe -> ATA PIO fallback
    for (int i = 0; i < virtio_blk::device_count(); ++i) {
        const auto* vi = virtio_blk::info(i);
        add(virtio_blk::device(i), vi ? vi->model : "virtio-blk",
            vi ? vi->bytes : 0, vi ? vi->native_block : 512, false);
    }
    for (int i = 0; i < ahci::device_count(); ++i) {
        const auto* ai = ahci::info(i);
        add(ahci::device(i), ai ? ai->model : "AHCI",
            ai ? ai->bytes : 0, ai ? ai->native_block : 512, false);
    }
    for (int i = 0; i < nvme::namespace_count(); ++i) {
        const auto* ni = nvme::info(i);
        add(nvme::device(i), ni ? ni->model : "NVMe",
            ni ? ni->bytes : 0, ni ? ni->native_block : 512, true);
    }
    if (ata_pio::primary_master()) {
        const auto* ai = ata_pio::primary_master_info();
        add(ata_pio::primary_master(), ai ? ai->model : "ATA",
            ai ? ai->sectors * 512ULL : 0, 512, false);
    }
}

int disk_count() { return g_ndisk; }
const DiskInfo* disk(int i) { return (i >= 0 && i < g_ndisk) ? &g_disk[i].info : nullptr; }
const BlockDevice* disk_device(int i) { return (i >= 0 && i < g_ndisk) ? g_disk[i].dev : nullptr; }
int part_count() { return g_npart; }
const PartInfo* part(int i) { return (i >= 0 && i < g_npart) ? &g_part[i].info : nullptr; }
const BlockDevice* part_device(int i) { return (i >= 0 && i < g_npart) ? &g_part[i].view : nullptr; }

const BlockDevice* find_aosfs() {
    for (int i = 0; i < g_npart; ++i) if (g_part[i].info.aos_fs) return &g_part[i].view;
    for (int d = 0; d < g_ndisk; ++d) {                           // raw (GPT-less) disk that was formatted as a whole
        if (g_disk[d].info.has_gpt) continue;
        uint8_t sec[512];
        if (read_blocks(g_disk[d].dev, 0, 1, sec) && u32(sec) == AOSFS_MAGIC) return g_disk[d].dev;
    }
    return nullptr;
}

FormatResult format_target(int d, int part_no, bool force, const BlockDevice** out) {
    if (d < 0 || d >= g_ndisk) return FormatResult::BadArgs;
    const BlockDevice* dev = nullptr;
    if (part_no == 0) {
        if (g_disk[d].info.has_gpt) return FormatResult::Refused;
        if (!g_disk[d].info.blank && !force) return FormatResult::Refused;   // blank whole disk, or FORCE
        dev = g_disk[d].dev;
    } else {
        Part* found = nullptr;
        for (int i = 0; i < g_npart; ++i) if (g_part[i].info.disk == d && g_part[i].info.index == part_no) found = &g_part[i];
        if (!found) return FormatResult::BadArgs;
        const PartInfo& pi = found->info;
        if (pi.sectors < 2048) return FormatResult::Refused;                                  // < 1 MiB
        if (pi.kind[0] == 'E' || (pi.kind[0] == 'M' && pi.kind[3] == 'r') || (pi.kind[0] == 'W' && pi.kind[8] == 'r'))
            return FormatResult::Refused;                         // EFI System / MS reserved / Windows recovery: never
        if (pi.foreign_fs && !force) return FormatResult::Refused;
        dev = &found->view;
    }
    if (!aosfs::format(dev)) return FormatResult::IoError;
    flush_blocks(dev);
    if (out) *out = dev;
    return FormatResult::Ok;
}

}  // namespace aos::storage::mgr
