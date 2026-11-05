/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../drivers/storage-drivers/block_device.hpp"

namespace aos::fs {

// Max name length (8.3-ish + long names up to 31)
constexpr size_t NAME_MAX = 31;
// Max bytes returned by read() / accepted by a single write() for shell convenience.
// On-disk files can be larger (up to free space); use append() for big files.
constexpr size_t CONTENT_MAX = 65536;

enum class Result {
    Ok = 0,
    Exists,
    NotFound,
    Full,
    BadName,
    TooBig,
    NotDir,
    NotEmpty,
    IsDir,
    IoError,      // disk I/O failed
    NoDisk        // no block device
};

// Snapshot of a file for read()/cat. Directory nodes have size=0 and empty content.
struct Node {
    char     name[NAME_MAX + 1];
    uint8_t  content[CONTENT_MAX];
    size_t   size;
    bool     used;
    bool     is_dir;
    int      parent;   // -1 = root (compat)
};

// true if a persistent disk FS is mounted. There is NO RAM fallback any more: without a disk every
// file operation returns Result::NoDisk (set one up with `disks` / `mkaosfs` in the terminal).
bool is_persistent();

// Mount order: AOS partition on NVMe -> ATA (valid AOSFS, or a completely blank disk) -> nothing.
bool init();
Result mkdir(const char* name);
Result create(const char* name);
Result write(const char* name, const void* data, size_t len);   // create or overwrite
Result append(const char* name, const void* data, size_t len);
Result remove(const char* name);
Result rename(const char* old_name, const char* new_name);
Result cd(const char* name);          // "..", "/", or dirname
const Node* read(const char* name);
// Thread-safe snapshot: copies up to `max` bytes of the file into `buf` while holding the FS lock.
Result read_into(const char* name, void* buf, size_t max, size_t* out_size);   // nullptr if missing / is dir; content truncated to CONTENT_MAX
void cwd_path(char* buf, size_t buflen);
size_t node_count();
void list(void (*cb)(const char* name, bool is_dir, size_t size, void* ctx), void* ctx);
void list_path(const char* path, void (*cb)(const char* name, bool is_dir, size_t size, void* ctx), void* ctx);
Result copy(const char* src, const char* dst);
const char* result_text(Result r);

// Disk management
Result format();                      // re-create AOSFS on the device that is currently mounted
Result mount_device(const aos::storage::BlockDevice* dev);
Result format_device(const aos::storage::BlockDevice* dev);   // format + mount (caller already vetted the target)
const char* device_label();           // e.g. "NVMe" / "ATA" / "none"
bool   sync();                        // flush cached metadata (always safe to call)

} // namespace aos::fs
