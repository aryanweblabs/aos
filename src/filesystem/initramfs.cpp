/* SPDX-License-Identifier: MIT */
#include "initramfs.hpp"

namespace aos::filesystem {
namespace {
const uint8_t* archive_data;
size_t archive_size;
bool mounted;
bool name_matches(const char* requested, const char* field, size_t field_length) {
    for (size_t i = 0; i < field_length; ++i) {
        if (requested[i] != field[i]) return false;
        if (!requested[i]) return true;
    }
    return requested[field_length] == '\0';
}
uint64_t octal(const char* text, size_t length) {
    uint64_t value = 0;
    for (size_t i = 0; i < length && text[i]; ++i) {
        if (text[i] < '0' || text[i] > '7') return 0;
        value = value * 8 + static_cast<unsigned>(text[i] - '0');
    }
    return value;
}
struct __attribute__((packed)) TarHeader {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], checksum[8];
    char type, linkname[100], magic[6], version[2], uname[32], gname[32];
    char devmajor[8], devminor[8], prefix[155], padding[12];
};
}

bool mount_initramfs(const void* archive, size_t length) {
    if (!archive || length < 512) return false;
    archive_data = static_cast<const uint8_t*>(archive);
    archive_size = length;
    mounted = true;
    return true;
}

bool find_file(const char* name, FileView* result) {
    if (!mounted || !name || !result) return false;
    size_t offset = 0;
    while (offset + 512 <= archive_size) {
        const auto* header = reinterpret_cast<const TarHeader*>(archive_data + offset);
        if (!header->name[0]) return false;
        const uint64_t size = octal(header->size, sizeof(header->size));
        if (size > archive_size || offset + 512 + size > archive_size) return false;
        if ((header->type == '\0' || header->type == '0') && name_matches(name, header->name, sizeof(header->name))) {
            result->name = header->name;
            result->data = archive_data + offset + 512;
            result->size = static_cast<size_t>(size);
            return true;
        }
        const uint64_t padded = (size + 511) & ~511ULL;
        if (padded > archive_size - offset - 512) return false;
        offset += 512 + static_cast<size_t>(padded);
    }
    return false;
}
}
