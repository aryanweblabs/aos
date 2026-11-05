/* SPDX-License-Identifier: MIT */
// ata_pio.cpp - Primary master ATA PIO with 28-bit and 48-bit LBA support
#include "ata_pio.hpp"

namespace aos::storage::ata_pio {
namespace {

constexpr uint16_t data_port   = 0x1F0;
constexpr uint16_t error_port  = 0x1F1;
constexpr uint16_t count_port  = 0x1F2;
constexpr uint16_t lba0_port   = 0x1F3;
constexpr uint16_t lba1_port   = 0x1F4;
constexpr uint16_t lba2_port   = 0x1F5;
constexpr uint16_t drive_port  = 0x1F6;
constexpr uint16_t status_port = 0x1F7;
constexpr uint16_t command_port= 0x1F7;

constexpr uint8_t status_error = 0x01;
constexpr uint8_t status_drq   = 0x08;
constexpr uint8_t status_fault = 0x20;
constexpr uint8_t status_busy  = 0x80;

struct AtaState {
    BlockDevice device;
    DriveInfo   info;
    bool        ready;
} state{};

inline void out8(uint16_t port, uint8_t v) { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
inline uint8_t in8(uint16_t port) { uint8_t v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
inline void insw(uint16_t port, void* buf, uint32_t words) {
    asm volatile("cld; rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}
inline void outsw(uint16_t port, const void* buf, uint32_t words) {
    asm volatile("cld; rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

bool wait_not_busy(uint8_t* final = nullptr) {
    for (uint32_t i = 0; i < 1000000; ++i) {
        const uint8_t s = in8(status_port);
        if (s == 0 || s == 0xFF) return false;
        if (!(s & status_busy)) {
            if (final) *final = s;
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

bool wait_drq() {
    for (uint32_t i = 0; i < 1000000; ++i) {
        const uint8_t s = in8(status_port);
        if (s == 0 || s == 0xFF || (s & (status_error | status_fault))) return false;
        if (!(s & status_busy) && (s & status_drq)) return true;
        asm volatile("pause");
    }
    return false;
}

void copy_ata_string(char* dst, const uint16_t* src, int words) {
    int n = 0;
    for (int i = 0; i < words; ++i) {
        char c1 = static_cast<char>((src[i] >> 8) & 0xFF);
        char c2 = static_cast<char>(src[i] & 0xFF);
        if (c1) dst[n++] = c1;
        if (c2) dst[n++] = c2;
    }
    // trim trailing spaces
    while (n > 0 && dst[n - 1] == ' ') --n;
    dst[n] = 0;
}

// 28-bit LBA PIO read (max 255 sectors per command)
bool read28(uint64_t lba, uint32_t count, uint8_t* bytes) {
    while (count) {
        const uint8_t chunk = count > 255 ? 255 : static_cast<uint8_t>(count);
        if (lba + chunk > state.info.sectors || !wait_not_busy()) return false;
        out8(drive_port, static_cast<uint8_t>(0xE0 | ((lba >> 24) & 0x0F)));
        out8(count_port, chunk);
        out8(lba0_port, static_cast<uint8_t>(lba));
        out8(lba1_port, static_cast<uint8_t>(lba >> 8));
        out8(lba2_port, static_cast<uint8_t>(lba >> 16));
        out8(command_port, 0x20);   // READ SECTORS
        for (uint32_t s = 0; s < chunk; ++s) {
            if (!wait_drq()) return false;
            insw(data_port, bytes, 256);
            bytes += 512;
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}

// 48-bit LBA PIO read
bool read48(uint64_t lba, uint32_t count, uint8_t* bytes) {
    while (count) {
        const uint16_t chunk = count > 65535 ? 65535 : static_cast<uint16_t>(count);
        if (lba + chunk > state.info.sectors || !wait_not_busy()) return false;
        out8(drive_port, 0x40);                     // master, LBA
        out8(count_port, static_cast<uint8_t>(chunk >> 8));
        out8(lba0_port,  static_cast<uint8_t>(lba >> 24));
        out8(lba1_port,  static_cast<uint8_t>(lba >> 32));
        out8(lba2_port,  static_cast<uint8_t>(lba >> 40));
        out8(count_port, static_cast<uint8_t>(chunk));
        out8(lba0_port,  static_cast<uint8_t>(lba));
        out8(lba1_port,  static_cast<uint8_t>(lba >> 8));
        out8(lba2_port,  static_cast<uint8_t>(lba >> 16));
        out8(command_port, 0x24);                   // READ SECTORS EXT
        for (uint32_t s = 0; s < chunk; ++s) {
            if (!wait_drq()) return false;
            insw(data_port, bytes, 256);
            bytes += 512;
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool write28(uint64_t lba, uint32_t count, const uint8_t* bytes) {
    while (count) {
        const uint8_t chunk = count > 255 ? 255 : static_cast<uint8_t>(count);
        if (lba + chunk > state.info.sectors || !wait_not_busy()) return false;
        out8(drive_port, static_cast<uint8_t>(0xE0 | ((lba >> 24) & 0x0F)));
        out8(count_port, chunk);
        out8(lba0_port, static_cast<uint8_t>(lba));
        out8(lba1_port, static_cast<uint8_t>(lba >> 8));
        out8(lba2_port, static_cast<uint8_t>(lba >> 16));
        out8(command_port, 0x30);   // WRITE SECTORS
        for (uint32_t s = 0; s < chunk; ++s) {
            if (!wait_drq()) return false;
            outsw(data_port, bytes, 256);
            bytes += 512;
        }
        out8(command_port, 0xE7);   // FLUSH CACHE
        if (!wait_not_busy()) return false;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool write48(uint64_t lba, uint32_t count, const uint8_t* bytes) {
    while (count) {
        const uint16_t chunk = count > 65535 ? 65535 : static_cast<uint16_t>(count);
        if (lba + chunk > state.info.sectors || !wait_not_busy()) return false;
        out8(drive_port, 0x40);
        out8(count_port, static_cast<uint8_t>(chunk >> 8));
        out8(lba0_port,  static_cast<uint8_t>(lba >> 24));
        out8(lba1_port,  static_cast<uint8_t>(lba >> 32));
        out8(lba2_port,  static_cast<uint8_t>(lba >> 40));
        out8(count_port, static_cast<uint8_t>(chunk));
        out8(lba0_port,  static_cast<uint8_t>(lba));
        out8(lba1_port,  static_cast<uint8_t>(lba >> 8));
        out8(lba2_port,  static_cast<uint8_t>(lba >> 16));
        out8(command_port, 0x34);   // WRITE SECTORS EXT
        for (uint32_t s = 0; s < chunk; ++s) {
            if (!wait_drq()) return false;
            outsw(data_port, bytes, 256);
            bytes += 512;
        }
        out8(command_port, 0xEA);   // FLUSH CACHE EXT
        if (!wait_not_busy()) return false;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool read_callback(void*, uint64_t lba, uint32_t count, void* buffer) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    if (state.info.lba48 && (lba > 0x0FFFFFFFULL || count > 255))
        return read48(lba, count, bytes);
    if (lba > 0x0FFFFFFFULL) return false;   // 28-bit limit
    return read28(lba, count, bytes);
}

bool write_callback(void*, uint64_t lba, uint32_t count, const void* buffer) {
    auto* bytes = static_cast<const uint8_t*>(buffer);
    if (state.info.lba48 && (lba > 0x0FFFFFFFULL || count > 255))
        return write48(lba, count, bytes);
    if (lba > 0x0FFFFFFFULL) return false;
    return write28(lba, count, bytes);
}

}  // namespace

bool initialize_primary_master() {
    state = {};
    out8(drive_port, 0xA0);
    for (unsigned i = 0; i < 4; ++i) (void)in8(status_port);

    out8(count_port, 0);
    out8(lba0_port, 0);
    out8(lba1_port, 0);
    out8(lba2_port, 0);
    out8(command_port, 0xEC);   // IDENTIFY

    const uint8_t initial = in8(status_port);
    if (initial == 0 || initial == 0xFF) return false;
    if (!wait_drq()) return false;

    uint16_t id[256]{};
    insw(data_port, id, 256);

    // LBA supported?
    if (!(id[49] & (1u << 9))) return false;

    const bool lba48 = (id[83] & (1u << 10)) != 0;
    uint64_t sectors = 0;
    if (lba48) {
        sectors = static_cast<uint64_t>(id[100])
                | (static_cast<uint64_t>(id[101]) << 16)
                | (static_cast<uint64_t>(id[102]) << 32)
                | (static_cast<uint64_t>(id[103]) << 48);
    }
    if (!sectors) {
        sectors = static_cast<uint32_t>(id[60]) | (static_cast<uint32_t>(id[61]) << 16);
    }
    if (!sectors) return false;
    // Cap at 2 TiB for safety in this kernel
    if (sectors > (2ULL << 30)) sectors = 2ULL << 30;

    state.info.present = true;
    state.info.lba48   = lba48;
    state.info.sectors = sectors;
    state.info.cylinders = id[1];
    state.info.heads     = id[3];
    state.info.sectors_per_track = id[6];
    copy_ata_string(state.info.model,  &id[27], 20);
    copy_ata_string(state.info.serial, &id[10], 10);

    state.device = {&state, sectors, 512, read_callback, write_callback};
    state.ready = true;
    return true;
}

const BlockDevice* primary_master() {
    return state.ready ? &state.device : nullptr;
}

const DriveInfo* primary_master_info() {
    return state.ready ? &state.info : nullptr;
}

}  // namespace aos::storage::ata_pio
