/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::filesystem {
struct FileView { const char* name; const uint8_t* data; size_t size; };
bool mount_initramfs(const void* archive, size_t length);
bool find_file(const char* name, FileView* result);
}
