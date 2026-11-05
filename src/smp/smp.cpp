#include "smp.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../arch/x86_64/interrupts.hpp"
#include "../arch/x86_64/percpu.hpp"
#include "../memory/physical/pmm.h"
#include "../memory/virtual/vmm.hpp"
#include "../scheduler/task.hpp"

extern "C" void aos_serial_write(const char* text);
extern "C" char __ap_tramp_load[];
extern "C" char __ap_tramp_size[];     // absolute symbol: its address is the size
extern "C" char ap_params[];           // address inside the trampoline image (link address 0x8000)

namespace aos::smp {
namespace {
constexpr uint64_t kTrampolinePhys = 0x8000;
volatile unsigned g_started = 0;

struct Params { uint64_t cr3, stack, entry, percpu; };

extern "C" [[noreturn]] void ap_main(aos::arch::PerCpu* c) {
    aos::arch::init_cpu(c);                 // GDT/TSS/GS/SYSCALL MSRs for this CPU
    interrupt_load_idt();
    aos::apic::enable_this_cpu();           // LAPIC + scheduler tick
    aos::scheduler::adopt_ap(c->cpu_id);    // this boot context becomes the CPU's idle thread
    __atomic_add_fetch(&g_started, 1, __ATOMIC_SEQ_CST);
    aos::scheduler::ap_idle_loop();
}
}  // namespace

unsigned online() { return 1 + g_started; }

void start(const uint8_t*) {
    const aos::apic::MadtInfo* m = aos::apic::madt();
    if (!aos::apic::active() || !m) { aos_serial_write("[smp] needs APIC mode, staying on one CPU\r\n"); return; }
    const uint8_t* src = reinterpret_cast<const uint8_t*>(__ap_tramp_load);
    const size_t size = reinterpret_cast<uintptr_t>(__ap_tramp_size);
    uint8_t* dst = reinterpret_cast<uint8_t*>(kTrampolinePhys);
    for (size_t i = 0; i < size; ++i) dst[i] = src[i];
    Params* pp = reinterpret_cast<Params*>(kTrampolinePhys + (reinterpret_cast<uintptr_t>(ap_params) - kTrampolinePhys));
    const uint32_t bsp = aos::apic::lapic_id();
    unsigned launched = 0;
    for (unsigned i = 0; i < m->cpu_count; ++i) {
        if (!m->cpus[i].enabled || m->cpus[i].apic_id == bsp) continue;
        aos::arch::PerCpu* c = aos::arch::alloc_percpu(aos::arch::cpu_count(), m->cpus[i].apic_id);
        const uint64_t stack = pmm_alloc_contiguous(4);
        if (!c || !stack) break;
        c->kernel_rsp = stack + 4 * 4096 - 16;
        pp->cr3 = aos::vmm::kernel_cr3();
        pp->stack = c->kernel_rsp;
        pp->entry = reinterpret_cast<uint64_t>(&ap_main);
        pp->percpu = reinterpret_cast<uint64_t>(c);
        c->timer_count = 0;
        const unsigned before = g_started;
        aos::apic::send_init_sipi(m->cpus[i].apic_id, static_cast<uint8_t>(kTrampolinePhys >> 12));
        for (unsigned t = 0; t < 200 && g_started == before; ++t) aos::apic::udelay_ms(1);
        if (g_started == before) aos_serial_write("[smp] an application processor did not respond\r\n");
        else ++launched;
    }
    aos_serial_write("[smp] application processors started: ");
    char d[3] = {static_cast<char>('0' + launched / 10), static_cast<char>('0' + launched % 10), 0};
    aos_serial_write(d);
    aos_serial_write("\r\n");
}
}  // namespace aos::smp
