/* SPDX-License-Identifier: MIT */
#include "acpi.hpp"

namespace aos::acpi {
namespace {

struct Table { const uint8_t* p; uint32_t len; };
Table g_tab[24];
int g_ntab = 0;
const uint8_t* g_fadt = nullptr; const uint8_t* g_dsdt = nullptr;

inline uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }
inline bool sig(const uint8_t* p, const char* s) { return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3]; }

const uint8_t* map(uint64_t phys) { return phys && phys < 0x100000000ULL ? reinterpret_cast<const uint8_t*>(phys) : nullptr; }

void add(const uint8_t* t) {
    if (!t || g_ntab >= 24) return;
    const uint32_t len = u32(t + 4);
    if (len < 36 || len > (4u << 20)) return;
    g_tab[g_ntab++] = Table{t, len};
}

bool match(const uint8_t* p, const char* s, int n) {
    for (int i = 0; i < n; ++i) if (p[i] != static_cast<uint8_t>(s[i])) return false;
    return true;
}

// Is there a PNP0C50/ACPI0C50 _HID at p?  (string form or compressed EISA id form)
bool is_hid_marker(const uint8_t* p, const uint8_t* end) {
    if (p + 8 > end) return false;
    if (match(p, "ACPI0C50", 8)) return true;
    if (match(p, "PNP0C50", 7)) return true;
    return p[0] == 0x41 && p[1] == 0xD0 && p[2] == 0x0C && p[3] == 0x50;
}

// Find "5B 82 <PkgLength> name" and the Memory32Fixed resource inside that Device package.
uint64_t find_controller_mmio(const char* name) {
    for (int t = 0; t < g_ntab; ++t) {
        const uint8_t* b = g_tab[t].p; const uint32_t n = g_tab[t].len;
        for (uint32_t i = 36; i + 8 < n; ++i) {
            if (b[i] != 0x5B || b[i + 1] != 0x82) continue;
            const uint8_t lead = b[i + 2];
            const uint32_t extra = lead >> 6;
            const uint32_t name_at = i + 3 + extra;
            if (name_at + 4 > n || !match(b + name_at, name, 4)) continue;
            uint32_t pkg = extra == 0 ? (lead & 0x3F) : (lead & 0x0F);
            for (uint32_t k = 0; k < extra; ++k) pkg |= static_cast<uint32_t>(b[i + 3 + k]) << (4 + 8 * k);
            uint32_t end = i + 2 + pkg;
            if (end > n) end = n;
            for (uint32_t j = name_at + 4; j + 12 <= end; ++j)
                if (b[j] == 0x86 && b[j + 1] == 0x09 && b[j + 2] == 0x00) {
                    const uint64_t base = u32(b + j + 4);
                    if (base) return base;
                }
        }
    }
    return 0;
}

}  // namespace

bool init(const uint8_t* mbi) {
    g_ntab = 0;
    if (!mbi) return false;
    const uint32_t total = u32(mbi);
    const uint8_t* rsdp = nullptr;
    for (uint32_t off = 8; off + 8 <= total;) {
        const uint32_t type = u32(mbi + off), size = u32(mbi + off + 4);
        if (type == 0 || size < 8) break;
        if (type == 14 || type == 15) rsdp = mbi + off + 8;
        off = (off + size + 7) & ~7u;
    }
    if (!rsdp || !match(rsdp, "RSD PTR ", 8)) return false;

    const uint8_t rev = rsdp[15];
    const uint8_t* root = nullptr; bool wide = false;
    if (rev >= 2 && u64(rsdp + 24)) { root = map(u64(rsdp + 24)); wide = true; }
    if (!root) { root = map(u32(rsdp + 16)); wide = false; }
    if (!root) return false;
    const uint32_t rlen = u32(root + 4);
    if (rlen < 36 || rlen > (1u << 20)) return false;
    const uint32_t cnt = (rlen - 36) / (wide ? 8 : 4);
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint8_t* t = map(wide ? u64(root + 36 + 8 * i) : u32(root + 36 + 4 * i));
        if (!t) continue;
        if (sig(t, "FACP")) {
            const uint32_t flen = u32(t + 4);
            const uint8_t* dsdt = nullptr;
            if (flen >= 148 && u64(t + 140)) dsdt = map(u64(t + 140));
            if (!dsdt && flen >= 44) dsdt = map(u32(t + 40));
            g_fadt = t; g_dsdt = dsdt;
            add(dsdt);
        } else if (sig(t, "SSDT")) {
            add(t);
        }
    }
    return g_ntab > 0;
}

int find_i2c_hid(I2cHidInfo* out, int max) {
    int n = 0;
    for (int t = 0; t < g_ntab && n < max; ++t) {
        const uint8_t* b = g_tab[t].p; const uint32_t len = g_tab[t].len;
        const uint8_t* end = b + len;
        for (const uint8_t* p = b + 36; p + 8 < end && n < max; ++p) {
            if (!is_hid_marker(p, end)) continue;
            // the device's _CRS holds an I2cSerialBusV2 descriptor within the next couple of KiB
            for (const uint8_t* q = p; q + 24 < end && q < p + 2048; ++q) {
                if (q[0] != 0x8E || q[5] != 1 || q[9] != 1) continue;
                const uint32_t dlen = q[1] | (q[2] << 8), tlen = q[10] | (q[11] << 8);
                if (dlen < 0x0F || dlen > 0x80 || tlen < 6 || 12 + tlen > dlen + 3) continue;
                I2cHidInfo info{};
                info.addr = static_cast<uint16_t>(q[16] | (q[17] << 8));
                const char* src = reinterpret_cast<const char*>(q + 12 + tlen);       // "\_SB.I2C1"
                int last = -1, sl = 0;
                while (sl < 32 && src[sl] && q + 12 + tlen + sl < end) { if (src[sl] == '.' || src[sl] == '\\') last = sl; ++sl; }
                const char* nm = src + last + 1;
                for (int k = 0; k < 4; ++k) info.bus_name[k] = (k < sl - last - 1) ? nm[k] : '_';
                info.bus_name[4] = 0;
                info.bus_mmio = find_controller_mmio(info.bus_name);
                bool dup = false;
                for (int k = 0; k < n; ++k) if (out[k].addr == info.addr && out[k].bus_name[3] == info.bus_name[3]) dup = true;
                if (!dup && (info.addr & 0x7F) == info.addr && info.addr) out[n++] = info;
                break;
            }
            p += 7;
        }
    }
    return n;
}

namespace {
inline void outw_(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
inline void outb_(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
inline uint16_t inw_(uint16_t port) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
// Find \_S5_ in the DSDT: 08 5F 53 35 5F 12 <PkgLen> <n> (0A) typa (0A) typb
bool find_s5(uint8_t* a, uint8_t* b) {
    if (!g_dsdt) return false;
    const uint32_t n = u32(g_dsdt + 4);
    for (uint32_t i = 36; i + 12 < n; ++i) {
        if (!match(g_dsdt + i, "_S5_", 4) || g_dsdt[i + 4] != 0x12) continue;
        uint32_t k = i + 5; k += (g_dsdt[k] >> 6) + 1; ++k;          // skip PkgLength + NumElements
        if (g_dsdt[k] == 0x0A) { ++k; }
        *a = g_dsdt[k++];
        if (g_dsdt[k] == 0x0A) { ++k; }
        *b = g_dsdt[k];
        return true;
    }
    return false;
}
}  // namespace

void poweroff() {
    // Emulators first (harmless on real hardware): QEMU/Bochs/VirtualBox ACPI-less shutdown ports.
    outw_(0x604, 0x2000); outw_(0xB004, 0x2000); outw_(0x4004, 0x3400);
    if (!g_fadt) return;
    const uint32_t pm1a = u32(g_fadt + 64), pm1b = u32(g_fadt + 68), smi = u32(g_fadt + 48);
    const uint8_t enable = g_fadt[52];
    uint8_t ta = 0, tb = 0;
    if (!pm1a || !find_s5(&ta, &tb)) return;
    if (!(inw_(static_cast<uint16_t>(pm1a)) & 1) && smi && enable) {       // SCI_EN clear: ask firmware to switch to ACPI mode
        outb_(static_cast<uint16_t>(smi), enable);
        for (int i = 0; i < 300000 && !(inw_(static_cast<uint16_t>(pm1a)) & 1); ++i) asm volatile("pause");
    }
    outw_(static_cast<uint16_t>(pm1a), static_cast<uint16_t>((ta << 10) | (1 << 13)));
    if (pm1b) outw_(static_cast<uint16_t>(pm1b), static_cast<uint16_t>((tb << 10) | (1 << 13)));
}

}  // namespace aos::acpi
