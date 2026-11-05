/* SPDX-License-Identifier: MIT */
// vfs.hpp - Virtual File System layer for AOS.
//
// Current status
// --------------
// The public API remains `aos::fs` (see ramfs.hpp).  Under the hood:
//
//   shell / syscalls
//         │
//         ▼
//   aos::fs  (facade, single global lock)
//         │
//         ├──────────────► aos::aosfs   (persistent disk FS)
//         │                      │
//         │                      ▼
//         │               page_cache  (cluster data cache)
//         │                      │
//         │                      ▼
//         │               BlockDevice (ATA / NVMe / …)
//         │
//         └──────────────► initramfs  (boot-only, read-only)
//
// The page cache sits under AOSFS and is transparent to callers.  Repeated
// reads of the same clusters are served from RAM (CLOCK eviction, 256 KiB).
//
// This header documents the intended VFS abstractions for future work
// (multiple simultaneous mounts, file-backed mmap, vnode reference counts).
// The concrete types below are the stable contracts new filesystems should
// implement.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "page_cache.hpp"

namespace aos::vfs {

// Protection / open flags (subset of POSIX, kept tiny).
constexpr uint32_t F_READ  = 1;
constexpr uint32_t F_WRITE = 2;
constexpr uint32_t F_EXEC  = 4;
constexpr uint32_t F_DIR   = 8;

// Lightweight inode view used by higher layers (page cache keys, mmap, …).
struct Inode {
    uint64_t ino;          // filesystem-local inode number
    uint64_t size;         // bytes
    uint32_t mode;         // F_READ | F_WRITE | F_DIR | …
    uint32_t nlink;
    uint64_t fsid;         // which filesystem instance
};

// Operations a filesystem must provide.  All paths are relative to that FS's root.
struct FileSystemOps {
    const char* name;

    bool (*mount)(void* ctx);
    void (*unmount)(void* ctx);
    bool (*sync)(void* ctx);

    // Lookup `name` in directory `dir_ino`.  On success fills `out`.
    bool (*lookup)(void* ctx, uint64_t dir_ino, const char* name, Inode* out);

    // Read up to `len` bytes at `off` from inode into `buf`.  Returns bytes read, or <0 on error.
    int64_t (*read)(void* ctx, uint64_t ino, uint64_t off, void* buf, size_t len);

    // Write `len` bytes at `off`.  Returns bytes written, or <0 on error.
    int64_t (*write)(void* ctx, uint64_t ino, uint64_t off, const void* buf, size_t len);

    // Create a regular file or directory.
    bool (*create)(void* ctx, uint64_t dir_ino, const char* name, uint32_t mode, Inode* out);
    bool (*unlink)(void* ctx, uint64_t dir_ino, const char* name);
};

struct Mount {
    const FileSystemOps* ops;
    void*                ctx;
    uint64_t             fsid;
    char                 path[32];   // mount point (currently always "/")
    bool                 used;
};

// Future: register/mount multiple filesystems.  Today AOSFS is mounted
// exclusively through aos::fs::init().

// Page-cache helpers shared by all disk-backed filesystems.
inline void cache_stats(aos::page_cache::Stats* s) { aos::page_cache::stats(s); }
inline void cache_reset_stats() { aos::page_cache::reset_stats(); }

}  // namespace aos::vfs
