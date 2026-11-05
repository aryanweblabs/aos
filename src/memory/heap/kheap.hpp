/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::heap {
bool initialize();
void* allocate(size_t size);
void release(void* pointer);
uint64_t pages_in_use();
}
