/* SPDX-License-Identifier: MIT */
// storage_mgr.hpp - disks, GPT partitions and the AOSFS partition finder.
// Rule: AOS never writes to a disk region it was not explicitly told to use (an AOS partition,
// or a blank whole disk after an explicit `mkaosfs ... CONFIRM`).
#pragma once
#include "block_device.hpp"
#include <stdint.h>

namespace aos::storage::mgr {

struct DiskInfo { char model[41]; uint64_t bytes; uint32_t native_block; bool is_nvme; bool has_gpt; bool blank; };
struct PartInfo {
    int      disk, index;          // index = GPT entry number (1-based, as other OSes show it)
    uint64_t first, sectors;       // 512-byte units, relative to the disk
    char     name[28];
    const char* kind;              // "AOS", "EFI System", "Windows data", "Linux", "Windows recovery", "other"
    bool     aos_type;             // GPT type GUID is the AOS one
    bool     aos_fs;               // valid AOSFS superblock found
    bool     foreign_fs;           // NTFS / FAT / exFAT / ext / BitLocker signature found (needs FORCE to overwrite)
};

void scan();                                        // (re)read all disks + GPT tables
int  disk_count();
const DiskInfo* disk(int i);
const BlockDevice* disk_device(int i);
int  part_count();
const PartInfo* part(int i);
const BlockDevice* part_device(int i);              // confined view of one partition

// First partition (or blank-formatted whole disk) that holds an AOSFS. nullptr if none.
const BlockDevice* find_aosfs();

enum class FormatResult { Ok, BadArgs, Refused, IoError };
// disk index + GPT partition number (1-based) or part==0 for "whole disk" (only allowed on a blank disk).
// force: overwrite a partition that still holds a foreign filesystem.
FormatResult format_target(int disk, int part, bool force, const BlockDevice** out_dev);

}  // namespace aos::storage::mgr
