/* SPDX-License-Identifier: MIT */
#include "apic.hpp"
#include "percpu.hpp"
#include "../../memory/virtual/vmm.hpp"

extern "C" void aos_serial_write(const char* text);
extern "C" void aos_serial_write_hex64(uint64_t value);

namespace aos::apic {
namespace {

inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint64_t rd64(const uint8_t* p) { return rd32(p) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
inline bool sig_eq(const uint8_t* p, const char* s) { return p[0] == static_cast<uint8_t>(s[0]) && p[1] == static_cast<uint8_t>(s[1]) && p[2] == static_cast<uint8_t>(s[2]) && p[3] == static_cast<uint8_t>(s[3]); }
bool checksum_ok(const uint8_t* p, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; ++i) s = static_cast<uint8_t>(s + p[i]); return s == 0; }

#ifndef AOS_HOST_TEST
inline void out8(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
inline uint8_t in8(uint16_t port) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline uint64_t rdmsr(uint32_t msr) { uint32_t lo, hi; asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (static_cast<uint64_t>(hi) << 32) | lo; }
inline void wrmsr(uint32_t msr, uint64_t v) { asm volatile("wrmsr" : : "c"(msr), "a"(static_cast<uint32_t>(v)), "d"(static_cast<uint32_t>(v >> 32)) : "memory"); }

constexpr uint32_t MSR_APIC_BASE = 0x1B;
constexpr uint32_t R_ID = 0x20, R_VER = 0x30, R_TPR = 0x80, R_EOI = 0xB0, R_SVR = 0xF0, R_ESR = 0x280,
                   R_ICR_LO = 0x300, R_ICR_HI = 0x310, R_LVT_TIMER = 0x320, R_LVT_LINT0 = 0x350,
                   R_LVT_LINT1 = 0x360, R_LVT_ERR = 0x370, R_TIMER_INIT = 0x380, R_TIMER_CUR = 0x390,
                   R_TIMER_DIV = 0x3E0;
constexpr uint8_t kTimerVector = 32, kSpuriousVector = 0xFF;
constexpr unsigned kTickHz = 90;               // the kernel's tick rate (timer_ticks(), watchdog, sleep)

MadtInfo g_madt;
bool     g_have_madt = false;
bool     g_active = false;
bool     g_x2apic = false;
volatile uint8_t* g_lapic = nullptr;
uint32_t g_timer_count = 0;                    // LAPIC timer initial count for kTickHz
volatile uint64_t g_irq_count = 0;
uint32_t g_ioapic_max[kMaxIoApics];

uint32_t lapic_read(uint32_t reg) {
    if (g_x2apic) return static_cast<uint32_t>(rdmsr(0x800 + (reg >> 4)));
    volatile uint8_t* base = g_lapic;
    asm volatile("" : "+r"(base));
    return *reinterpret_cast<volatile uint32_t*>(base + reg);
}
void lapic_write(uint32_t reg, uint32_t v) {
    if (g_x2apic) { wrmsr(0x800 + (reg >> 4), v); return; }
    volatile uint8_t* base = g_lapic;
    asm volatile("" : "+r"(base));
    *reinterpret_cast<volatile uint32_t*>(base + reg) = v;
}

// ---- PIT channel 2 as a calibration stopwatch (channel 0 keeps ticking undisturbed) ----
bool pit2_wait(uint16_t count) {
    out8(0x61, static_cast<uint8_t>(in8(0x61) & ~0x03));      // gate low, speaker off
    out8(0x43, 0xB0);                                          // ch2, lobyte/hibyte, mode 0, binary
    out8(0x42, count & 0xFF);
    out8(0x42, count >> 8);
    out8(0x61, static_cast<uint8_t>((in8(0x61) & ~0x02) | 0x01));   // gate high: counting starts
    for (unsigned i = 0; i < 20000000; ++i) {
        if (in8(0x61) & 0x20) return true;
    }
    return false;
}
#endif  // !AOS_HOST_TEST
}  // namespace

// ------------------------------------------------------------------ pure parsing

bool parse_madt(const uint8_t* t, size_t length, MadtInfo* out) {
    if (!t || !out || length < 44 || !sig_eq(t, "APIC")) return false;
    const uint32_t tlen = rd32(t + 4);
    if (tlen < 44 || tlen > length) return false;
    *out = {};
    out->lapic_addr = rd32(t + 36);
    out->pcat_compat = (rd32(t + 40) & 1) != 0;
    for (size_t off = 44; off + 2 <= tlen;) {
        const uint8_t type = t[off], len = t[off + 1];
        if (len < 2 || off + len > tlen) break;
        const uint8_t* e = t + off;
        if (type == 0 && len >= 8) {                       // processor local APIC
            const uint32_t flags = rd32(e + 4);
            if (out->cpu_count < kMaxCpus)
                out->cpus[out->cpu_count++] = {e[2], e[3], (flags & 3) != 0};
        } else if (type == 9 && len >= 16) {               // processor local x2APIC
            const uint32_t flags = rd32(e + 8);
            if (out->cpu_count < kMaxCpus)
                out->cpus[out->cpu_count++] = {rd32(e + 12), rd32(e + 4), (flags & 3) != 0};
        } else if (type == 1 && len >= 12) {               // I/O APIC
            if (out->ioapic_count < kMaxIoApics)
                out->ioapics[out->ioapic_count++] = {e[2], rd32(e + 4), rd32(e + 8)};
        } else if (type == 2 && len >= 10) {               // interrupt source override
            if (out->iso_count < kMaxIso)
                out->isos[out->iso_count++] = {e[2], e[3], rd32(e + 4), rd16(e + 8)};
        } else if (type == 5 && len >= 12) {               // local APIC address override
            out->lapic_addr = rd64(e + 4);
        }
        off += len;
    }
    return out->lapic_addr != 0;
}

IrqRoute resolve_isa_irq(const MadtInfo& info, uint8_t isa_irq) {
    IrqRoute r{isa_irq, false, false};                    // ISA default: identity GSI, edge, active high
    for (unsigned i = 0; i < info.iso_count; ++i) {
        const MadtIso& o = info.isos[i];
        if (o.bus != 0 || o.source != isa_irq) continue;
        r.gsi = o.gsi;
        const unsigned pol = o.flags & 3, trg = (o.flags >> 2) & 3;
        r.active_low = (pol == 3);
        r.level = (trg == 3);
        break;
    }
    return r;
}

uint32_t ioapic_entry_low(uint8_t vector, const IrqRoute& r, bool masked) {
    return vector | (r.active_low ? (1u << 13) : 0u) | (r.level ? (1u << 15) : 0u) | (masked ? (1u << 16) : 0u);
}

const uint8_t* find_table(const uint8_t* mbi, const char* signature, size_t* length) {
    const uint8_t* rsdp = nullptr;
    if (mbi) {                                             // Multiboot2 hands us a copy of the RSDP
        const uint32_t total = rd32(mbi);
        for (uint32_t off = 8; off + 8 <= total;) {
            const uint32_t type = rd32(mbi + off), size = rd32(mbi + off + 4);
            if (type == 0 || size < 8) break;
            if ((type == 14 || type == 15) && size >= 8 + 20) { rsdp = mbi + off + 8; break; }
            off = (off + size + 7) & ~7u;
        }
    }
#ifndef AOS_HOST_TEST
    if (!rsdp || !sig_eq(rsdp, "RSD ") ) {                 // BIOS scan: EBDA, then 0xE0000-0xFFFFF
        rsdp = nullptr;
        uintptr_t bda = 0x40E;                             // BIOS data area: EBDA segment
        asm volatile("" : "+r"(bda));                      // opaque to the optimizer: this is a real low-memory address
        const uint32_t ebda = static_cast<uint32_t>(*reinterpret_cast<const volatile uint16_t*>(bda)) << 4;
        const uint32_t ranges[2][2] = {{ebda, ebda + 1024}, {0xE0000, 0x100000}};
        for (auto& rg : ranges) {
            if (rg[0] < 0x400) continue;
            for (uint32_t a = rg[0] & ~15u; a + 20 <= rg[1] && !rsdp; a += 16) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(a));
                if (sig_eq(p, "RSD ") && sig_eq(p + 4, "PTR ") && checksum_ok(p, 20)) rsdp = p;
            }
        }
    }
#endif
    if (!rsdp || !sig_eq(rsdp, "RSD ") || !sig_eq(rsdp + 4, "PTR ")) return nullptr;
    const uint8_t rev = rsdp[15];
    uint64_t root = 0;
    bool wide = false;
    if (rev >= 2 && rd64(rsdp + 24)) { root = rd64(rsdp + 24); wide = true; }
    if (!root) root = rd32(rsdp + 16);
    if (!root || root >= 0x100000000ULL) return nullptr;
    const uint8_t* r = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(root));
    const uint32_t rlen = rd32(r + 4);
    if (rlen < 36 || rlen > 0x100000) return nullptr;
    const unsigned esz = wide ? 8 : 4;
    for (uint32_t off = 36; off + esz <= rlen; off += esz) {
        const uint64_t addr = wide ? rd64(r + off) : rd32(r + off);
        if (!addr || addr >= 0x100000000ULL) continue;
        const uint8_t* t = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
        if (!sig_eq(t, signature)) continue;
        const uint32_t tl = rd32(t + 4);
        if (tl < 36 || tl > 0x100000 || !checksum_ok(t, tl)) continue;
        if (length) *length = tl;
        return t;
    }
    return nullptr;
}

// ------------------------------------------------------------------ kernel side
#ifndef AOS_HOST_TEST
namespace {

volatile uint32_t* ioapic_regs(unsigned i) { return reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(g_madt.ioapics[i].addr)); }
[[maybe_unused]] uint32_t io_read(unsigned i, uint8_t reg) { volatile uint32_t* r = ioapic_regs(i); r[0] = reg; return r[4]; }
void io_write(unsigned i, uint8_t reg, uint32_t v) { volatile uint32_t* r = ioapic_regs(i); r[0] = reg; r[4] = v; }

int ioapic_for_gsi(uint32_t gsi) {
    for (unsigned i = 0; i < g_madt.ioapic_count; ++i)
        if (gsi >= g_madt.ioapics[i].gsi_base && gsi <= g_madt.ioapics[i].gsi_base + g_ioapic_max[i]) return static_cast<int>(i);
    return -1;
}

[[maybe_unused]] bool route_isa(uint8_t irq, uint8_t vector, uint32_t dest, bool masked) {
    const IrqRoute r = resolve_isa_irq(g_madt, irq);
    const int io = ioapic_for_gsi(r.gsi);
    if (io < 0) return false;
    const uint8_t pin = static_cast<uint8_t>(r.gsi - g_madt.ioapics[io].gsi_base);
    io_write(static_cast<unsigned>(io), static_cast<uint8_t>(0x10 + 2 * pin), ioapic_entry_low(vector, r, true));   // mask while editing
    io_write(static_cast<unsigned>(io), static_cast<uint8_t>(0x11 + 2 * pin), dest << 24);
    io_write(static_cast<unsigned>(io), static_cast<uint8_t>(0x10 + 2 * pin), ioapic_entry_low(vector, r, masked));
    return true;
}

void lapic_enable_local() {
    lapic_write(R_TPR, 0);
    lapic_write(R_SVR, 0x100 | kSpuriousVector);
    lapic_write(R_LVT_ERR, 1u << 16);
    lapic_write(R_LVT_LINT0, 1u << 16);                   // ExtINT is masked: the 8259 is switched off
    lapic_write(R_LVT_LINT1, 1u << 16);
    lapic_write(R_TIMER_DIV, 0x3);                        // divide by 16
}

[[maybe_unused]] void pic_set_masks(uint8_t master, uint8_t slave) { out8(0x21, master); out8(0xA1, slave); }
}  // namespace

bool active() { return g_active; }
uint64_t lapic_irq_count() { return g_irq_count; }
const MadtInfo* madt() { return g_have_madt ? &g_madt : nullptr; }
unsigned timer_hz() { return g_active ? kTickHz : 0; }
void eoi() { ++g_irq_count; lapic_write(R_EOI, 0); }
uint32_t lapic_id() { return g_x2apic ? lapic_read(R_ID) : (lapic_read(R_ID) >> 24); }
unsigned cpu_count() {
    if (!g_have_madt || !g_active) return 1;
    unsigned n = 0;
    for (unsigned i = 0; i < g_madt.cpu_count; ++i) if (g_madt.cpus[i].enabled) ++n;
    return n ? n : 1;
}

void udelay_ms(unsigned ms) {
    while (ms--) pit2_wait(1193);                         // ~1 ms
}

void enable_this_cpu() {
    lapic_enable_local();
    lapic_write(R_LVT_TIMER, kTimerVector | (1u << 17));  // periodic
    lapic_write(R_TIMER_INIT, g_timer_count);
}

bool send_init_sipi(uint32_t apic_id, uint8_t page) {
    auto icr = [&](uint32_t lo) {
        if (g_x2apic) { wrmsr(0x830, (static_cast<uint64_t>(apic_id) << 32) | lo); return; }
        lapic_write(R_ICR_HI, apic_id << 24);
        lapic_write(R_ICR_LO, lo);
        for (unsigned i = 0; i < 100000 && (lapic_read(R_ICR_LO) & (1u << 12)); ++i) asm volatile("pause");
    };
    icr(0x00004500);                                      // INIT, level assert
    udelay_ms(10);
    icr(0x00004600 | page);                               // STARTUP
    udelay_ms(1);
    icr(0x00004600 | page);                               // second STARTUP per the MP spec
    return true;
}

bool init(const uint8_t* mbi) {
#ifdef AOS_NO_APIC
    (void)mbi;
    return false;
#else
    size_t len = 0;
    const uint8_t* t = find_table(mbi, "APIC", &len);
    if (!t || !parse_madt(t, len, &g_madt)) { aos_serial_write("[apic] no MADT, staying on the 8259 PIC\r\n"); return false; }
    g_have_madt = true;
    if (!g_madt.ioapic_count) { aos_serial_write("[apic] no I/O APIC in MADT, staying on the 8259 PIC\r\n"); return false; }

    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 9))) { aos_serial_write("[apic] CPU has no LAPIC\r\n"); return false; }

    uint64_t base_msr = rdmsr(MSR_APIC_BASE);
    g_x2apic = (base_msr & (1ULL << 10)) != 0;            // firmware already switched to x2APIC mode
    if (!(base_msr & (1ULL << 11))) { base_msr |= (1ULL << 11); wrmsr(MSR_APIC_BASE, base_msr); }
    if (!g_x2apic) {
        const uint64_t phys = base_msr & 0x000FFFFFFFFFF000ULL;
        if (phys >= 0x100000000ULL) { aos_serial_write("[apic] LAPIC above 4 GiB, unsupported\r\n"); return false; }
        if (!aos::vmm::set_cache_mode(phys, 4096, aos::vmm::CacheMode::Uncached)) { aos_serial_write("[apic] cannot map LAPIC\r\n"); return false; }
        g_lapic = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(phys));
    }
    for (unsigned i = 0; i < g_madt.ioapic_count; ++i) {
        if (!aos::vmm::set_cache_mode(g_madt.ioapics[i].addr, 4096, aos::vmm::CacheMode::Uncached)) return false;
        g_ioapic_max[i] = (io_read(i, 1) >> 16) & 0xFF;
    }
    const uint32_t bsp_id = lapic_id();
    if (bsp_id > 0xFE) { aos_serial_write("[apic] BSP APIC id does not fit an I/O APIC destination\r\n"); return false; }

    lapic_enable_local();

    // ---- calibrate the LAPIC timer against PIT channel 2 (20 ms window)
    lapic_write(R_LVT_TIMER, kTimerVector | (1u << 16));  // one-shot, masked
    lapic_write(R_TIMER_INIT, 0xFFFFFFFF);
    if (!pit2_wait(23864)) { lapic_write(R_TIMER_INIT, 0); aos_serial_write("[apic] PIT calibration timed out\r\n"); return false; }
    const uint32_t elapsed = 0xFFFFFFFFu - lapic_read(R_TIMER_CUR);
    lapic_write(R_TIMER_INIT, 0);
    const uint64_t per_sec = static_cast<uint64_t>(elapsed) * 50;
    if (elapsed < 1000 || per_sec / kTickHz < 100) { aos_serial_write("[apic] implausible LAPIC timer rate\r\n"); return false; }
    g_timer_count = static_cast<uint32_t>(per_sec / kTickHz);

    // ---- self test: let the LAPIC timer tick while the PIT's IRQ0 is masked in the PIC
    const uint64_t before = g_irq_count;
    uint64_t f;
    asm volatile("pushfq; pop %0; cli" : "=r"(f) : : "memory");
    pic_set_masks(0xFF, 0xFF);                            // nothing from the 8259 during the test
    out8(0x20, 0x20);                                     // clear a possibly latched in-service bit
    g_active = true;                                      // vector 32 now means "LAPIC": EOI goes to the LAPIC
    lapic_write(R_LVT_TIMER, kTimerVector | (1u << 17));
    lapic_write(R_TIMER_INIT, g_timer_count);
    if (f & 0x200) asm volatile("sti" : : : "memory");
    pit2_wait(47727);                                     // 40 ms
    const bool ticking = (g_irq_count - before) >= 2;
    if (!ticking) {                                       // revert to the legacy controller
        asm volatile("cli" : : : "memory");
        lapic_write(R_LVT_TIMER, 1u << 16);
        lapic_write(R_TIMER_INIT, 0);
        g_active = false;
        out8(0x20, 0x20);
        pic_set_masks(0xF8, 0xEF);
        if (f & 0x200) asm volatile("sti" : : : "memory");
        aos_serial_write("[apic] LAPIC timer did not tick, staying on the 8259 PIC\r\n");
        return false;
    }

    // Keyboard and mouse through the I/O APIC (the PS/2 poll fallback still covers broken routing).
    const bool kb = route_isa(1, 33, bsp_id, false);
    const bool ms = route_isa(12, 44, bsp_id, false);
    (void)kb; (void)ms;
    return true;
#endif
}

void describe(void (*print)(const char* text)) {
    auto hex = [&](uint64_t v) {
        static const char* d = "0123456789abcdef";
        char buf[19]; buf[0] = '0'; buf[1] = 'x';
        int n = 0; char tmp[17];
        if (!v) tmp[n++] = '0';
        while (v) { tmp[n++] = d[v & 15]; v >>= 4; }
        int k = 2; while (n) buf[k++] = tmp[--n];
        buf[k] = 0; print(buf);
    };
    auto dec = [&](uint64_t v) {
        char tmp[21]; int n = 0; char buf[21];
        if (!v) tmp[n++] = '0';
        while (v) { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; }
        int k = 0; while (n) buf[k++] = tmp[--n];
        buf[k] = 0; print(buf);
    };
    if (!g_have_madt) { print("APIC: not used (no MADT) - legacy 8259 PIC + PIT\n"); return; }
    print(g_active ? "APIC: active, interrupt controller = LAPIC + I/O APIC" : "APIC: present but NOT active (fell back to 8259 PIC + PIT)");
    print(g_x2apic ? " [x2APIC]\n" : " [xAPIC]\n");
    print("  LAPIC base "); hex(g_madt.lapic_addr); print(", BSP id "); dec(g_active ? lapic_id() : 0);
    print(", timer "); dec(g_active ? kTickHz : 0); print(" Hz, count "); dec(g_timer_count); print("\n");
    for (unsigned i = 0; i < g_madt.ioapic_count; ++i) {
        print("  IOAPIC "); dec(g_madt.ioapics[i].id); print(" @ "); hex(g_madt.ioapics[i].addr);
        print(" gsi base "); dec(g_madt.ioapics[i].gsi_base); print("\n");
    }
    for (unsigned i = 0; i < g_madt.iso_count; ++i) {
        print("  override: ISA IRQ "); dec(g_madt.isos[i].source); print(" -> GSI "); dec(g_madt.isos[i].gsi);
        print(" flags "); hex(g_madt.isos[i].flags); print("\n");
    }
    for (unsigned i = 0; i < g_madt.cpu_count; ++i) {
        print("  CPU apic id "); dec(g_madt.cpus[i].apic_id); print(g_madt.cpus[i].enabled ? " enabled\n" : " disabled\n");
    }
    print("  LAPIC interrupts handled: "); dec(g_irq_count); print("\n");
}
#endif  // !AOS_HOST_TEST
}  // namespace aos::apic
