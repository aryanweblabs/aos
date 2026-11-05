// core_test.cpp - host-side unit tests for the new core-kernel pieces that are pure logic:
//   * scheduler policy (priorities, round robin, aging, preemption, pinning)
//   * ACPI MADT parsing, ISA->GSI routing, I/O APIC entry encoding, RSDP/XSDT table lookup
//   * ELF loader validation (against the real user programs and corrupted variants)
//   * per-process address spaces (page tables, copies, isolation, leak-free teardown)
// The hardware-facing code (context switch, APIC registers, ring-3 entry) cannot run here; see README.
#define AOS_HOST_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vector>
#include "../../src/scheduler/sched_policy.hpp"
#include "../../src/arch/x86_64/apic.hpp"
#include "../../src/process/elf.hpp"
#include "../../src/memory/virtual/aspace.hpp"
#include "../../src/memory/virtual/vmm.hpp"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------- physical memory shim (all frames below 2 GiB)
static const size_t kFrames = 4096;
static uint8_t* pool;
static std::vector<bool> used;
static size_t free_frames;
extern "C" uint64_t pmm_alloc_frame(void) {
    for (size_t i = 0; i < kFrames; ++i) if (!used[i]) { used[i] = true; --free_frames; return reinterpret_cast<uint64_t>(pool + i * 4096); }
    return 0;
}
extern "C" void pmm_free_frame(uint64_t a) {
    const size_t i = (a - reinterpret_cast<uint64_t>(pool)) / 4096;
    if (i < kFrames && used[i]) { used[i] = false; ++free_frames; }
}
namespace aos::vmm {
static uint64_t g_master;
uint64_t kernel_cr3() { return g_master; }
}

// ---------------------------------------------------------------- scheduler policy
using namespace aos::scheduler::policy;
struct T {
    uint8_t state, prio, eff_prio; uint32_t flags, wait_ticks; uint64_t wake_tick; int32_t pinned_cpu, on_cpu;
    uint32_t slice_left; uint64_t ran;
};
static T mk(uint8_t prio, uint32_t flags = 0, int pinned = -1) {
    T t{}; t.state = ST_READY; t.prio = t.eff_prio = prio; t.flags = flags; t.pinned_cpu = pinned; t.on_cpu = -1; return t;
}
// Mirrors aos::scheduler::on_tick()/switch_locked() for one CPU.
struct Sim {
    std::vector<T> t; unsigned cursor[kLevels] = {}; int cur = -1; uint64_t now = 0; unsigned cpu = 0;
    void dispatch(int n) {
        if (cur >= 0) { t[cur].on_cpu = -1; if (t[cur].state == ST_RUNNING) t[cur].state = ST_READY; }
        cur = n; t[n].state = ST_RUNNING; t[n].on_cpu = cpu; on_dispatch(t[n]); t[n].slice_left = slice_for(t[n].prio);
    }
    void reschedule() {
        if (cur >= 0) { t[cur].on_cpu = -1; if (t[cur].state == ST_RUNNING) t[cur].state = ST_READY; }
        int n = pick_next(t.data(), t.size(), cpu, now, cursor);
        if (n < 0) { printf("  (nothing runnable)\n"); ++g_fail; return; }
        dispatch(n);
    }
    void tick() {
        ++now; age(t.data(), t.size(), now);
        T& c = t[cur]; ++c.ran;
        bool need;
        if (c.flags & TASK_IDLE) need = any_ready(t.data(), t.size(), cpu, now, kIdleLevel);
        else { if (c.slice_left) --c.slice_left; need = !c.slice_left || better_ready(t.data(), t.size(), cpu, now, c.eff_prio); }
        if (need) reschedule();
    }
};

static void test_policy() {
    printf("scheduler policy\n");
    { // equal priorities share the CPU evenly
        Sim s; s.t = {mk(2), mk(2), mk(2), mk(4, TASK_IDLE, 0)}; s.reschedule();
        for (int i = 0; i < 9000; ++i) s.tick();
        uint64_t a = s.t[0].ran, b = s.t[1].ran, c = s.t[2].ran;
        printf("  3 equal threads: %llu %llu %llu\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
        CHECK(a > 2700 && b > 2700 && c > 2700 && a < 3300 && b < 3300 && c < 3300);
        CHECK(s.t[3].ran == 0);                                         // idle never runs while work exists
    }
    { // higher priority wins, but aging prevents starvation of the low one
        Sim s; s.t = {mk(1), mk(3), mk(4, TASK_IDLE, 0)}; s.reschedule();
        for (int i = 0; i < 9000; ++i) s.tick();
        printf("  prio1 vs prio3: %llu vs %llu\n", (unsigned long long)s.t[0].ran, (unsigned long long)s.t[1].ran);
        CHECK(s.t[0].ran > s.t[1].ran * 2);                              // clear preference
        CHECK(s.t[1].ran > 50);                                          // ...but the low thread is not starved (aging gives it ~1%)
    }
    { // a sleeping urgent thread preempts a running low one as soon as it is due
        Sim s; s.t = {mk(0), mk(3), mk(4, TASK_IDLE, 0)}; s.t[0].wake_tick = 100; s.reschedule();
        CHECK(s.cur == 1);
        for (int i = 0; i < 99; ++i) s.tick();
        CHECK(s.cur == 1);
        s.tick();                                                        // now == 100: urgent thread became due
        CHECK(s.cur == 0);
    }
    { // idle runs when everything sleeps, and gives way when work appears
        Sim s; s.t = {mk(2), mk(4, TASK_IDLE, 0)}; s.t[0].wake_tick = 50; s.reschedule();
        CHECK(s.cur == 1);
        for (int i = 0; i < 49; ++i) s.tick();
        CHECK(s.cur == 1);
        s.tick();
        CHECK(s.cur == 0);
    }
    { // CPU affinity: APs only take SMP_OK threads, pinned threads stay put
        T plain = mk(2), smp = mk(2, TASK_SMP_OK), pin0 = mk(2, 0, 0), idle1 = mk(4, TASK_IDLE, 1);
        CHECK(!eligible(plain, 1, 0)); CHECK(eligible(plain, 0, 0));
        CHECK(eligible(smp, 1, 0)); CHECK(!eligible(pin0, 1, 0)); CHECK(eligible(pin0, 0, 0));
        CHECK(eligible(idle1, 1, 0)); CHECK(!eligible(idle1, 0, 0));
        T running = mk(2); running.on_cpu = 0; CHECK(!eligible(running, 1, 0));   // never on two CPUs at once
    }
    { // slices shrink with priority
        CHECK(slice_for(0) > slice_for(2)); CHECK(slice_for(2) > slice_for(3)); CHECK(slice_for(4) >= 1);
    }
}

// ---------------------------------------------------------------- ACPI / APIC
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static void fix_checksum(uint8_t* t, size_t len, size_t at = 9) { t[at] = 0; uint8_t s = 0; for (size_t i = 0; i < len; ++i) s += t[i]; t[at] = (uint8_t)(0 - s); }

static size_t build_madt(uint8_t* t) {
    memset(t, 0, 512); memcpy(t, "APIC", 4);
    put32(t + 36, 0xFEE00000); put32(t + 40, 1);
    size_t o = 44;
    auto lapic = [&](uint8_t acpi, uint8_t id, uint32_t fl) { t[o] = 0; t[o+1] = 8; t[o+2] = acpi; t[o+3] = id; put32(t+o+4, fl); o += 8; };
    lapic(0, 0, 1); lapic(1, 2, 1); lapic(2, 4, 0);                      // third CPU disabled
    t[o] = 1; t[o+1] = 12; t[o+2] = 1; put32(t+o+4, 0xFEC00000); put32(t+o+8, 0); o += 12;       // IOAPIC 1
    t[o] = 1; t[o+1] = 12; t[o+2] = 2; put32(t+o+4, 0xFEC01000); put32(t+o+8, 24); o += 12;      // IOAPIC 2, GSI 24..
    t[o] = 2; t[o+1] = 10; t[o+2] = 0; t[o+3] = 0; put32(t+o+4, 2); t[o+8] = 0; t[o+9] = 0; o += 10;        // IRQ0 -> GSI2
    t[o] = 2; t[o+1] = 10; t[o+2] = 0; t[o+3] = 9; put32(t+o+4, 9); t[o+8] = 0x0F; t[o+9] = 0; o += 10;    // IRQ9 level, active low
    t[o] = 2; t[o+1] = 10; t[o+2] = 0; t[o+3] = 12; put32(t+o+4, 28); t[o+8] = 0; t[o+9] = 0; o += 10;     // IRQ12 -> GSI28 (second IOAPIC)
    t[o] = 4; t[o+1] = 6; o += 6;                                          // NMI: ignored
    t[o] = 5; t[o+1] = 12; put64(t+o+4, 0xFEE01000); o += 12;              // LAPIC address override
    put32(t + 4, (uint32_t)o); fix_checksum(t, o);
    return o;
}

static void test_apic() {
    printf("ACPI MADT / IOAPIC routing\n");
    uint8_t t[512]; const size_t len = build_madt(t);
    aos::apic::MadtInfo m;
    CHECK(aos::apic::parse_madt(t, len, &m));
    CHECK(m.lapic_addr == 0xFEE01000);                                    // override wins
    CHECK(m.pcat_compat);
    CHECK(m.cpu_count == 3 && m.cpus[0].enabled && m.cpus[1].enabled && !m.cpus[2].enabled);
    CHECK(m.cpus[1].apic_id == 2);
    CHECK(m.ioapic_count == 2 && m.ioapics[1].addr == 0xFEC01000 && m.ioapics[1].gsi_base == 24);
    CHECK(m.iso_count == 3);
    auto r1 = aos::apic::resolve_isa_irq(m, 1);                          // no override: identity, edge, high
    CHECK(r1.gsi == 1 && !r1.active_low && !r1.level);
    auto r0 = aos::apic::resolve_isa_irq(m, 0); CHECK(r0.gsi == 2);
    auto r9 = aos::apic::resolve_isa_irq(m, 9); CHECK(r9.gsi == 9 && r9.active_low && r9.level);
    auto r12 = aos::apic::resolve_isa_irq(m, 12); CHECK(r12.gsi == 28);
    CHECK(aos::apic::ioapic_entry_low(33, r1, false) == 33);
    CHECK(aos::apic::ioapic_entry_low(44, r1, true) == (44u | (1u << 16)));
    CHECK(aos::apic::ioapic_entry_low(0x41, r9, false) == (0x41u | (1u << 13) | (1u << 15)));
    // corrupt input must be rejected, never crash
    uint8_t bad[512]; memcpy(bad, t, len);
    CHECK(!aos::apic::parse_madt(bad, 20, &m));
    memcpy(bad, "XXXX", 4); CHECK(!aos::apic::parse_madt(bad, len, &m));
    memcpy(bad, t, len); put32(bad + 4, 100000); CHECK(!aos::apic::parse_madt(bad, len, &m));
    memcpy(bad, t, len); bad[44 + 1] = 0; CHECK(aos::apic::parse_madt(bad, len, &m));   // zero-length entry: stops, no loop forever
    // find_table through a fake multiboot2 RSDP tag + XSDT
    uint8_t* mem = (uint8_t*)mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(mem != MAP_FAILED);
    uint8_t* madt = mem + 0x1000; memcpy(madt, t, len);
    uint8_t* xsdt = mem + 0x2000; memset(xsdt, 0, 64); memcpy(xsdt, "XSDT", 4);
    put32(xsdt + 4, 36 + 16); put64(xsdt + 36, 0);                        // empty slot must be skipped
    put64(xsdt + 44, (uint64_t)(uintptr_t)madt); fix_checksum(xsdt, 52);
    uint8_t* mbi = mem + 0x3000; memset(mbi, 0, 128);
    uint8_t* tag = mbi + 8; put32(tag, 15); put32(tag + 4, 8 + 36);
    uint8_t* rsdp = tag + 8; memcpy(rsdp, "RSD PTR ", 8); rsdp[15] = 2; put32(rsdp + 20, 36); put64(rsdp + 24, (uint64_t)(uintptr_t)xsdt);
    put32(mbi, 8 + 8 + 36 + 8);
    size_t flen = 0;
    const uint8_t* found = aos::apic::find_table(mbi, "APIC", &flen);
    CHECK(found == madt && flen == len);
    CHECK(aos::apic::find_table(mbi, "HPET", &flen) == nullptr);
    madt[10] ^= 0xFF;                                                     // break the checksum
    CHECK(aos::apic::find_table(mbi, "APIC", &flen) == nullptr);
    munmap(mem, 1 << 16);
}

// ---------------------------------------------------------------- ELF loader
static std::vector<uint8_t> slurp(const char* path) {
    std::vector<uint8_t> v; FILE* f = fopen(path, "rb"); if (!f) return v;
    uint8_t buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f); return v;
}
static void test_elf(const char* dir) {
    printf("ELF loader validation\n");
    using aos::elf::Err;
    const char* names[] = {"hello.elf", "counter.elf", "fault.elf"};
    aos::elf::Image img;
    for (const char* n : names) {
        char path[512]; snprintf(path, sizeof path, "%s/%s", dir, n);
        auto d = slurp(path);
        CHECK(!d.empty());
        if (d.empty()) { printf("  (skipping %s: build user programs first)\n", n); continue; }
        Err e = aos::elf::parse(d.data(), d.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img);
        CHECK(e == Err::Ok);
        CHECK(img.entry >= aos::vmm::kUserBase && img.segment_count >= 1);
        // truncated at every interesting length must fail cleanly
        for (size_t cut : {size_t(0), size_t(10), size_t(63), size_t(64), size_t(100), d.size() / 2})
            CHECK(aos::elf::parse(d.data(), cut, aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) != Err::Ok);
        auto m = d; m[0] = 0; CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::BadMagic);
        m = d; m[4] = 1; CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::NotElf64);
        m = d; m[18] = 3; CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::NotX86_64);
        m = d; m[16] = 3; CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::NotExecutable);
        m = d; { uint64_t ph; memcpy(&ph, &m[32], 8); uint64_t low = 0x1000; memcpy(&m[ph + 16], &low, 8); }   // PT_LOAD vaddr into the kernel's range
        CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::BadSegment);
        m = d; { uint64_t e0 = aos::vmm::kUserBase + 0x100000; memcpy(&m[24], &e0, 8); }                      // entry outside the code
        CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::BadEntry);
        m = d; { uint64_t huge = 1ULL << 40; uint64_t ph; memcpy(&ph, &m[32], 8); memcpy(&m[ph + 40], &huge, 8); }  // absurd memsz
        CHECK(aos::elf::parse(m.data(), m.size(), aos::vmm::kUserBase, aos::vmm::kUserEnd, &img) == Err::BadSegment);
    }
}

// ---------------------------------------------------------------- address spaces
static void test_aspace() {
    printf("address spaces\n");
    using namespace aos::vmm;
    // kernel master PML4 with recognisable entries in the shared halves
    uint64_t* master = (uint64_t*)pmm_alloc_frame(); memset(master, 0, 4096);
    master[0] = 0x1003; master[256] = 0x2003; master[511] = 0x3003;
    g_master = (uint64_t)(uintptr_t)master;
    const size_t base_free = free_frames;

    AddressSpace a, b;
    CHECK(as_create(&a) && as_create(&b));
    uint64_t* pa = (uint64_t*)(uintptr_t)a.pml4;
    CHECK(pa[0] == 0x1003 && pa[256] == 0x2003 && pa[511] == 0x3003);   // kernel half inherited
    CHECK(pa[1] == 0);                                                  // user half private and empty
    CHECK(user_range_ok(kUserBase, 4096) && !user_range_ok(0x1000, 1) && !user_range_ok(kUserEnd - 1, 2) && !user_range_ok(~0ULL, 2));

    CHECK(as_alloc(&a, kUserBase, 3, PF_WRITE | PF_NX));
    CHECK(a.pages == 3);
    uint64_t phys, fl;
    CHECK(as_translate(&a, kUserBase + 4096 + 7, &phys, &fl) && (phys & 4095) == 7 && (fl & PF_USER) && (fl & PF_WRITE) && (fl & PF_NX));
    CHECK(!as_translate(&a, kUserBase + 3 * 4096, &phys, &fl));         // not mapped
    CHECK(!as_translate(&b, kUserBase, &phys, &fl));                    // other process cannot see it
    CHECK(!as_alloc(&a, kUserBase, 1, PF_WRITE));                       // double map refused

    // copies crossing page boundaries
    char src[6000], dst[6000];
    for (size_t i = 0; i < sizeof src; ++i) src[i] = (char)(i * 7 + 1);
    CHECK(as_copy_to(&a, kUserBase + 4000, src, sizeof src));
    CHECK(as_copy_from(&a, dst, kUserBase + 4000, sizeof dst) && memcmp(src, dst, sizeof src) == 0);
    CHECK(!as_copy_to(&a, kUserBase + 3 * 4096 - 10, src, 20));          // runs off the mapping
    CHECK(!as_copy_to(&a, 0x1000, src, 4) && !as_copy_from(&a, dst, 0xFFFF800000000000ULL, 4));   // kernel addresses refused
    CHECK(!as_copy_to(&a, ~0ULL - 2, src, 8));                          // overflow in the range check

    // read-only mapping: user copy refuses writes, the loader (force) may fill it
    CHECK(as_alloc(&a, kUserBase + 0x100000, 1, PF_NX));
    CHECK(!as_copy_to(&a, kUserBase + 0x100000, "x", 1));
    CHECK(as_copy_to(&a, kUserBase + 0x100000, "x", 1, true));

    // strings
    CHECK(as_copy_to(&a, kUserBase, "hello\0", 6));
    char s[16]; CHECK(as_copy_string(&a, s, kUserBase, sizeof s) && strcmp(s, "hello") == 0);
    memset(dst, 'A', 4096); CHECK(as_copy_to(&a, kUserBase, dst, 4096));
    CHECK(!as_copy_string(&a, s, kUserBase, sizeof s));                 // unterminated within max

    // frames handed to as_map are owned by the address space
    CHECK(as_alloc(&b, kUserBase + (5ULL << 30), 2, PF_WRITE));          // a different 1 GiB slot: new PD/PT chain
    CHECK(b.pages == 2);

    // kernel slot propagation (a new top-level kernel entry shows up in live address spaces)
    master[300] = 0x4003; sync_kernel_slot(300);
    CHECK(((uint64_t*)(uintptr_t)a.pml4)[300] == 0x4003 && ((uint64_t*)(uintptr_t)b.pml4)[300] == 0x4003);
    master[5] = 0x5003; sync_kernel_slot(5);                             // user-range slots are never shared
    CHECK(((uint64_t*)(uintptr_t)a.pml4)[5] == 0);

    as_destroy(&a); as_destroy(&b);
    CHECK(a.pml4 == 0 && b.pml4 == 0);
    CHECK(free_frames == base_free);                                    // every page and table frame came back
    // destroyed spaces no longer receive kernel updates
    master[301] = 0x6003; sync_kernel_slot(301);
    pmm_free_frame((uint64_t)(uintptr_t)master);
}

int main(int argc, char** argv) {
    pool = (uint8_t*)mmap(nullptr, kFrames * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (pool == MAP_FAILED) { perror("mmap"); return 2; }
    used.assign(kFrames, false); free_frames = kFrames;
    test_policy();
    test_apic();
    test_elf(argc > 1 ? argv[1] : "../../build/hh0_smp0_apic1/user");
    test_aspace();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
