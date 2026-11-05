/* SPDX-License-Identifier: MIT */
#include "../drivers/audio/hda.hpp"
#include "../drivers/backlight.hpp"
#include <stdint.h>
#include <stddef.h>
#include "../arch/x86_64/interrupts.hpp"
#include "../arch/x86_64/percpu.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../process/process.hpp"
#ifdef AOS_SMP
#include "../smp/smp.hpp"
#endif
#include "../memory/physical/pmm.h"
#include "../memory/heap/kheap.hpp"
#include "../memory/virtual/vmm.hpp"
#include "../scheduler/task.hpp"
#include "../filesystem/initramfs.hpp"
#include "../filesystem/ramfs.hpp"
#include "../drivers/ps2_mouse.hpp"
#include "../drivers/usb/xhci.hpp"
#include "../drivers/i2c/i2c_hid.hpp"
#include "../drivers/storage-drivers/ata_pio.hpp"
#include "../drivers/storage-drivers/block_device.hpp"
#include "../drivers/storage-drivers/nvme.hpp"
#include "../drivers/storage-drivers/virtio_blk.hpp"
#include "../drivers/storage-drivers/ahci.hpp"
#include "../cpu/cpu.h"
#include "../safety/watchdog.h"
#include "../safety/canary.h"
#include "../safety/kinfo.h"
#include "../network/net.h"
#include "../shell/shell.hpp"

extern "C" {
    extern char _kernel_start;        // virtual addresses (higher half with HH=1)
    extern char _kernel_end;
    extern char _kernel_phys_start;   // physical addresses, what the PMM needs
    extern char _kernel_phys_end;
    void aos_serial_write(const char* text);

    void* memset(void* s, int c, size_t n) {
        void* d = s;
        asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
        return s;
    }
    void* memcpy(void* d, const void* s, size_t n) {
        void* r = d;
        asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        return r;
    }
    void* memmove(void* d, const void* s, size_t n) {
        unsigned char* dd = static_cast<unsigned char*>(d);
        const unsigned char* ss = static_cast<const unsigned char*>(s);
        if (dd < ss || dd >= ss + n) { while (n--) *dd++ = *ss++; }
        else { dd += n; ss += n; while (n--) *--dd = *--ss; }
        return d;
    }

    uintptr_t __stack_chk_guard = 0x595e9fbd94fda766ULL;
    [[noreturn]] void __stack_chk_fail() {
        aos_serial_write("CRITICAL: Stack Smashing Detected (__stack_chk_guard corrupted)!\r\n");
        for (;;) asm volatile("cli; hlt");
    }
}

namespace {
inline void out8(uint16_t port, uint8_t value) { asm volatile("outb %0, %1" :: "a"(value), "Nd"(port)); }
inline uint8_t in8(uint16_t port) { uint8_t value; asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port)); return value; }

void serial_init() {
    constexpr uint16_t p = 0x3F8;
    out8(p+1, 0); out8(p+3, 0x80); out8(p, 1); out8(p+1, 0); out8(p+3, 3); out8(p+2, 0xC7); out8(p+4, 0x0B);
}
void serial_write(const char* s) {
    static bool dead = false;   // no UART on this machine (most laptops): never spin
    while (*s) {
        if (!dead) {
            unsigned n = 0;
            while ((in8(0x3FD) & 0x20) == 0 && ++n < 20000) asm volatile("pause");
            if (n >= 20000 || in8(0x3FD) == 0xFF) dead = true;
            else out8(0x3F8, static_cast<uint8_t>(*s));
        }
        ++s;
    }
}
char serial_read_char() {
    const uint8_t lsr = in8(0x3FD);
    if (lsr == 0xFF || (lsr & 0x01) == 0) return 0;   // 0xFF = no UART present (floating bus)
    return static_cast<char>(in8(0x3F8));
}
void serial_write_dec(uint64_t v) {
    char buf[24]; int i = 0;
    if (v == 0) { serial_write("0"); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) { char c[2] = {buf[i], 0}; serial_write(c); }
}
void serial_write_hex(uint64_t v) {
    static const char* d = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 15; i >= 0; --i) {
        char c[2] = {d[(v >> (i*4)) & 0xF], 0};
        serial_write(c);
    }
}

void serial_sink_write(void*, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\r') continue;
        if (s[i] == '\n') serial_write("\r\n");
        else { char c[2] = {s[i], 0}; serial_write(c); }
    }
}
void serial_sink_clear(void*) { serial_write("\x1b[2J\x1b[H"); }
const aos::shell::Sink serial_sink = {serial_sink_write, serial_sink_clear, nullptr};

char linebuf[128];
size_t linepos = 0;

// Command history (last 16 lines). ESC [ A = up, ESC [ B = down.
constexpr unsigned kHistMax = 16;
char hist[kHistMax][128];
unsigned hist_count = 0;   // total stored (caps at kHistMax)
unsigned hist_slot = 0;    // next write index (ring)
int hist_view = -1;        // -1 = editing live line; 0.. = viewing history
char draft[128];           // saved live line when browsing history
unsigned esc_state = 0;    // 0=normal, 1=got ESC, 2=got ESC[

void hist_push(const char* line) {
    if (!line || !line[0]) return;
    unsigned i = 0;
    while (line[i] && i + 1 < sizeof(hist[0])) { hist[hist_slot][i] = line[i]; ++i; }
    hist[hist_slot][i] = 0;
    hist_slot = (hist_slot + 1) % kHistMax;
    if (hist_count < kHistMax) ++hist_count;
}

void line_redraw(const char* content) {
    // erase current input
    while (linepos > 0) { serial_write("\b \b"); --linepos; }
    linepos = 0;
    while (content[linepos] && linepos + 1 < sizeof(linebuf)) {
        linebuf[linepos] = content[linepos];
        char s[2] = {content[linepos], 0};
        serial_write(s);
        ++linepos;
    }
    linebuf[linepos] = 0;
}

void serial_prompt() {
    char p[64];
    aos::shell::prompt(p, sizeof(p));
    serial_write(p);
}

void serial_feed(char ch) {
    // ANSI escape parser for up/down arrows
    if (esc_state == 1) {
        esc_state = (ch == '[') ? 2 : 0;
        return;
    }
    if (esc_state == 2) {
        esc_state = 0;
        if (ch == 'A' || ch == 'B') {   // up / down
            if (!hist_count) return;
            if (hist_view < 0) {
                // save draft
                for (unsigned i = 0; i < linepos; ++i) draft[i] = linebuf[i];
                draft[linepos] = 0;
                hist_view = 0;
            }
            if (ch == 'A') {   // older
                if (hist_view + 1 < (int)hist_count) ++hist_view;
            } else {           // newer
                if (hist_view > 0) --hist_view;
                else { hist_view = -1; line_redraw(draft); return; }
            }
            // hist_view 1 = most recent
            unsigned idx = (hist_slot + kHistMax - hist_view) % kHistMax;
            line_redraw(hist[idx]);
            return;
        }
        return;
    }
    if (ch == 0x1b) { esc_state = 1; return; }

    if (ch == '\r' || ch == '\n') {
        serial_write("\r\n");
        linebuf[linepos] = 0;
        hist_push(linebuf);
        hist_view = -1;
        aos::shell::execute(linebuf, serial_sink);
        linepos = 0;
        serial_prompt();
        return;
    }
    if (ch == 0x7f || ch == '\b') {
        if (linepos > 0) { --linepos; serial_write("\b \b"); }
        hist_view = -1;
        return;
    }
    if (ch >= 32 && ch < 127 && linepos + 1 < sizeof(linebuf)) {
        linebuf[linepos++] = ch;
        char s[2] = {ch, 0};
        serial_write(s);
        hist_view = -1;
    }
}

struct Tag { uint32_t type, size; };
struct __attribute__((packed)) ModuleTag { uint32_t type, size, start, end; char command[1]; };
}  // namespace

extern "C" void aos_serial_write(const char* text) { serial_write(text); }
extern "C" void aos_serial_write_hex64(uint64_t value) { serial_write_hex(value); }

namespace {
uint64_t g_last_pet = 0, g_last_net = 0;

// Input from serial/keyboard goes to the foreground program if there is one, else to the shell.
void route_input(char ch) {
    const uint32_t fg = aos::process::foreground();
    if (fg && ch == 3) { aos::process::kill(fg); serial_write("^C\r\n"); return; }
    if (aos::process::stdin_wanted()) { aos::process::stdin_push(ch); return; }
    if (!fg) serial_feed(ch);
}

// Everything the main loop does between sleeps; also run while `exec` waits for a program.
void poll_all() {
    const uint64_t now = timer_ticks();
    if (now - g_last_pet > 90) {
        watchdog_pet(static_cast<uint32_t>(now));
        g_last_pet = now;
    }
    watchdog_check(static_cast<uint32_t>(now));

    if (g_netif.initialized && now != g_last_net) {
        net_poll();
        net_tick(static_cast<uint32_t>(now));
        g_last_net = now;
    }

    ps2_poll();
    for (char ch; (ch = serial_read_char()) != 0;) route_input(ch);
    for (char ch; (ch = keyboard_read_char()) != 0;) route_input(ch);
    aos::usb::poll();
    aos::audio::poll();
    aos::i2c::poll();
}
}  // namespace

extern "C" [[noreturn]] void kernel_main(uint32_t magic, uint32_t info) {
    serial_init();
    serial_write("\r\n========================================\r\n");
    serial_write("  AOS Enhanced 64-bit (headless kernel)\r\n");
    serial_write("========================================\r\n");

    if (magic != 0x36d76289 || !info) {
        serial_write("ERROR: no Multiboot2 information\r\n");
        for (;;) asm volatile("hlt");
    }
    serial_write("[boot] Multiboot2 OK\r\n");

    auto* base = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(info));
    uint32_t total = *reinterpret_cast<uint32_t*>(base);
    bool initramfs_ready = false;

    for (uint32_t off = 8; off + sizeof(Tag) <= total;) {
        auto* tag = reinterpret_cast<Tag*>(base + off);
        if (tag->type == 0 || tag->size < 8) break;
        if (tag->type == 3 && tag->size >= sizeof(ModuleTag)) {
            auto* m = reinterpret_cast<ModuleTag*>(tag);
            if (m->end >= m->start)
                initramfs_ready = aos::filesystem::mount_initramfs(
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(m->start)), m->end - m->start);
        }
        off = (off + tag->size + 7) & ~7u;
    }

    if (!initramfs_ready) {
        serial_write("ERROR: initramfs missing\r\n");
        for (;;) asm volatile("cli; hlt");
    }
    aos::filesystem::FileView boot_note{};
    if (!aos::filesystem::find_file("README.TXT", &boot_note)) {
        serial_write("ERROR: initramfs invalid\r\n");
        for (;;) asm volatile("cli; hlt");
    }
    serial_write("[boot] Initramfs mounted\r\n");

    if (!pmm_init(base, reinterpret_cast<uint64_t>(&_kernel_phys_start), reinterpret_cast<uint64_t>(&_kernel_phys_end))) {
        serial_write("ERROR: PMM failed\r\n");
        for (;;) asm volatile("cli; hlt");
    }
    serial_write("[boot] PMM free frames: ");
    serial_write_dec(pmm_free_frames());
    serial_write("\r\n");

    aos::vmm::init();                                   // boot PML4 = master kernel page table
    if (!aos::arch::init_bsp()) {                       // per-CPU block, GDT + TSS (IST stacks), SYSCALL MSRs
        serial_write("ERROR: GDT/TSS\r\n");
        for (;;) asm volatile("cli; hlt");
    }
    if (!aos::heap::initialize() || !aos::scheduler::initialize()) {
        serial_write("ERROR: heap/scheduler\r\n");
        for (;;) asm volatile("cli; hlt");
    }
    aos::process::init();
    serial_write("[boot] GDT/TSS + Heap + preemptive Scheduler OK\r\n");

    cpu_init();
    serial_write("[boot] CPU: ");
    serial_write(g_cpu.vendor);
    serial_write(" | ");
    serial_write(g_cpu.brand[0] ? g_cpu.brand : "?");
    serial_write("\r\n");

    canary_init();
    watchdog_init(90 * 10);
    kinfo_init();
    serial_write("[boot] Safety OK\r\n");

    // Class drivers first (universal), legacy ATA last.
    if (aos::storage::virtio_blk::init()) {
        serial_write("[boot] virtio-blk disks: ");
        serial_write_dec(static_cast<uint64_t>(aos::storage::virtio_blk::device_count()));
        serial_write("\r\n");
    } else {
        serial_write("[boot] virtio-blk: none\r\n");
    }
    if (aos::storage::ahci::init()) {
        serial_write("[boot] AHCI disks: ");
        serial_write_dec(static_cast<uint64_t>(aos::storage::ahci::device_count()));
        serial_write("\r\n");
    } else {
        serial_write("[boot] AHCI: none\r\n");
    }
    if (aos::storage::nvme::init()) {
        serial_write("[boot] NVMe disks: ");
        serial_write_dec(static_cast<uint64_t>(aos::storage::nvme::namespace_count()));
        serial_write("\r\n");
    } else {
        serial_write("[boot] No NVMe disk\r\n");
    }
    if (aos::storage::ata_pio::initialize_primary_master())
        serial_write("[boot] ATA PIO OK (legacy fallback)\r\n");
    else
        serial_write("[boot] No ATA disk\r\n");

    aos::fs::init();
    if (aos::fs::is_persistent()) {
        serial_write("[boot] AOSFS mounted from "); serial_write(aos::fs::device_label()); serial_write("\r\n");
    } else {
        serial_write("[boot] NO STORAGE: nothing is saved. Run `disks` then `mkaosfs <disk> <part> CONFIRM`\r\n");
    }

    // Headless: fixed virtual screen size for mouse coordinate clamping only.
    mouse_init(800, 600);
    interrupt_init();
    serial_write("[boot] Interrupts OK (8259 + PIT)\r\n");
    if (aos::apic::init(base)) {
        serial_write("[boot] APIC: LAPIC timer + I/O APIC active, CPUs in MADT: ");
        serial_write_dec(aos::apic::cpu_count());
        serial_write("\r\n");
    } else {
        serial_write("[boot] APIC not used: legacy PIC/PIT interrupts\r\n");
    }
#ifdef AOS_SMP
    aos::smp::start(base);
#endif

    // USB (xHCI) mice/touchpads/tablets and I2C-HID touchpads. Both are polled from the main loop.
    if (aos::audio::init()) serial_write("[boot] audio: HDA output ready\r\n"); else serial_write("[boot] audio: none\r\n");
    aos::backlight::init();
    if (aos::usb::init()) {
        serial_write("[boot] USB pointer devices: "); serial_write_dec(static_cast<uint64_t>(aos::usb::pointer_count())); serial_write("\r\n");
    } else {
        serial_write("[boot] USB: no xHCI controller\r\n");
    }
    if (aos::i2c::init(base)) {
        serial_write("[boot] I2C-HID touchpad OK\r\n");
    } else {
        serial_write("[boot] I2C-HID: none found\r\n");
    }

    if (net_init()) {
        serial_write("[boot] Network OK (NIC found)\r\n");
        kinfo_register("net");
        kinfo_set(0, SUBSYS_OK);
    } else {
        serial_write("[boot] Network: no supported NIC\r\n");
    }

    serial_write("\r\nType 'help' for commands.\r\n");
    serial_prompt();

    aos::shell::set_input_pump(poll_all);

    for (;;) {
        const uint64_t now = timer_ticks();
        poll_all();
        aos::scheduler::yield();
        if (!input_pending()) aos::scheduler::idle_wait(now);   // hlt (or sleep a tick when other threads exist)
    }
}
