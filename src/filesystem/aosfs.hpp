/* SPDX-License-Identifier: MIT */
// aosfs.hpp - on-disk AOS filesystem (cluster-chain, persistent on ATA)
#pragma once
#include "ramfs.hpp"
#include "../drivers/storage-drivers/block_device.hpp"

namespace aos::aosfs {

// Mount primary ATA as AOSFS. If disk has no valid superblock, returns false
// (caller may format). If already mounted, returns true.
bool mount(const aos::storage::BlockDevice* dev);

// Create a fresh filesystem on the whole device (destroys data).
bool format(const aos::storage::BlockDevice* dev);

bool mounted();
void unmount();

// Same operations as aos::fs (used by the fs facade)
aos::fs::Result mkdir(const char* name);
aos::fs::Result create(const char* name);
aos::fs::Result write(const char* name, const void* data, size_t len);
aos::fs::Result append(const char* name, const void* data, size_t len);
aos::fs::Result remove(const char* name);
aos::fs::Result rename(const char* old_name, const char* new_name);
aos::fs::Result cd(const char* name);
const aos::fs::Node* read(const char* name);
void cwd_path(char* buf, size_t buflen);
size_t node_count();
void list(void (*cb)(const char* name, bool is_dir, size_t size, void* ctx), void* ctx);
void list_path(const char* path, void (*cb)(const char* name, bool is_dir, size_t size, void* ctx), void* ctx);
aos::fs::Result copy(const char* src, const char* dst);
bool sync();

}  // namespace aos::aosfs
