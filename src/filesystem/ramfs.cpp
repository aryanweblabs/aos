/* SPDX-License-Identifier: MIT */
// ramfs.cpp - filesystem facade. Persistent AOSFS only: NVMe partition first, then ATA. No RAM storage.
#include "ramfs.hpp"
#include "aosfs.hpp"
#include "page_cache.hpp"
#include "../drivers/storage-drivers/ata_pio.hpp"
#include "../drivers/storage-drivers/nvme.hpp"
#include "../drivers/storage-drivers/virtio_blk.hpp"
#include "../drivers/storage-drivers/ahci.hpp"
#include "../drivers/storage-drivers/storage_mgr.hpp"
#include "../scheduler/mutex.hpp"

extern "C" void aos_serial_write(const char* text);

namespace aos::fs {
namespace {
aos::sync::RecursiveMutex g_fs_mutex;    // every public entry point holds it: threads and user programs share one disk
#define FS_LOCK aos::sync::Guard fs_guard_(g_fs_mutex)
bool use_disk = false;
const aos::storage::BlockDevice* g_dev = nullptr;
const char* g_label = "none";

bool blank(const aos::storage::BlockDevice* d) {
    static uint8_t b[512 * 8];
    if (!aos::storage::read_blocks(d, 0, 8, b)) return false;
    for (unsigned i = 0; i < sizeof b; ++i) if (b[i]) return false;
    return true;
}
}  // namespace

bool is_persistent() { return use_disk; }
const char* device_label() { return g_label; }

Result mount_device(const aos::storage::BlockDevice* dev) { FS_LOCK;
    if (!dev) return Result::NoDisk;
    if (use_disk) aos::aosfs::unmount();
    use_disk = false;
    if (!aos::aosfs::mount(dev)) return Result::IoError;
    use_disk = true; g_dev = dev;
    return Result::Ok;
}

Result format_device(const aos::storage::BlockDevice* dev) { FS_LOCK;
    if (!dev) return Result::NoDisk;
    if (use_disk) aos::aosfs::unmount();
    use_disk = false;
    if (!aos::aosfs::format(dev)) return Result::IoError;
    return mount_device(dev);
}

bool init() { FS_LOCK;
    use_disk = false; g_dev = nullptr; g_label = "none";
    aos::page_cache::init();

    aos::storage::mgr::scan();
    if (const auto* d = aos::storage::mgr::find_aosfs()) {
        if (mount_device(d) == Result::Ok) { g_label = "disk"; return true; }
    }

    const auto* ata = aos::storage::ata_pio::primary_master();
    if (ata && aos::storage::valid(ata)) {
        if (mount_device(ata) == Result::Ok) { g_label = "ATA"; return true; }
        // Never overwrite a disk that holds data: only a completely blank disk (fresh VM image) is set up automatically.
        if (blank(ata) && format_device(ata) == Result::Ok) { g_label = "ATA"; return true; }
        aos_serial_write("[fs] ATA disk has data but no AOSFS: left untouched\r\n");
    }
    return false;
}

Result format() { FS_LOCK; return format_device(g_dev); }

bool sync() { FS_LOCK; return use_disk ? aos::aosfs::sync() : true; }

Result mkdir(const char* name) { FS_LOCK; return use_disk ? aos::aosfs::mkdir(name) : Result::NoDisk; }
Result create(const char* name) { FS_LOCK; return use_disk ? aos::aosfs::create(name) : Result::NoDisk; }
Result write(const char* name, const void* data, size_t len) { FS_LOCK; return use_disk ? aos::aosfs::write(name, data, len) : Result::NoDisk; }
Result append(const char* name, const void* data, size_t len) { FS_LOCK; return use_disk ? aos::aosfs::append(name, data, len) : Result::NoDisk; }
Result remove(const char* name) { FS_LOCK; return use_disk ? aos::aosfs::remove(name) : Result::NoDisk; }
Result rename(const char* a, const char* b) { FS_LOCK; return use_disk ? aos::aosfs::rename(a, b) : Result::NoDisk; }
Result cd(const char* name) { FS_LOCK; return use_disk ? aos::aosfs::cd(name) : Result::NoDisk; }
const Node* read(const char* name) { FS_LOCK; return use_disk ? aos::aosfs::read(name) : nullptr; }
void cwd_path(char* buf, size_t n) { FS_LOCK;
    if (use_disk) { aos::aosfs::cwd_path(buf, n); return; }
    if (buf && n > 1) { buf[0] = '/'; buf[1] = 0; }
}
size_t node_count() { FS_LOCK; return use_disk ? aos::aosfs::node_count() : 0; }
void list(void (*cb)(const char*, bool, size_t, void*), void* ctx) { FS_LOCK; if (use_disk) aos::aosfs::list(cb, ctx); }
void list_path(const char* path, void (*cb)(const char*, bool, size_t, void*), void* ctx) { FS_LOCK;
    if (use_disk) aos::aosfs::list_path(path, cb, ctx);
}
Result copy(const char* src, const char* dst) { FS_LOCK;
    return use_disk ? aos::aosfs::copy(src, dst) : Result::NoDisk;
}

Result read_into(const char* name, void* buf, size_t max, size_t* out_size) {
    FS_LOCK;
    if (!use_disk) return Result::NoDisk;
    const Node* n = aos::aosfs::read(name);
    if (!n) return Result::NotFound;
    if (out_size) *out_size = n->size;          // always report true size
    if (!buf || max == 0) return Result::Ok;     // size-only probe
    const size_t take = n->size < max ? n->size : max;
    uint8_t* d = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < take; ++i) d[i] = n->content[i];
    return Result::Ok;
}

const char* result_text(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::Exists: return "already exists";
        case Result::NotFound: return "not found";
        case Result::Full: return "disk full";
        case Result::BadName: return "invalid name";
        case Result::TooBig: return "too big";
        case Result::NotDir: return "not a directory";
        case Result::NotEmpty: return "directory not empty";
        case Result::IsDir: return "is a directory";
        case Result::IoError: return "disk I/O error";
        case Result::NoDisk: return "no storage - run: disks, then mkaosfs";
    }
    return "?";
}

}  // namespace aos::fs
