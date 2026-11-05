/* SPDX-License-Identifier: MIT */
// apic.hpp - ACPI MADT parsing, Local APIC (xAPIC + x2APIC), I/O APIC routing and the LAPIC timer.
//
// apic::init() is all-or-nothing: if anything looks wrong (no MADT, no I/O APIC, timer does not tick) the
// legacy 8259 + PIT setup that interrupt_init() created stays in charge and init() returns false.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::apic {

constexpr unsigned kMaxCpus = 32, kMaxIoApics = 8, kMaxIso = 24;

struct MadtCpu   { uint32_t acpi_id; uint32_t apic_id; bool enabled; };
struct MadtIoApic{ uint8_t id; uint32_t addr; uint32_t gsi_base; };
struct MadtIso   { uint8_t bus; uint8_t source; uint32_t gsi; uint16_t flags; };

struct MadtInfo {
    uint64_t lapic_addr;
    bool     pcat_compat;
    unsigned cpu_count;
    unsigned ioapic_count;
    unsigned iso_count;
    MadtCpu    cpus[kMaxCpus];
    MadtIoApic ioapics[kMaxIoApics];
    MadtIso    isos[kMaxIso];
};

struct IrqRoute { uint32_t gsi; bool active_low; bool level; };

// Pure helpers (also compiled into the host test).
bool parse_madt(const uint8_t* table, size_t length, MadtInfo* out);
IrqRoute resolve_isa_irq(const MadtInfo& info, uint8_t isa_irq);
uint32_t ioapic_entry_low(uint8_t vector, const IrqRoute& r, bool masked);
const uint8_t* find_table(const uint8_t* mbi, const char* signature, size_t* length);

// Kernel interface.
bool init(const uint8_t* multiboot_info);
bool active();             // true once the LAPIC timer + IOAPIC are driving interrupts
void eoi();                // acknowledge the current LAPIC interrupt
uint32_t lapic_id();
unsigned cpu_count();      // CPUs listed (and enabled) in the MADT, 1 when APIC mode is off
unsigned timer_hz();
uint64_t lapic_irq_count();
const MadtInfo* madt();
void describe(void (*print)(const char* text));

// AP support (SMP=1).
void enable_this_cpu();                       // enable LAPIC + start the scheduler tick on the calling CPU
bool send_init_sipi(uint32_t apic_id, uint8_t startup_page);
void udelay_ms(unsigned ms);
}  // namespace aos::apic
