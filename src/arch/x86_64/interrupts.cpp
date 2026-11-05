/* SPDX-License-Identifier: MIT */
#include "interrupts.hpp"
#include "frame.hpp"
#include "percpu.hpp"
#include "apic.hpp"
#include "../../drivers/ps2_mouse.hpp"
#include "../../scheduler/task.hpp"
#include "../../process/process.hpp"
#include "../../process/syscall.hpp"
#include "../../memory/virtual/aspace.hpp"

extern "C" void (*isr_stub_table[256])();

namespace {
struct __attribute__((packed)) IdtGate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
};
struct __attribute__((packed)) IdtPointer {
    uint16_t limit;
    uint64_t base;
};
extern "C" void aos_serial_write(const char* text);
extern "C" void aos_serial_write_hex64(uint64_t value);
IdtGate idt[256]{};
volatile uint64_t ticks = 0;
volatile uint8_t key_queue[128]{};
volatile uint8_t key_head = 0, key_tail = 0;
bool left_shift_down = false;
bool right_shift_down = false;
bool caps_lock = false;
bool caps_key_down = false;
bool e0_prefix = false;
bool ctrl_down = false;
bool kclick = false;              // Ctrl+Enter held = left mouse button
// Who delivers PS/2 bytes on this machine: 0 = unknown yet, 1 = IRQ, 2 = polling.
// Exactly one path may consume bytes, otherwise every key shows up twice.
volatile int kbd_mode = 0, aux_mode = 0;

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
inline uint8_t in8(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void wait_io() { for (volatile unsigned i = 0; i < 10000; ++i) asm volatile("pause"); }
void set_gate(uint8_t vector, uintptr_t address, uint8_t ist = 0, uint8_t dpl = 0) {
    idt[vector].offset_low = address & 0xffff;
    idt[vector].selector = 0x08;
    idt[vector].ist = ist;
    idt[vector].attributes = static_cast<uint8_t>(0x8e | (dpl << 5));
    idt[vector].offset_middle = (address >> 16) & 0xffff;
    idt[vector].offset_high = (address >> 32) & 0xffffffff;
    idt[vector].reserved = 0;
}
char translate_scancode(uint8_t scan, bool shift) {
    static const char normal[128] = {
        0, 0, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
        'd','f','g','h','j','k','l',';','\'', '`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[128] = {
        0, 0, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
        'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (scan >= 128) return 0;
    const bool is_letter = (scan >= 0x10 && scan <= 0x19) ||
                           (scan >= 0x1e && scan <= 0x26) ||
                           (scan >= 0x2c && scan <= 0x32);
    if (ctrl_down && is_letter) return static_cast<char>(normal[scan] & 0x1f);   // Ctrl+A..Z -> 1..26
    if (is_letter) shift = shift != caps_lock;
    return shift ? shifted[scan] : normal[scan];
}
void enqueue_key(char value) {
    if (!value) return;
    const uint8_t next = (key_head + 1) & 127;
    if (next == key_tail) return;
    key_queue[key_head] = static_cast<uint8_t>(value);
    key_head = next;
}
void pic_eoi(uint8_t irq) {
    if (irq >= 8) out8(0xa0, 0x20);
    out8(0x20, 0x20);
}
// End of interrupt for whichever controller is currently delivering it.
void irq_eoi(uint8_t irq) {
    if (aos::apic::active()) aos::apic::eoi();
    else pic_eoi(irq);
}
}

// keyboard scancode state machine (shared by the IRQ handler and the polling fallback)
void kbd_scan(uint8_t scan) {
        if (scan == 0xe0) { e0_prefix = true; return; }
        if (e0_prefix) {   // extended keys -> codes 0x81.. (see gui/input.hpp)
            e0_prefix = false;
            if (scan == 0x1d) ctrl_down = true;
            else if (scan == 0x9d) ctrl_down = false;
            else if (ctrl_down && (scan == 0x48 || scan == 0x50 || scan == 0x4b || scan == 0x4d)) {
                // Ctrl+Arrows = move the mouse pointer (hold for repeat)
                mouse_inject(scan == 0x4b ? -18 : (scan == 0x4d ? 18 : 0), scan == 0x48 ? 18 : (scan == 0x50 ? -18 : 0), -1);
            }
            else if (!(scan & 0x80)) {
                char k = 0;
                switch (scan) { case 0x48: k = static_cast<char>(0x81); break; case 0x50: k = static_cast<char>(0x82); break;
                                case 0x4b: k = static_cast<char>(0x83); break; case 0x4d: k = static_cast<char>(0x84); break;
                                case 0x47: k = static_cast<char>(0x85); break; case 0x4f: k = static_cast<char>(0x86); break;
                                case 0x53: k = static_cast<char>(0x87); break; case 0x49: k = static_cast<char>(0x88); break;
                                case 0x51: k = static_cast<char>(0x89); break; }
                enqueue_key(k);
            }
            return;
        }
        if (scan == 0x1d) ctrl_down = true;
        else if (scan == 0x9d) ctrl_down = false;
        else if (scan == 0x1c && ctrl_down) { if (!kclick) { kclick = true; mouse_inject(0, 0, 1); } }   // Ctrl+Enter = click
        else if (scan == 0x9c && kclick) { kclick = false; mouse_inject(0, 0, 0); }
        else if (scan == 0x3c) enqueue_key(static_cast<char>(0x8A));   // F2 = terminal
        else if (scan == 0x3d) enqueue_key(static_cast<char>(0x8B));   // F3 = notepad
        else if (scan == 0x3e) enqueue_key(static_cast<char>(0x8C));   // F4 = files
        else if (scan == 0x3f) enqueue_key(static_cast<char>(0x8D));   // F5 = control panel
        else if (scan == 0x2a) left_shift_down = true;
        else if (scan == 0x36) right_shift_down = true;
        else if (scan == 0xaa) left_shift_down = false;
        else if (scan == 0xb6) right_shift_down = false;
        else if (scan == 0x3a) { if (!caps_key_down) caps_lock = !caps_lock; caps_key_down = true; }
        else if (scan == 0xba) caps_key_down = false;
        else if (!(scan & 0x80)) enqueue_key(translate_scancode(scan, left_shift_down || right_shift_down));
        return;
}


extern "C" void interrupt_dispatch(InterruptFrame* frame) {
    const uint64_t vector = frame->vector;
    if (vector < 32 && (frame->cs & 3) == 3) {
        // User-mode exception.  For #PF try demand-paging / COW first.
        if (vector == 14) {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            aos::process::Process* p = aos::process::current();
            if (p && aos::vmm::handle_user_page_fault(&p->as, cr2, frame->error)) {
                // Fault handled; resume the user program.
                return;
            }
        }
        // Real fault or unhandled #PF → kill the process, kernel continues.
        asm volatile("sti");
        aos::process::user_fault(frame);
    }
    if (vector < 32) {
        static const char* const names[32] = {
            "DIVIDE ERROR", "DEBUG", "NON-MASKABLE INTERRUPT", "BREAKPOINT",
            "OVERFLOW", "BOUND RANGE", "INVALID OPCODE", "DEVICE NOT AVAILABLE",
            "DOUBLE FAULT", "COPROCESSOR OVERRUN", "INVALID TSS", "SEGMENT NOT PRESENT",
            "STACK SEGMENT FAULT", "GENERAL PROTECTION FAULT", "PAGE FAULT", "RESERVED",
            "X87 FLOATING POINT", "ALIGNMENT CHECK", "MACHINE CHECK", "SIMD FLOATING POINT",
            "VIRTUALIZATION", "CONTROL PROTECTION", "RESERVED", "RESERVED",
            "RESERVED", "RESERVED", "RESERVED", "RESERVED",
            "HYPERVISOR INJECTION", "VMM COMMUNICATION", "SECURITY EXCEPTION", "RESERVED"
        };
        aos_serial_write("\r\nAOS KERNEL PANIC: ");
        aos_serial_write(names[vector]);
        aos_serial_write(" (vector "); aos_serial_write_hex64(vector);
        aos_serial_write(", error "); aos_serial_write_hex64(frame->error);
        aos_serial_write(")\r\nRIP="); aos_serial_write_hex64(frame->rip);
        aos_serial_write(" CS="); aos_serial_write_hex64(frame->cs);
        aos_serial_write(" RSP="); aos_serial_write_hex64(frame->rsp);
        if (aos::arch::cpu_count()) { aos_serial_write(" CPU="); aos_serial_write_hex64(aos::arch::percpu()->cpu_id); }
        if (vector == 14) {
            uint64_t fault_address;
            asm volatile("mov %%cr2, %0" : "=r"(fault_address));
            aos_serial_write(" CR2="); aos_serial_write_hex64(fault_address);
        }
        aos_serial_write("\r\nSystem halted.\r\n");
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    }
    if (vector == 32) {
        const bool bsp = aos::arch::percpu()->cpu_id == 0;
        if (bsp) ++ticks;                 // the BSP's timer is the kernel's clock
        irq_eoi(0);
        aos::scheduler::on_tick();        // may switch to another task; resumes here later
        if ((frame->cs & 3) == 3) aos::process::check_kill();
        return;
    }
    if (vector == 0x80) {                 // system call (SYSCALL instruction or int 0x80)
        if (frame->rflags & 0x200) asm volatile("sti");
        aos::syscall::dispatch(frame);
        return;
    }
    if (vector == 0xFF) return;           // LAPIC spurious: no EOI
    if (vector == 33) {
        if (kbd_mode != 2) {
            const uint8_t st = in8(0x64);
            if (st & 1) {                       // OBF set = a real byte is waiting (else: spurious / already polled)
                const uint8_t v = in8(0x60);
                if (st & 0x20) { aux_mode = aux_mode ? aux_mode : 1; mouse_irq_byte(v); }
                else { kbd_mode = 1; kbd_scan(v); }
            }
        }
        irq_eoi(1);
        return;
    }
    if (vector == 44) {            // PS/2 mouse: one byte of a 3-byte packet
        if (aux_mode != 2) {
            const uint8_t st = in8(0x64);
            if (st & 1) { const uint8_t v = in8(0x60); aux_mode = 1; mouse_irq_byte(v); }
        }
        irq_eoi(12);
        return;
    }
    if (vector >= 40 && vector <= 47) irq_eoi(static_cast<uint8_t>(vector - 32));
    else if (vector >= 32 && vector <= 39) irq_eoi(static_cast<uint8_t>(vector - 32));
    else if (aos::apic::active()) aos::apic::eoi();     // unexpected LAPIC/IOAPIC vector: do not wedge the ISR
}

void interrupt_init() {
    for (unsigned i = 0; i < 256; ++i)
        set_gate(static_cast<uint8_t>(i), reinterpret_cast<uintptr_t>(isr_stub_table[i]));
    set_gate(8,  reinterpret_cast<uintptr_t>(isr_stub_table[8]),  1);        // #DF on its own stack (IST1)
    set_gate(2,  reinterpret_cast<uintptr_t>(isr_stub_table[2]),  2);        // NMI (IST2)
    set_gate(18, reinterpret_cast<uintptr_t>(isr_stub_table[18]), 3);        // #MC (IST3)
    set_gate(0x80, reinterpret_cast<uintptr_t>(isr_stub_table[0x80]), 0, 3); // int 0x80 callable from ring 3
    interrupt_load_idt();

    // Remap both 8259 PICs to vectors 32-47; timer, keyboard, cascade and mouse are enabled.
    out8(0x20, 0x11); wait_io(); out8(0xa0, 0x11); wait_io();
    out8(0x21, 0x20); wait_io(); out8(0xa1, 0x28); wait_io();
    out8(0x21, 0x04); wait_io(); out8(0xa1, 0x02); wait_io();
    out8(0x21, 0x01); wait_io(); out8(0xa1, 0x01); wait_io();
    out8(0x21, 0xf8); out8(0xa1, 0xef);

    // PIT channel 0 at ~90 Hz.
    // 1,193,182 / 13258 ≈ 90.0 Hz
    constexpr uint16_t divisor = 13258;
    out8(0x43, 0x36);
    out8(0x40, divisor & 0xff);
    out8(0x40, divisor >> 8);
    asm volatile("sti");
}

void interrupt_load_idt() {
    IdtPointer pointer{static_cast<uint16_t>(sizeof(idt) - 1), reinterpret_cast<uint64_t>(idt)};
    asm volatile("lidt %0" : : "m"(pointer));
}

uint64_t timer_ticks() { return ticks; }
char keyboard_read_char() {
    if (key_tail == key_head) return 0;
    const char result = static_cast<char>(key_queue[key_tail]);
    key_tail = (key_tail + 1) & 127;
    return result;
}

// Polling fallback: on many laptops IRQ1/IRQ12 never reach the CPU (IOAPIC routing), but the i8042
// output buffer still fills. Drain it from the main loop. cli keeps the IRQ handler from racing us.
void ps2_poll() {
    for (int guard = 0; guard < 64; ++guard) {
        uint64_t f; asm volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        const uint8_t st = in8(0x64);
        bool got = false;
        if ((st & 1) && st != 0xFF) {
            const bool aux = (st & 0x20) != 0;
            if (aux ? aux_mode != 1 : kbd_mode != 1) {   // leave bytes to the IRQ path if IRQs work
                const uint8_t v = in8(0x60);
                if (aux) { aux_mode = 2; mouse_irq_byte(v); } else { kbd_mode = 2; kbd_scan(v); }
                got = true;
            }
        }
        if (f & 0x200) asm volatile("sti" ::: "memory");
        if (!got) break;
    }
}

bool input_pending() { return key_tail != key_head || mouse_pending(); }
